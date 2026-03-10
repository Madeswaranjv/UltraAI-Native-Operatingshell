#include "CppAdapter.h"
#include "../ai/AIContextGenerator.h"
#include "../build/BuildEngine.h"
#include "../core/ConfigManager.h"
#include "../core/Logger.h"
#include "../graph/DependencyGraph.h"
#include "../graph/IncludeParser.h"
#include "../hashing/HashManager.h"
#include "../incremental/IncrementalAnalyzer.h"
#include "../patch/PatchManager.h"
#include "../platform/WindowsProcessExecutor.h"
#include "context/ContextSnapshot.h"
#include "../scanner/ProjectScanner.h"
#include "../utils/PathUtils.h"
#include "utils/FileClassifier.h"
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

}  // namespace

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
  for (const auto& f : files) {
    if (!ultra::utils::isParsableSourceOrHeader(f.path)) continue;
    std::string key = f.path.lexically_normal().string();
    graph.addNode(key);
    filenameToPaths[f.path.filename().string()].push_back(key);
  }
  for (const auto& f : files) {
    if (!ultra::utils::isParsableSourceOrHeader(f.path)) continue;
    std::string fromKey = f.path.lexically_normal().string();
    std::vector<std::string> includes =
        ultra::graph::IncludeParser::extractIncludes(f.path);
    for (const std::string& inc : includes) {
      std::optional<std::string> toKey =
          resolveInclude(inc, f.path, filenameToPaths);
      if (toKey.has_value()) graph.addEdge(fromKey, toKey.value());
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
  std::vector<std::string> changed = hashManager.detectChanges(files, graph);
  std::vector<std::string> rebuildSet =
      ultra::incremental::IncrementalAnalyzer::computeRebuildSet(changed,
                                                                 graph);
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
  std::vector<std::string> changed = hashManager.detectChanges(files, graph);
  std::vector<std::string> rebuildSet =
      ultra::incremental::IncrementalAnalyzer::computeRebuildSet(changed,
                                                                 graph);
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
  lastBuildExitCode_ = engine.incrementalBuild(root, rebuildSet);
}

void CppAdapter::buildFast(const std::filesystem::path& root) {
  ultra::core::Logger::info(ultra::core::LogCategory::Incremental,
                            "Scanning project (build-fast)...");
  std::vector<ultra::scanner::FileInfo> files = scan(root);
  ultra::graph::DependencyGraph graph = buildGraph(files);
  std::filesystem::path dbPath = root / ".ultra.db";
  ultra::hashing::HashManager hashManager(dbPath);
  hashManager.load();
  std::vector<std::string> changed = hashManager.detectChanges(files, graph);
  std::vector<std::string> rebuildSet =
      ultra::incremental::IncrementalAnalyzer::computeRebuildSet(changed,
                                                                 graph);
  hashManager.save();
  if (changed.empty()) {
    std::cout << "Build is up to date.\n";
    lastBuildExitCode_ = 0;
    return;
  }
  std::size_t allCppCount = 0;
  for (const std::string& n : graph.getNodes()) {
    if (std::filesystem::path(n).extension() == ".cpp") ++allCppCount;
  }
  ultra::build::BuildEngine engine(
      std::make_unique<ultra::platform::WindowsProcessExecutor>());
  lastBuildExitCode_ =
      engine.fastIncrementalBuild(root, rebuildSet, allCppCount);
}

namespace {

std::string formatBytes(std::size_t bytes) {
  if (bytes >= 1024 * 1024) {
    double mb = static_cast<double>(bytes) / (1024 * 1024);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    return buf;
  }
  if (bytes >= 1024) {
    double kb = static_cast<double>(bytes) / 1024;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f KB", kb);
    return buf;
  }
  return std::to_string(bytes) + " B";
}

void printContextSummary(const ultra::ai::GenerateResult& result) {
  std::string jsonStr = result.context.dump();
  std::size_t compressedBytes = jsonStr.size();
  std::size_t originalBytes = result.stats.includedBytes > 0
                                  ? result.stats.includedBytes
                                  : result.stats.totalBytes;
  double reduction = 0.0;
  if (originalBytes > 0) {
    reduction = 100.0 * (1.0 - static_cast<double>(compressedBytes) /
                                    static_cast<double>(originalBytes));
  }
  std::cout << "AI Context Summary\n";
  std::cout << "Total project files: " << result.stats.totalFiles << '\n';
  std::cout << "Included in context: " << result.stats.includedFiles << '\n';
  std::cout << "Filtered out: " << result.stats.filteredFiles << '\n';
  std::cout << "Original size estimate: " << formatBytes(originalBytes)
            << '\n';
  std::cout << "Compressed context size: " << formatBytes(compressedBytes)
            << '\n';
  std::ios_base::fmtflags prev = std::cout.flags();
  std::cout << "Reduction: " << std::fixed << std::setprecision(1) << reduction
            << "%\n";
  std::cout.flags(prev);
}

}  // namespace

nlohmann::json CppAdapter::generateContext(
    const std::filesystem::path& root) {
  ultra::core::Logger::info(ultra::core::LogCategory::Context,
                            "Scanning project...");
  std::vector<ultra::scanner::FileInfo> files = scan(root);
  ultra::graph::DependencyGraph graph = buildGraph(files);
  std::filesystem::path configPath = root / "ultra.json";
  ultra::core::ConfigManager config(configPath);
  ultra::ai::GenerateResult result =
      ultra::ai::AIContextGenerator::generate(files, graph, config);
  printContextSummary(result);
  std::unordered_map<std::string, std::string> snapshot;
  ultra::hashing::HashManager hm(root / ".ultra.db");
  for (const std::string& pathKey : result.includedPathKeys) {
    std::string h = hm.computeHash(std::filesystem::path(pathKey));
    if (!h.empty()) snapshot[pathKey] = h;
  }
  ultra::context::saveSnapshot(root / ".ultra.context.prev.json", snapshot);
  return result.context;
}

nlohmann::json CppAdapter::generateContextWithAst(
    const std::filesystem::path& root) {
  ultra::core::Logger::info(ultra::core::LogCategory::Context,
                            "Scanning project (AST mode)...");
  std::vector<ultra::scanner::FileInfo> files = scan(root);
  ultra::graph::DependencyGraph graph = buildGraph(files);
  std::filesystem::path configPath = root / "ultra.json";
  ultra::core::ConfigManager config(configPath);
  ultra::ai::GenerateResult result =
      ultra::ai::AIContextGenerator::generateWithAst(files, graph, config);
  printContextSummary(result);
  std::unordered_map<std::string, std::string> snapshot;
  ultra::hashing::HashManager hm(root / ".ultra.db");
  for (const std::string& pathKey : result.includedPathKeys) {
    std::string h = hm.computeHash(std::filesystem::path(pathKey));
    if (!h.empty()) snapshot[pathKey] = h;
  }
  ultra::context::saveSnapshot(root / ".ultra.context.prev.json", snapshot);
  return result.context;
}

bool CppAdapter::applyPatch(const std::filesystem::path& root,
                            const std::filesystem::path& diffFile) {
  std::cout << "Applying Patch\n";
  ultra::build::BuildEngine engine(
      std::make_unique<ultra::platform::WindowsProcessExecutor>());
  ultra::patch::PatchManager patchManager(engine);
  ultra::patch::ApplyResult result = patchManager.applyPatch(root, diffFile);
  std::cout << "Files modified: " << result.filesModified << '\n';
  if (result.success) {
    std::cout << "Build verification: SUCCESS\n";
    std::cout << "Patch applied successfully.\n";
  } else {
    std::cout << "Build verification: FAILED\n";
    std::cout << "Patch reverted.\n";
  }
  return result.success;
}

}  // namespace ultra::language
