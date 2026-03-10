#include "AIContextGenerator.h"
#include "StructureExtractor.h"
#include "../core/ConfigManager.h"
#include "../graph/DependencyGraph.h"
#include "../scanner/FileInfo.h"
#include "ast/AstExtractor.h"
#include "ast/AstTypes.h"
#include "external/json.hpp"
#include "utils/FileClassifier.h"
#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_set>

namespace ultra::ai {

namespace {

const std::vector<std::string> kContextIgnoreDirs{"build", ".git", "tests",
                                                  "vendor"};
constexpr std::size_t kMaxFunctionsVendor = 100;
constexpr std::size_t kMaxStructsClassesVendor = 20;

std::string typeToString(ultra::scanner::FileType t) {
  switch (t) {
    case ultra::scanner::FileType::Source:
      return "source";
    case ultra::scanner::FileType::Header:
      return "header";
    default:
      return "other";
  }
}

bool isUnderIgnoreDir(const std::filesystem::path& path) {
  for (const auto& part : path) {
    std::string name = part.string();
    if (std::find(kContextIgnoreDirs.begin(), kContextIgnoreDirs.end(),
                  name) != kContextIgnoreDirs.end()) {
      return true;
    }
  }
  return false;
}

bool matchesVendorPattern(const std::string& filename,
                          const std::vector<std::string>& patterns) {
  for (const std::string& pat : patterns) {
    if (filename.find(pat) != std::string::npos) return true;
  }
  return false;
}

bool isVendorByHeuristics(const std::filesystem::path& path,
                          const std::vector<std::string>& vendorPatterns) {
  std::string filename = path.filename().string();
  if (matchesVendorPattern(filename, vendorPatterns)) return true;
  std::vector<std::string> classes = StructureExtractor::extractClasses(path);
  std::vector<std::string> structs = StructureExtractor::extractStructs(path);
  std::vector<std::string> funcs = StructureExtractor::extractFunctions(path);
  if (funcs.size() > kMaxFunctionsVendor) return true;
  if (classes.size() + structs.size() > kMaxStructsClassesVendor) return true;
  return false;
}

}  // namespace

GenerateResult AIContextGenerator::generate(
    const std::vector<ultra::scanner::FileInfo>& files,
    const ultra::graph::DependencyGraph& graph,
    const ultra::core::ConfigManager& config) {
  GenerateResult result;
  result.stats.totalFiles = files.size();
  const std::size_t maxBytes = config.maxFileSizeKb() * 1024;
  std::unordered_set<std::string> graphNodes;
  for (const std::string& node : graph.getNodes()) {
    graphNodes.insert(node);
  }
  std::size_t totalBytes = 0;
  for (const auto& f : files) {
    totalBytes += f.size;
  }
  result.stats.totalBytes = totalBytes;
  nlohmann::json root = nlohmann::json::object();
  root["project_summary"] = nlohmann::json::object();
  root["files"] = nlohmann::json::object();
  std::size_t includedCount = 0;
  std::size_t includedBytes = 0;
  for (const auto& f : files) {
    std::string pathKey = f.path.lexically_normal().string();
    if (!ultra::utils::isContextSourceFile(f.path)) continue;
    if (isUnderIgnoreDir(f.path)) continue;
    if (f.size > maxBytes) continue;
    if (graphNodes.find(pathKey) == graphNodes.end()) continue;
    if (isVendorByHeuristics(f.path, config.vendorPatterns())) continue;
    std::string filename = f.path.filename().string();
    nlohmann::json fileObj = nlohmann::json::object();
    fileObj["type"] = typeToString(f.type);
    fileObj["classes"] = nlohmann::json::array();
    fileObj["structs"] = nlohmann::json::array();
    fileObj["functions"] = nlohmann::json::array();
    fileObj["dependencies"] = nlohmann::json::array();
    for (const std::string& c : StructureExtractor::extractClasses(f.path)) {
      fileObj["classes"].push_back(c);
    }
    for (const std::string& s : StructureExtractor::extractStructs(f.path)) {
      fileObj["structs"].push_back(s);
    }
    for (const std::string& fn : StructureExtractor::extractFunctions(f.path)) {
      fileObj["functions"].push_back(fn);
    }
    std::set<std::string> depNames;
    for (const std::string& depPath : graph.getDependencies(pathKey)) {
      std::string depName =
          std::filesystem::path(depPath).filename().string();
      if (depNames.insert(depName).second) {
        fileObj["dependencies"].push_back(depName);
      }
    }
    root["files"][filename] = fileObj;
    ++includedCount;
    includedBytes += f.size;
  }
  result.stats.includedFiles = includedCount;
  result.stats.filteredFiles = result.stats.totalFiles - includedCount;
  result.stats.includedBytes = includedBytes;
  root["project_summary"]["total_project_files"] = result.stats.totalFiles;
  root["project_summary"]["total_context_files"] = includedCount;
  root["project_summary"]["total_graph_nodes"] = graph.nodeCount();
  root["project_summary"]["total_graph_edges"] = graph.edgeCount();
  result.context = std::move(root);
  return result;
}

GenerateResult AIContextGenerator::generateWithAst(
    const std::vector<ultra::scanner::FileInfo>& files,
    const ultra::graph::DependencyGraph& graph,
    const ultra::core::ConfigManager& config) {
  GenerateResult result;
  result.stats.totalFiles = files.size();
  const std::size_t maxBytes = config.maxFileSizeKb() * 1024;
  std::unordered_set<std::string> graphNodes;
  for (const std::string& node : graph.getNodes()) {
    graphNodes.insert(node);
  }
  std::size_t totalBytes = 0;
  for (const auto& f : files) {
    totalBytes += f.size;
  }
  result.stats.totalBytes = totalBytes;
  nlohmann::json root = nlohmann::json::object();
  root["project_summary"] = nlohmann::json::object();
  root["files"] = nlohmann::json::object();
  root["project_summary"]["ast_mode"] =
      std::string(ultra::ast::AstExtractor::isLibClangAvailable() ? "libclang"
                                                                 : "regex");
  std::size_t includedCount = 0;
  std::size_t includedBytes = 0;
  for (const auto& f : files) {
    std::string pathKey = f.path.lexically_normal().string();
    if (!ultra::utils::isContextSourceFile(f.path)) continue;
    if (isUnderIgnoreDir(f.path)) continue;
    if (f.size > maxBytes) continue;
    if (graphNodes.find(pathKey) == graphNodes.end()) continue;
    if (isVendorByHeuristics(f.path, config.vendorPatterns())) continue;
    std::string filename = f.path.filename().string();
    ultra::ast::FileStructure fs = ultra::ast::AstExtractor::extract(f.path);
    nlohmann::json fileObj = nlohmann::json::object();
    fileObj["type"] = typeToString(f.type);
    fileObj["classes"] = nlohmann::json::array();
    fileObj["structs"] = nlohmann::json::array();
    fileObj["methods"] = nlohmann::json::array();
    fileObj["functions"] = nlohmann::json::array();
    fileObj["namespaces"] = nlohmann::json::array();
    fileObj["dependencies"] = nlohmann::json::array();
    for (const auto& c : fs.classes) {
      nlohmann::json obj = nlohmann::json::object();
      obj["name"] = c.name;
      obj["line"] = c.line;
      fileObj["classes"].push_back(obj);
    }
    for (const auto& s : fs.structs) {
      nlohmann::json obj = nlohmann::json::object();
      obj["name"] = s.name;
      obj["line"] = s.line;
      fileObj["structs"].push_back(obj);
    }
    for (const auto& m : fs.methods) {
      nlohmann::json obj = nlohmann::json::object();
      obj["name"] = m.name;
      obj["line"] = m.line;
      fileObj["methods"].push_back(obj);
    }
    for (const auto& fn : fs.freeFunctions) {
      nlohmann::json obj = nlohmann::json::object();
      obj["name"] = fn.name;
      obj["line"] = fn.line;
      fileObj["functions"].push_back(obj);
    }
    for (const auto& ns : fs.namespaces) {
      nlohmann::json obj = nlohmann::json::object();
      obj["name"] = ns.name;
      obj["line"] = ns.line;
      fileObj["namespaces"].push_back(obj);
    }
    std::set<std::string> depNames;
    for (const std::string& depPath : graph.getDependencies(pathKey)) {
      std::string depName =
          std::filesystem::path(depPath).filename().string();
      if (depNames.insert(depName).second) {
        fileObj["dependencies"].push_back(depName);
      }
    }
    root["files"][filename] = fileObj;
    result.includedPathKeys.push_back(pathKey);
    ++includedCount;
    includedBytes += f.size;
  }
  result.stats.includedFiles = includedCount;
  result.stats.filteredFiles = result.stats.totalFiles - includedCount;
  result.stats.includedBytes = includedBytes;
  root["project_summary"]["total_project_files"] = result.stats.totalFiles;
  root["project_summary"]["total_context_files"] = includedCount;
  root["project_summary"]["total_graph_nodes"] = graph.nodeCount();
  root["project_summary"]["total_graph_edges"] = graph.edgeCount();
  result.context = std::move(root);
  return result;
}

std::vector<std::string> AIContextGenerator::getContextPathSet(
    const std::vector<ultra::scanner::FileInfo>& files,
    const ultra::graph::DependencyGraph& graph,
    const ultra::core::ConfigManager& config) {
  std::vector<std::string> out;
  const std::size_t maxBytes = config.maxFileSizeKb() * 1024;
  std::unordered_set<std::string> graphNodes;
  for (const std::string& node : graph.getNodes()) {
    graphNodes.insert(node);
  }
  for (const auto& f : files) {
    std::string pathKey = f.path.lexically_normal().string();
    if (!ultra::utils::isContextSourceFile(f.path)) continue;
    if (isUnderIgnoreDir(f.path)) continue;
    if (f.size > maxBytes) continue;
    if (graphNodes.find(pathKey) == graphNodes.end()) continue;
    if (isVendorByHeuristics(f.path, config.vendorPatterns())) continue;
    out.push_back(pathKey);
  }
  return out;
}

}  // namespace ultra::ai
