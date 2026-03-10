#pragma once

#include <external/json.hpp>

#include "../CognitiveState.h"
#include "GraphSnapshot.h"
#include "impact_analyzer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ultra::runtime {

class QueryEngine {
 public:
  explicit QueryEngine(std::filesystem::path projectRoot);

  [[nodiscard]] nlohmann::json queryTarget(const GraphSnapshot& snapshot,
                                           const std::string& target) const;
  [[nodiscard]] nlohmann::json queryFile(const GraphSnapshot& snapshot,
                                         const std::string& target) const;
  [[nodiscard]] nlohmann::json querySymbol(const GraphSnapshot& snapshot,
                                           const std::string& symbolName) const;
  [[nodiscard]] nlohmann::json queryImpact(const CognitiveState& state,
                                           const std::string& target) const;

  [[nodiscard]] std::string resolveIndexedFilePath(
      const GraphSnapshot& snapshot,
      const std::string& target) const;

  [[nodiscard]] std::uintmax_t fileSizeForIndexedPath(
      const std::string& indexedPath) const;

  bool readSourceByIndexedPath(const std::string& indexedPath,
                               nlohmann::json& payloadOut,
                               std::string& error) const;

 private:
  struct FileKind {
    std::string label;
    bool semantic{false};
  };

  struct FileView {
    std::string nodeId;
    std::string path;
    std::uint32_t fileId{0U};
    double weight{0.0};
    bool isHot{false};
  };

  struct SymbolView {
    std::string nodeId;
    std::uint64_t symbolId{0U};
    std::string name;
    std::string definedIn;
    std::vector<std::string> usedIn;
    double weight{0.0};
    double centrality{0.0};
  };

  static const memory::StateGraph& requireGraph(const GraphSnapshot& snapshot);
  static std::string normalizePathToken(const std::string& value);
  static bool isHeaderExtension(const std::string& lowerExt);
  static bool isSourceExtension(const std::string& lowerExt);
  static FileKind classifyFile(const std::string& path);
  static std::vector<std::string> toSortedVector(
      const std::set<std::string>& values);
  [[nodiscard]] std::vector<FileView> collectFiles(
      const GraphSnapshot& snapshot) const;
  [[nodiscard]] std::vector<SymbolView> collectSymbols(
      const GraphSnapshot& snapshot) const;

  std::filesystem::path projectRoot_;
};

inline QueryEngine::QueryEngine(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()) {}

inline const memory::StateGraph& QueryEngine::requireGraph(
    const GraphSnapshot& snapshot) {
  if (!snapshot.graph) {
    throw std::runtime_error("Graph snapshot is empty.");
  }
  return *snapshot.graph;
}

inline std::string QueryEngine::normalizePathToken(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  std::string normalized = value;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  normalized =
      std::filesystem::path(normalized).lexically_normal().generic_string();
  if (normalized == ".") {
    return {};
  }
  if (normalized.size() >= 2U && normalized[0] == '.' && normalized[1] == '/') {
    normalized.erase(0, 2U);
  }
  return normalized;
}

inline bool QueryEngine::isHeaderExtension(const std::string& lowerExt) {
  static constexpr std::array<const char*, 4U> kHeaderExt{
      ".h", ".hh", ".hpp", ".hxx"};
  return std::find(kHeaderExt.begin(), kHeaderExt.end(), lowerExt) !=
         kHeaderExt.end();
}

inline bool QueryEngine::isSourceExtension(const std::string& lowerExt) {
  static constexpr std::array<const char*, 14U> kSourceExt{
      ".c",   ".cc",  ".cpp", ".cxx", ".js",  ".jsx", ".mjs",
      ".cjs", ".ts",  ".tsx", ".py",  ".pyi", ".m",   ".mm"};
  return std::find(kSourceExt.begin(), kSourceExt.end(), lowerExt) !=
         kSourceExt.end();
}

inline QueryEngine::FileKind QueryEngine::classifyFile(const std::string& path) {
  std::string extension = std::filesystem::path(path).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });

  if (isHeaderExtension(extension)) {
    return {"header", true};
  }
  if (isSourceExtension(extension)) {
    return {"source", true};
  }

  static constexpr std::array<const char*, 10U> kConfigExt{
      ".json", ".yaml", ".yml", ".toml", ".ini",
      ".cfg",  ".conf", ".xml", ".env",  ".properties"};
  if (std::find(kConfigExt.begin(), kConfigExt.end(), extension) !=
      kConfigExt.end()) {
    return {"config", false};
  }

  static constexpr std::array<const char*, 5U> kDocsExt{
      ".md", ".mdx", ".rst", ".txt", ".adoc"};
  if (std::find(kDocsExt.begin(), kDocsExt.end(), extension) !=
      kDocsExt.end()) {
    return {"documentation", false};
  }

  static constexpr std::array<const char*, 8U> kScriptExt{
      ".sh", ".bash", ".ps1", ".bat", ".cmd", ".zsh", ".fish", ".rb"};
  if (std::find(kScriptExt.begin(), kScriptExt.end(), extension) !=
      kScriptExt.end()) {
    return {"script", false};
  }

  static constexpr std::array<const char*, 5U> kBuildExt{
      ".cmake", ".gradle", ".mk", ".make", ".ninja"};
  if (std::find(kBuildExt.begin(), kBuildExt.end(), extension) !=
      kBuildExt.end()) {
    return {"build", false};
  }

  return {"other", false};
}

inline std::vector<std::string> QueryEngine::toSortedVector(
    const std::set<std::string>& values) {
  return std::vector<std::string>(values.begin(), values.end());
}

inline std::vector<QueryEngine::FileView> QueryEngine::collectFiles(
    const GraphSnapshot& snapshot) const {
  const memory::StateGraph& graph = requireGraph(snapshot);
  std::vector<FileView> files;
  for (const memory::StateNode& node : graph.queryByType(memory::NodeType::File)) {
    const std::string path = node.data.value("path", std::string{});
    if (path.empty()) {
      continue;
    }
    FileView view;
    view.nodeId = node.nodeId;
    view.path = path;
    view.fileId = node.data.value("file_id", 0U);
    view.weight = node.data.value("weight", 0.0);
    view.isHot = node.data.value("is_hot", false);
    files.push_back(std::move(view));
  }

  std::sort(files.begin(), files.end(),
            [](const FileView& left, const FileView& right) {
              if (left.path != right.path) {
                return left.path < right.path;
              }
              return left.fileId < right.fileId;
            });
  return files;
}

inline std::vector<QueryEngine::SymbolView> QueryEngine::collectSymbols(
    const GraphSnapshot& snapshot) const {
  const memory::StateGraph& graph = requireGraph(snapshot);
  std::vector<SymbolView> symbols;
  for (const memory::StateNode& node :
       graph.queryByType(memory::NodeType::Symbol)) {
    const std::string name = node.data.value("name", std::string{});
    if (name.empty()) {
      continue;
    }
    SymbolView view;
    view.nodeId = node.nodeId;
    view.symbolId = node.data.value("symbol_id", 0ULL);
    view.name = name;
    view.definedIn = node.data.value("defined_in", std::string{});
    view.weight = node.data.value("weight", 0.0);
    view.centrality = node.data.value("centrality", 0.0);
    if (node.data.contains("used_in") && node.data["used_in"].is_array()) {
      for (const auto& item : node.data["used_in"]) {
        if (item.is_string()) {
          view.usedIn.push_back(item.get<std::string>());
        }
      }
    }
    std::sort(view.usedIn.begin(), view.usedIn.end());
    view.usedIn.erase(std::unique(view.usedIn.begin(), view.usedIn.end()),
                      view.usedIn.end());
    symbols.push_back(std::move(view));
  }

  std::sort(symbols.begin(), symbols.end(),
            [](const SymbolView& left, const SymbolView& right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.symbolId < right.symbolId;
            });
  return symbols;
}

inline std::string QueryEngine::resolveIndexedFilePath(
    const GraphSnapshot& snapshot,
    const std::string& target) const {
  if (target.empty()) {
    return {};
  }

  const std::vector<FileView> files = collectFiles(snapshot);
  std::set<std::string> candidates;
  const auto addCandidate = [&candidates](const std::string& candidate) {
    const std::string normalized = normalizePathToken(candidate);
    if (!normalized.empty()) {
      candidates.insert(normalized);
    }
  };

  addCandidate(target);

  const std::filesystem::path rawTarget(target);
  if (rawTarget.is_absolute()) {
    addCandidate(rawTarget.lexically_normal().generic_string());
    const std::filesystem::path rel =
        rawTarget.lexically_normal().lexically_relative(projectRoot_);
    if (!rel.empty() && rel.generic_string() != ".") {
      addCandidate(rel.generic_string());
    }
  } else {
    const std::filesystem::path absolute =
        (projectRoot_ / rawTarget).lexically_normal();
    const std::filesystem::path rel = absolute.lexically_relative(projectRoot_);
    if (!rel.empty() && rel.generic_string() != ".") {
      addCandidate(rel.generic_string());
    }
  }

  for (const FileView& file : files) {
    if (candidates.find(file.path) != candidates.end()) {
      return file.path;
    }
  }

  std::string uniqueSuffixMatch;
  for (const FileView& file : files) {
    for (const std::string& candidate : candidates) {
      if (candidate.empty() || candidate.size() >= file.path.size()) {
        continue;
      }
      const std::size_t offset = file.path.size() - candidate.size();
      if (file.path.compare(offset, candidate.size(), candidate) != 0) {
        continue;
      }
      if (offset > 0U && file.path[offset - 1U] != '/') {
        continue;
      }
      if (uniqueSuffixMatch.empty()) {
        uniqueSuffixMatch = file.path;
      } else if (uniqueSuffixMatch != file.path) {
        return {};
      }
    }
  }

  return uniqueSuffixMatch;
}

inline nlohmann::json QueryEngine::queryFile(const GraphSnapshot& snapshot,
                                             const std::string& target) const {
  const memory::StateGraph& graph = requireGraph(snapshot);
  const std::vector<FileView> files = collectFiles(snapshot);
  const std::vector<SymbolView> symbols = collectSymbols(snapshot);
  const std::string resolvedPath = resolveIndexedFilePath(snapshot, target);
  if (resolvedPath.empty()) {
    return nlohmann::json{{"kind", "not_found"}, {"target", target}};
  }

  const auto fileIt =
      std::find_if(files.begin(), files.end(),
                   [&resolvedPath](const FileView& file) {
                     return file.path == resolvedPath;
                   });
  if (fileIt == files.end()) {
    return nlohmann::json{{"kind", "not_found"}, {"target", target}};
  }

  const FileKind fileKind = classifyFile(fileIt->path);
  nlohmann::json result;
  result["kind"] = "file";
  result["path"] = fileIt->path;
  result["file_type"] = fileKind.label;
  result["size"] = 0U;
  result["semantic"] = fileKind.semantic;
  result["recently_modified"] = fileIt->isHot;

  if (!fileKind.semantic) {
    return result;
  }

  std::set<std::string> symbolsDefined;
  for (const SymbolView& symbol : symbols) {
    if (symbol.definedIn == fileIt->path) {
      symbolsDefined.insert(symbol.name);
    }
  }

  std::set<std::string> symbolsUsed;
  for (const memory::StateEdge& edge : graph.getOutboundEdges(fileIt->nodeId)) {
    const memory::StateNode targetNode = graph.getNode(edge.targetId);
    if (targetNode.nodeType != memory::NodeType::Symbol) {
      continue;
    }
    const std::string symbolName = targetNode.data.value("name", std::string{});
    if (!symbolName.empty()) {
      symbolsUsed.insert(symbolName);
    }
  }

  std::set<std::string> dependencies;
  std::set<std::string> dependedBy;
  std::set<std::string> neighbors;

  for (const memory::StateEdge& edge : graph.getOutboundEdges(fileIt->nodeId)) {
    const memory::StateNode targetNode = graph.getNode(edge.targetId);
    if (targetNode.nodeType != memory::NodeType::File) {
      continue;
    }
    const std::string depPath = targetNode.data.value("path", std::string{});
    if (!depPath.empty()) {
      dependencies.insert(depPath);
      neighbors.insert(depPath);
    }
  }

  for (const FileView& file : files) {
    if (file.nodeId == fileIt->nodeId) {
      continue;
    }
    for (const memory::StateEdge& edge : graph.getOutboundEdges(file.nodeId)) {
      if (edge.targetId != fileIt->nodeId) {
        continue;
      }
      dependedBy.insert(file.path);
      neighbors.insert(file.path);
      break;
    }
  }

  double centrality = 0.0;
  if (files.size() > 1U) {
    centrality = static_cast<double>(neighbors.size()) /
                 static_cast<double>(files.size() - 1U);
  }

  result["symbols_defined"] = toSortedVector(symbolsDefined);
  result["symbols_used"] = toSortedVector(symbolsUsed);
  result["dependencies"] = toSortedVector(dependencies);
  result["depended_by"] = toSortedVector(dependedBy);
  result["weight"] = fileIt->weight;
  result["centrality"] = centrality;
  return result;
}

inline nlohmann::json QueryEngine::querySymbol(const GraphSnapshot& snapshot,
                                               const std::string& symbolName) const {
  const std::vector<SymbolView> symbols = collectSymbols(snapshot);
  const auto symbolIt =
      std::find_if(symbols.begin(), symbols.end(),
                   [&symbolName](const SymbolView& symbol) {
                     return symbol.name == symbolName;
                   });
  if (symbolIt == symbols.end()) {
    return nlohmann::json{{"kind", "not_found"}, {"target", symbolName}};
  }

  nlohmann::json result;
  result["kind"] = "symbol";
  result["name"] = symbolIt->name;
  result["defined_in"] = symbolIt->definedIn;
  result["used_in"] = symbolIt->usedIn;
  result["usage_count"] = symbolIt->usedIn.size();
  result["weight"] = symbolIt->weight;
  result["centrality"] = symbolIt->centrality;
  return result;
}

inline nlohmann::json QueryEngine::queryTarget(const GraphSnapshot& snapshot,
                                               const std::string& target) const {
  nlohmann::json fileResult = queryFile(snapshot, target);
  if (fileResult.value("kind", "") != "not_found") {
    return fileResult;
  }
  return querySymbol(snapshot, target);
}

inline nlohmann::json QueryEngine::queryImpact(const CognitiveState& state,
                                               const std::string& target) const {
  const GraphSnapshot& snapshot = state.snapshot;
  ImpactAnalyzer analyzer(snapshot);
  const std::string resolvedFile = resolveIndexedFilePath(snapshot, target);
  if (!resolvedFile.empty()) {
    return analyzer.analyzeFileImpact(resolvedFile);
  }
  return analyzer.analyzeSymbolImpact(target);
}

inline std::uintmax_t QueryEngine::fileSizeForIndexedPath(
    const std::string& indexedPath) const {
  if (indexedPath.empty()) {
    return 0U;
  }
  const std::filesystem::path absolutePath =
      (projectRoot_ / indexedPath).lexically_normal();
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(absolutePath, ec);
  if (ec) {
    return 0U;
  }
  return size;
}

inline bool QueryEngine::readSourceByIndexedPath(const std::string& indexedPath,
                                                 nlohmann::json& payloadOut,
                                                 std::string& error) const {
  payloadOut = nlohmann::json::object();
  if (indexedPath.empty()) {
    error = "Missing indexed source path.";
    return false;
  }

  const std::filesystem::path absolutePath =
      (projectRoot_ / indexedPath).lexically_normal();
  std::ifstream input(absolutePath, std::ios::binary);
  if (!input) {
    error = "Failed to open source file: " + indexedPath;
    return false;
  }

  const std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) {
    error = "Failed reading source file: " + indexedPath;
    return false;
  }

  payloadOut["kind"] = "source";
  payloadOut["path"] = indexedPath;
  payloadOut["content"] = content;
  return true;
}

}  // namespace ultra::runtime
