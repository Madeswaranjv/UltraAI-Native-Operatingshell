  #include "CppAdapter.h"
  #include "../ai/SemanticExtractor.h"
  #include "../build/BuildEngine.h"
  #include "../core/ConfigManager.h"
  #include "../core/Logger.h"
  #include "../graph/DependencyGraph.h"
  #include "../hashing/HashManager.h"
  #include "../incremental/IncrementalAnalyzer.h"
  #include "../patch/PatchManager.h"
  #include "../platform/WindowsProcessExecutor.h"
  #include "context/ContextSnapshot.h"
  #include "../scanner/ProjectScanner.h"
  #include "../utils/PathUtils.h"
  #include "utils/FileClassifier.h"
  // NEW AST INCLUDES
  #include "../ai/FileRegistry.h"
  #include <algorithm>
  #include <cstdio>
  #include <fstream>
  #include <iomanip>
  #include <iostream>
  #include <optional>
  #include <unordered_map>
  #include <string>
  namespace ultra::language {
  namespace {
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
      (void)root;      // prevent unused parameter warning
      (void)diffFile;  // prevent unused parameter warning
      return false;
  }
  }//namespace ultra::language
