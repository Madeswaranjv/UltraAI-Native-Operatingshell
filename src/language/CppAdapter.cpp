  #include "CppAdapter.h"
  #include "../ai/SemanticExtractor.h"
  #include "../build/BuildEngine.h"
  #include "../core/ConfigManager.h"
  #include "../core/Logger.h"
  #include "../graph/DependencyGraph.h"
  #include "../hashing/HashManager.h"
  #include "../incremental/IncrementalAnalyzer.h"
  #include "../platform/IProcessExecutor.h"
  #include "../platform/WindowsProcessExecutor.h"
#if !defined(_WIN32)
  #include "../platform/UnixProcessExecutor.h"
#endif
  #include "context/ContextSnapshot.h"
  #include "../scanner/ProjectScanner.h"
  #include "../utils/PathUtils.h"
  #include "utils/FileClassifier.h"
  // NEW AST INCLUDES
  #include "../ai/FileRegistry.h"
  #include <algorithm>
  #include <chrono>
  #include <cstdio>
  #include <fstream>
  #include <iomanip>
  #include <iostream>
  #include <optional>
  #include <string>
  #include <system_error>
  #include <unordered_map>
  namespace ultra::language {
  namespace {
  struct PatchTargetSnapshot {
    std::filesystem::path relativePath;
    std::filesystem::path absolutePath;
    bool existedBefore{false};
    bool existedAfter{false};
    std::string beforeContent;
    std::string afterContent;
  };
  std::optional<std::string> resolveInclude(
      const std::string& includeName,
      const std::filesystem::path& fromPath,
      const std::unordered_map<std::string, std::vector<std::string>>&
          filenameToPaths) {
    auto it = filenameToPaths.find(includeName);
    if (it == filenameToPaths.end()) return std::nullopt;
    std::filesystem::path fromDir = fromPath.parent_path();
    std::string sameDir = (fromDir / includeName).lexically_normal().string();
    for (const std::string& candidate : it->second) {
      if (candidate == sameDir) return candidate;
    }
    return it->second.front();
  }
  std::string quoteForShell(const std::string& raw) {
    std::string escaped;
    escaped.reserve(raw.size() + 2U);
    escaped.push_back('"');
    for (const char ch : raw) {
      if (ch == '"') {
        escaped += "\\\"";
        continue;
      }
      escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
  }
  std::string buildCommandInWorkingDirectory(
      const std::filesystem::path& cwd,
      const std::string& command) {
#if defined(_WIN32)
    return "cd /d " + quoteForShell(cwd.string()) + " && " + command;
#else
    return "cd " + quoteForShell(cwd.string()) + " && " + command;
#endif
  }
  std::string combineProcessOutput(
      const ultra::platform::ProcessResult& result) {
    std::string combined = result.stdOut;
    if (!result.stdErr.empty()) {
      if (!combined.empty() && combined.back() != '\n') {
        combined.push_back('\n');
      }
      combined += result.stdErr;
    }
    return combined;
  }
  std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
      return {};
    }

    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    return content;
  }
  std::optional<std::filesystem::path> normalizeDiffPathToken(
      const std::string& token) {
    if (token.empty() || token == "/dev/null") {
      return std::nullopt;
    }

    std::filesystem::path path(token);
    const std::string generic = path.generic_string();
    if (generic.size() > 2U &&
        (generic.rfind("a/", 0U) == 0U || generic.rfind("b/", 0U) == 0U)) {
      return std::filesystem::path(generic.substr(2U)).lexically_normal();
    }
    return path.lexically_normal();
  }
  std::vector<std::filesystem::path> parsePatchTargets(
      const std::filesystem::path& diffFile) {
    std::vector<std::filesystem::path> targets;
    std::ifstream input(diffFile);
    if (!input.is_open()) {
      return targets;
    }

    std::optional<std::filesystem::path> beforePath;
    std::string line;
    while (std::getline(input, line)) {
      if (line.rfind("--- ", 0U) == 0U) {
        beforePath = normalizeDiffPathToken(line.substr(4U));
        continue;
      }
      if (line.rfind("+++ ", 0U) != 0U) {
        continue;
      }

      const std::optional<std::filesystem::path> afterPath =
          normalizeDiffPathToken(line.substr(4U));
      const std::optional<std::filesystem::path> chosenPath =
          afterPath.has_value() ? afterPath : beforePath;
      beforePath.reset();
      if (!chosenPath.has_value()) {
        continue;
      }

      const std::filesystem::path normalized = chosenPath->lexically_normal();
      const auto duplicate = std::find(targets.begin(), targets.end(), normalized);
      if (duplicate == targets.end()) {
        targets.push_back(normalized);
      }
    }

    return targets;
  }
  std::vector<PatchTargetSnapshot> snapshotPatchTargets(
      const std::filesystem::path& root,
      const std::vector<std::filesystem::path>& relativePaths) {
    std::vector<PatchTargetSnapshot> snapshots;
    snapshots.reserve(relativePaths.size());
    for (const std::filesystem::path& relativePath : relativePaths) {
      PatchTargetSnapshot snapshot;
      snapshot.relativePath = relativePath.lexically_normal();
      snapshot.absolutePath = (root / snapshot.relativePath).lexically_normal();

      std::error_code ec;
      snapshot.existedBefore = std::filesystem::exists(snapshot.absolutePath, ec) &&
                               !ec;
      if (snapshot.existedBefore &&
          std::filesystem::is_regular_file(snapshot.absolutePath, ec) && !ec) {
        snapshot.beforeContent = readTextFile(snapshot.absolutePath);
      }
      snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
  }
  std::vector<std::string> refreshPatchVerification(
      std::vector<PatchTargetSnapshot>& snapshots) {
    std::vector<std::string> changedPaths;
    for (PatchTargetSnapshot& snapshot : snapshots) {
      std::error_code ec;
      snapshot.existedAfter = std::filesystem::exists(snapshot.absolutePath, ec) && !ec;
      snapshot.afterContent.clear();
      if (snapshot.existedAfter &&
          std::filesystem::is_regular_file(snapshot.absolutePath, ec) && !ec) {
        snapshot.afterContent = readTextFile(snapshot.absolutePath);
      }

      if (snapshot.existedBefore != snapshot.existedAfter ||
          snapshot.beforeContent != snapshot.afterContent) {
        changedPaths.push_back(snapshot.relativePath.generic_string());
      }
    }
    return changedPaths;
  }
  bool containsSkippedPatchMarker(const std::string& output) {
    return output.find("Skipped patch") != std::string::npos;
  }
  ultra::platform::ProcessResult executeCapturedCommand(
      ultra::platform::IProcessExecutor& executor,
      const std::string& command) {
    std::error_code ec;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path stdoutPath =
        ec ? std::filesystem::path("ultra_patch_stdout.log")
           : tempDir / ("ultra_patch_stdout_" + std::to_string(suffix) + ".log");
    const std::filesystem::path stderrPath =
        ec ? std::filesystem::path("ultra_patch_stderr.log")
           : tempDir / ("ultra_patch_stderr_" + std::to_string(suffix) + ".log");

    const std::string redirected =
        command + " > " + quoteForShell(stdoutPath.string()) + " 2> " +
        quoteForShell(stderrPath.string());

#if defined(_WIN32)
    ultra::platform::ProcessResult result = executor.execute(redirected);
#else
    ultra::platform::ProcessResult result = executor.execute(redirected);
#endif
    result.stdOut = readTextFile(stdoutPath);
    result.stdErr = readTextFile(stderrPath);

    std::error_code removeError;
    std::filesystem::remove(stdoutPath, removeError);
    removeError.clear();
    std::filesystem::remove(stderrPath, removeError);
    return result;
  }
  void printAnalyzeSummary(std::size_t totalScanned,
                          const std::vector<std::string>& changed,
                          const std::vector<std::string>& rebuildSet) {
    std::cout << "Incremental Analysis\n\n";
    std::cout << "Total files scanned: " << totalScanned << '\n';
    std::cout << "Changed files: " << changed.size() << '\n';
    if (changed.empty()) {
      std::cout << "\nNo changes detected.\nBuild is up to date.\n";
      return;
    }
    std::cout << "\nChanged:\n\n";
    for (const std::string& pathStr : changed) {
      std::cout << std::filesystem::path(pathStr).filename().string() << '\n';
    }
    std::cout << "\nFiles requiring rebuild: " << rebuildSet.size() << '\n';
    std::cout << "\nRebuild set:\n\n";
    for (const std::string& pathStr : rebuildSet) {
      std::cout << std::filesystem::path(pathStr).filename().string() << '\n';
    }
  }
  } // namespace
  std::vector<ultra::scanner::FileInfo> CppAdapter::scan(
      const std::filesystem::path& root) {
    std::filesystem::path configPath = root / "ultra.json";
    ultra::core::ConfigManager config(configPath);
    ultra::scanner::ProjectScanner scanner(config);
    return scanner.scan(root);
  }
  ultra::graph::DependencyGraph CppAdapter::buildGraph(
      const std::vector<ultra::scanner::FileInfo>& files) {
    std::unordered_map<std::string, std::vector<std::string>> filenameToPaths;
    ultra::graph::DependencyGraph graph;
    struct FileLang {
      const ultra::scanner::FileInfo* file{nullptr};
      ultra::ai::Language lang{ultra::ai::Language::Unknown};
      std::string key;
    };
    std::vector<FileLang> parsableFiles;
    for (const auto& f : files) {
      const ultra::ai::Language lang = ultra::ai::FileRegistry::detectLanguage(f.path);
      if (lang == ultra::ai::Language::Unknown) {
        continue;
      }
      const std::string key = f.path.lexically_normal().string();
      parsableFiles.push_back({&f, lang, key});
      graph.addNode(key);
      filenameToPaths[f.path.filename().string()].push_back(key);
    }
    for (const FileLang& entry : parsableFiles) {
      ultra::ai::SemanticParseResult semantic;
      std::string parseError;
      if (!ultra::ai::SemanticExtractor::extract(entry.file->path, entry.lang, semantic,
                                          parseError)) {
        ultra::core::Logger::warning(
            "Tree-sitter parse failed for " + entry.file->path.string() +
            ": " + parseError);
        continue;
      }
      for (const std::string& reference : semantic.dependencyReferences) {
        const std::optional<std::string> toKey =
            resolveInclude(reference, entry.file->path, filenameToPaths);
        if (toKey.has_value()) {
          graph.addEdge(entry.key, toKey.value());
        }
      }
    }
    return graph;
  }
  void CppAdapter::analyze(const std::filesystem::path& root) {
    ultra::core::Logger::info(ultra::core::LogCategory::Incremental,
                              "Scanning project...");
    std::vector<ultra::scanner::FileInfo> files = scan(root);
    ultra::graph::DependencyGraph graph = buildGraph(files);
    std::filesystem::path dbPath = root / ".ultra.db";
    ultra::hashing::HashManager hashManager(dbPath);
    hashManager.load();
    std::vector<std::string> changed =
        hashManager.detectChanges(files, graph);
    std::vector<std::string> rebuildSet =
        ultra::incremental::IncrementalAnalyzer::computeRebuildSet(
            changed, graph);
    printAnalyzeSummary(files.size(), changed, rebuildSet);
    hashManager.save();
  }
  void CppAdapter::build(const std::filesystem::path& root) {
    ultra::build::BuildEngine engine(
        std::make_unique<ultra::platform::WindowsProcessExecutor>());
    lastBuildExitCode_ = engine.fullBuild(root);
  }
  void CppAdapter::buildIncremental(const std::filesystem::path& root) {
    ultra::core::Logger::info(ultra::core::LogCategory::Incremental,
                              "Scanning project...");
    std::vector<ultra::scanner::FileInfo> files = scan(root);
    ultra::graph::DependencyGraph graph = buildGraph(files);
    std::filesystem::path dbPath = root / ".ultra.db";
    ultra::hashing::HashManager hashManager(dbPath);
    hashManager.load();
    std::vector<std::string> changed =
        hashManager.detectChanges(files, graph);
    std::vector<std::string> rebuildSet =
        ultra::incremental::IncrementalAnalyzer::computeRebuildSet(
            changed, graph);
    hashManager.save();
    if (changed.empty()) {
      std::cout << "Build is up to date.\n";
      lastBuildExitCode_ = 0;
      return;
    }
    std::cout << "Rebuild set (" << rebuildSet.size() << " files):\n";
    for (const std::string& p : rebuildSet) {
      std::cout << "  "
                << std::filesystem::path(p).filename().string() << '\n';
    }
    std::cout << '\n';
    ultra::build::BuildEngine engine(
        std::make_unique<ultra::platform::WindowsProcessExecutor>());
    lastBuildExitCode_ =
        engine.incrementalBuild(root, rebuildSet);
  } 
  void CppAdapter::buildFast(const std::filesystem::path& root) {
      buildIncremental(root);
  }
  nlohmann::json CppAdapter::generateContext(
      const std::filesystem::path& root) {
      (void)root;  // prevent unused parameter warning
      return {};
  }
  nlohmann::json CppAdapter::generateContextWithAst(
      const std::filesystem::path& root) {
    const std::vector<ultra::scanner::FileInfo> files = scan(root);
    nlohmann::json result;
    result["project"] = root.filename().string();
    result["pipeline"] = "semantic-extractor-tree-sitter";
    result["files"] = nlohmann::json::array();
    result["errors"] = nlohmann::json::array();
    for (const auto& file : files) {
      const ultra::ai::Language lang = ultra::ai::FileRegistry::detectLanguage(file.path);
      if (lang == ultra::ai::Language::Unknown) {
        continue;
      }
      ultra::ai::SemanticParseResult semantic;
      std::string parseError;
      if (!ultra::ai::SemanticExtractor::extract(file.path, lang, semantic, parseError)) {
        result["errors"].push_back(
            {{"path", file.path.string()}, {"error", parseError}});
        continue;
      }
      nlohmann::json fileJson;
      fileJson["path"] = file.path.string();
      fileJson["language"] = ultra::ai::FileRegistry::languageToString(lang);
      fileJson["dependencies"] = semantic.dependencyReferences;
      fileJson["symbols"] = semantic.symbols.size();
      fileJson["symbol_dependencies"] = semantic.symbolDependencies.size();
      result["files"].push_back(std::move(fileJson));
    }
    const std::filesystem::path out = root / ".ultra.context.json";
    std::ofstream output(out, std::ios::binary | std::ios::trunc);
    if (output) {
      output << result.dump(2);
    }
    std::cout << "\n[context] written: " << out << "\n";
    return result;
}
  bool CppAdapter::applyPatch(
      const std::filesystem::path& root,
      const std::filesystem::path& diffFile) {
    lastPatchOutcome_ = {
        {"ok", false},
        {"applied", false},
        {"file_verified", false},
        {"build_required", false},
        {"build_executed", false},
        {"build_exit_code", 0},
        {"rolled_back", false},
        {"error", ""},
        {"git_output", ""},
    };
    if (!std::filesystem::exists(diffFile) ||
        !std::filesystem::is_regular_file(diffFile)) {
      ultra::core::Logger::error(
          ultra::core::LogCategory::Patch,
          "Diff file not found: " + diffFile.string());
      lastBuildExitCode_ = 1;
      lastPatchOutcome_["build_exit_code"] = lastBuildExitCode_;
      lastPatchOutcome_["error"] = "Diff file not found: " + diffFile.string();
      return false;
    }

    const std::vector<std::filesystem::path> patchTargets =
        parsePatchTargets(diffFile);
    if (patchTargets.empty()) {
      ultra::core::Logger::error(
          ultra::core::LogCategory::Patch,
          "Patch verification could not resolve any target files.");
      lastBuildExitCode_ = 1;
      lastPatchOutcome_["build_exit_code"] = lastBuildExitCode_;
      lastPatchOutcome_["error"] = "patch_targets_unresolved";
      return false;
    }

    std::vector<PatchTargetSnapshot> patchSnapshots =
        snapshotPatchTargets(root, patchTargets);

    ultra::platform::WindowsProcessExecutor executor;
    ultra::build::BuildEngine engine(
        std::make_unique<ultra::platform::WindowsProcessExecutor>());

    const std::string applyCommand =
        buildCommandInWorkingDirectory(
            root,
            "git apply --verbose --whitespace=nowarn " +
                quoteForShell(diffFile.string()));
    const ultra::platform::ProcessResult applyResult =
        executeCapturedCommand(executor, applyCommand);
    const std::string gitOutput = combineProcessOutput(applyResult);
    lastPatchOutcome_["git_output"] = gitOutput;
    if (!applyResult.success()) {
      lastBuildExitCode_ = applyResult.exitCode == 0 ? 1 : applyResult.exitCode;
      const std::string errorMessage =
          gitOutput.empty() ? "apply_patch failed during git apply." : gitOutput;
      ultra::core::Logger::error(
          ultra::core::LogCategory::Patch,
          errorMessage);
      lastPatchOutcome_["build_exit_code"] = lastBuildExitCode_;
      lastPatchOutcome_["error"] = errorMessage;
      return false;
    }

    if (containsSkippedPatchMarker(gitOutput)) {
      lastBuildExitCode_ = 1;
      lastPatchOutcome_["build_exit_code"] = lastBuildExitCode_;
      lastPatchOutcome_["error"] = "patch_skipped_no_match";
      ultra::core::Logger::error(
          ultra::core::LogCategory::Patch,
          gitOutput.empty() ? "Patch was skipped by git apply."
                            : gitOutput);
      return false;
    }

    const std::vector<std::string> changedPaths =
        refreshPatchVerification(patchSnapshots);
    const bool fileVerified = !changedPaths.empty();
    lastPatchOutcome_["file_verified"] = fileVerified;
    if (!fileVerified) {
      lastBuildExitCode_ = 1;
      lastPatchOutcome_["build_exit_code"] = lastBuildExitCode_;
      lastPatchOutcome_["error"] = "file_not_modified";
      ultra::core::Logger::error(
          ultra::core::LogCategory::Patch,
          "Patch apply completed without modifying any verified file targets.");
      return false;
    }

    lastPatchOutcome_["applied"] = true;

    const bool buildRequired =
        ultra::build::BuildExecutor::projectHasBuildSystem(root);
    lastPatchOutcome_["build_required"] = buildRequired;
    if (!buildRequired) {
      lastBuildExitCode_ = 0;
      lastPatchOutcome_["ok"] = true;
      return true;
    }

    const ultra::platform::ProcessResult sleepResult = executor.execute(
        buildCommandInWorkingDirectory(root, "ultra sleep_ai"));
    if (!sleepResult.success()) {
      ultra::core::Logger::warning(
          ultra::core::LogCategory::Patch,
          "sleep_ai returned non-zero before build. Continuing with Release build.\n" +
              combineProcessOutput(sleepResult));
    }

    lastBuildExitCode_ = engine.fullBuild(root);
    lastPatchOutcome_["build_executed"] = engine.lastBuildExecuted();
    lastPatchOutcome_["build_exit_code"] = lastBuildExitCode_;
    if (lastBuildExitCode_ == 0) {
      lastPatchOutcome_["ok"] = true;
      return true;
    }

    if (!engine.lastBuildExecuted()) {
      const std::string errorMessage =
          "Release build could not be executed after patch apply; patch changes were kept.";
      ultra::core::Logger::error(
          ultra::core::LogCategory::Patch,
          errorMessage);
      lastPatchOutcome_["applied"] = true;
      lastPatchOutcome_["file_verified"] = true;
      lastPatchOutcome_["error"] = errorMessage;
      return false;
    }

    const ultra::platform::ProcessResult rollbackResult = executor.execute(
        buildCommandInWorkingDirectory(
            root,
            "git apply -R --whitespace=nowarn " + quoteForShell(diffFile.string())));
    if (!rollbackResult.success()) {
      const std::string errorMessage =
          "Release build failed after patch apply, and rollback also failed.\n" +
          combineProcessOutput(rollbackResult);
      ultra::core::Logger::error(
          ultra::core::LogCategory::Patch,
          errorMessage);
      const std::vector<std::string> postRollbackFailureChanges =
          refreshPatchVerification(patchSnapshots);
      const bool stillApplied = !postRollbackFailureChanges.empty();
      lastPatchOutcome_["applied"] = stillApplied;
      lastPatchOutcome_["file_verified"] = stillApplied;
      lastPatchOutcome_["error"] = errorMessage;
      return false;
    }

    const std::vector<std::string> postRollbackChanges =
        refreshPatchVerification(patchSnapshots);
    lastPatchOutcome_["applied"] = false;
    lastPatchOutcome_["file_verified"] = !postRollbackChanges.empty();
    if (!postRollbackChanges.empty()) {
      lastPatchOutcome_["applied"] = true;
      lastPatchOutcome_["error"] =
          "rollback_incomplete_after_build_failure";
      ultra::core::Logger::error(
          ultra::core::LogCategory::Patch,
          "Rollback reported success, but verified files still differ from the pre-patch snapshot.");
      return false;
    }
    lastPatchOutcome_["file_verified"] = false;
    lastPatchOutcome_["rolled_back"] = true;
    lastPatchOutcome_["error"] =
        "Release build failed after patch apply; patch changes were rolled back.";
    ultra::core::Logger::error(
        ultra::core::LogCategory::Patch,
        "Release build failed after patch apply; patch changes were rolled back.");
    return false;
  }  }//namespace ultra::language

