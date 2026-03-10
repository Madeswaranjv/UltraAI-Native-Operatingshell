#include "AgentExportBuilder.h"

#include "external/json.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace ultra::ai {

namespace {

// -----------------------------
// Deterministic JSON Canonicalizer
// -----------------------------
nlohmann::json canonicalizeJson(const nlohmann::json& input) {
  if (input.is_object()) {
    // Sort keys alphabetically
    std::map<std::string, nlohmann::json> ordered;
    for (auto it = input.begin(); it != input.end(); ++it) {
      ordered[it.key()] = canonicalizeJson(it.value());
    }

    nlohmann::json result = nlohmann::json::object();
    for (const auto& [key, value] : ordered) {
      result[key] = value;
    }
    return result;
  }

  if (input.is_array()) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& item : input) {
      result.push_back(canonicalizeJson(item));
    }
    return result;
  }

  // Primitive types remain unchanged
  return input;
}

// -----------------------------
// Symbol helpers
// -----------------------------

std::string symbolTypeToString(const SymbolType symbolType) {
  switch (symbolType) {
    case SymbolType::Class:
      return "class";
    case SymbolType::Function:
      return "function";
    case SymbolType::EntryPoint:
      return "entry_point";
    case SymbolType::Import:
      return "import";
    case SymbolType::Export:
      return "export";
    case SymbolType::ReactComponent:
      return "react_component";
    default:
      return "unknown";
  }
}

std::string visibilityToString(const Visibility visibility) {
  switch (visibility) {
    case Visibility::Public:
      return "public";
    case Visibility::Private:
      return "private";
    case Visibility::Protected:
      return "protected";
    case Visibility::Module:
      return "module";
    default:
      return "unknown";
  }
}

bool symbolLessForContext(const SymbolRecord& left,
                          const SymbolRecord& right) {
  return std::tie(left.lineNumber,
                  left.name,
                  left.symbolType,
                  left.visibility)
       < std::tie(right.lineNumber,
                  right.name,
                  right.symbolType,
                  right.visibility);
}

}  // namespace

// -----------------------------
// Agent Context Writer
// -----------------------------

bool AgentExportBuilder::writeAgentContext(
    const std::filesystem::path& outputPath,
    const std::vector<FileRecord>& files,
    const std::vector<SymbolRecord>& symbols,
    const DependencyTableData& deps,
    std::string& error) {

  nlohmann::json root = nlohmann::json::object();
  root["project_summary"] = nlohmann::json::object();
  root["entry_points"] = nlohmann::json::array();
  root["dependency_graph_summary"] = nlohmann::json::object();
  root["language_distribution"] = nlohmann::json::array();
  root["per_file_symbol_summaries"] = nlohmann::json::array();

  const std::map<std::uint32_t, std::string> pathById =
      FileRegistry::mapPathById(files);

  std::map<std::uint32_t, std::vector<SymbolRecord>> symbolsByFileId =
      SymbolTable::groupByFileId(symbols);

  std::map<Language, std::uint32_t> languageCounts;
  std::map<std::uint32_t, std::uint32_t> outgoingEdges;
  std::map<std::uint32_t, std::uint32_t> incomingEdges;

  for (const FileRecord& file : files) {
    languageCounts[file.language] += 1U;
  }

  for (const FileDependencyEdge& edge : deps.fileEdges) {
    outgoingEdges[edge.fromFileId] += 1U;
    incomingEdges[edge.toFileId] += 1U;
  }

  std::set<std::pair<std::string, std::string>> entryPoints;
  for (const SymbolRecord& symbol : symbols) {
    if (symbol.symbolType != SymbolType::EntryPoint) {
      continue;
    }

    const auto pathIt = pathById.find(symbol.fileId);
    if (pathIt == pathById.end()) {
      continue;
    }

    entryPoints.insert(std::make_pair(pathIt->second, symbol.name));
  }

  for (const auto& [path, symbolName] : entryPoints) {
    nlohmann::json item = nlohmann::json::object();
    item["path"] = path;
    item["symbol"] = symbolName;
    root["entry_points"].push_back(item);
  }

  for (const auto& [language, count] : languageCounts) {
    nlohmann::json item = nlohmann::json::object();
    item["language"] = FileRegistry::languageToString(language);
    item["count"] = count;
    root["language_distribution"].push_back(item);
  }

  for (const FileRecord& file : files) {
    nlohmann::json fileSummary = nlohmann::json::object();
    fileSummary["file_id"] = file.fileId;
    fileSummary["path"] = file.path;
    fileSummary["language"] =
        FileRegistry::languageToString(file.language);

    const auto symbolsIt = symbolsByFileId.find(file.fileId);
    const std::size_t symbolCount =
        symbolsIt == symbolsByFileId.end()
            ? 0U
            : symbolsIt->second.size();

    fileSummary["symbol_count"] = symbolCount;

    fileSummary["outgoing_dependencies"] =
        outgoingEdges.find(file.fileId) == outgoingEdges.end()
            ? 0U
            : outgoingEdges[file.fileId];

    fileSummary["incoming_dependencies"] =
        incomingEdges.find(file.fileId) == incomingEdges.end()
            ? 0U
            : incomingEdges[file.fileId];

    fileSummary["symbols"] = nlohmann::json::array();

    if (symbolsIt != symbolsByFileId.end()) {
      std::vector<SymbolRecord> perFileSymbols =
          symbolsIt->second;

      std::sort(perFileSymbols.begin(),
                perFileSymbols.end(),
                symbolLessForContext);

      for (const SymbolRecord& symbol : perFileSymbols) {
        nlohmann::json symbolJson = nlohmann::json::object();
        symbolJson["symbol_id"] = symbol.symbolId;
        symbolJson["name"] = symbol.name;
        symbolJson["symbol_type"] =
            symbolTypeToString(symbol.symbolType);
        symbolJson["visibility"] =
            visibilityToString(symbol.visibility);
        symbolJson["line"] = symbol.lineNumber;

        fileSummary["symbols"].push_back(symbolJson);
      }
    }

    root["per_file_symbol_summaries"]
        .push_back(fileSummary);
  }

  root["project_summary"]["total_files"] = files.size();
  root["project_summary"]["total_symbols"] = symbols.size();
  root["project_summary"]["total_file_dependencies"] =
      deps.fileEdges.size();
  root["project_summary"]["total_symbol_dependencies"] =
      deps.symbolEdges.size();

  root["dependency_graph_summary"]["file_edges"] =
      deps.fileEdges.size();
  root["dependency_graph_summary"]["symbol_edges"] =
      deps.symbolEdges.size();

  // -----------------------------
  // Canonicalize JSON BEFORE writing
  // -----------------------------
  nlohmann::json canonical = canonicalizeJson(root);

  std::ofstream output(outputPath,
                       std::ios::binary | std::ios::trunc);
  if (!output) {
    error =
        "Failed to open agent_context.json for writing: "
        + outputPath.string();
    return false;
  }

  output << canonical.dump(2);  // pretty stable output

  if (!output) {
    error =
        "Failed writing agent_context.json: "
        + outputPath.string();
    return false;
  }

  return true;
}

}  // namespace ultra::ai