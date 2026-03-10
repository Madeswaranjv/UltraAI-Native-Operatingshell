#include "SignatureDiff.h"
#include "../memory/SemanticMemory.h"

#include <sstream>
//SignatureDiff.cpp
namespace ultra::diff {

namespace {

std::string symbolNodeId(const SymbolDelta& delta) {
  std::uint64_t symbolId = delta.newRecord.symbolId;
  if (symbolId == 0ULL) {
    symbolId = delta.oldRecord.symbolId;
  }
  if (symbolId == 0ULL) {
    return {};
  }
  std::ostringstream stream;
  stream << "symbol:" << symbolId;
  return stream.str();
}

}  // namespace

std::vector<ContractBreak> SignatureDiff::analyze(
    const std::vector<SymbolDelta>& deltas,
    const ultra::graph::DependencyGraph& depGraph,
    ultra::memory::SemanticMemory* semanticMemory,
    const std::uint64_t semanticVersion) {
  
  std::vector<ContractBreak> breaks;
  
  (void)depGraph;

  for (const auto& delta : deltas) {
    if (delta.changeType == ultra::types::ChangeType::Removed) {
      ContractBreak cb;
      cb.functionName = delta.symbolName;
      cb.breakType = BreakType::Removed;
      cb.severity = 1.0;
      cb.description = "Symbol '" + delta.symbolName + "' was removed. All callers will break.";
      breaks.push_back(cb);
      if (semanticMemory != nullptr) {
        const std::string nodeId = symbolNodeId(delta);
        if (!nodeId.empty()) {
          semanticMemory->trackSymbolEvolution(
              nodeId, delta.symbolName, delta.oldRecord.signature, "api_removed",
              semanticVersion);
        }
      }
    } else if (delta.changeType == ultra::types::ChangeType::Modified) {
      bool isBreaking = false;
      ContractBreak cb;
      cb.functionName = delta.symbolName;

      // Check visibility restriction (e.g., Public -> Private)
      if (delta.oldRecord.visibility == ultra::ai::Visibility::Public &&
          delta.newRecord.visibility != ultra::ai::Visibility::Public) {
        cb.breakType = BreakType::VisibilityChange;
        cb.severity = 0.9;
        cb.description = "Visibility restricted from Public to " + 
                         std::to_string(static_cast<int>(delta.newRecord.visibility));
        isBreaking = true;
      }
      // Check signature differences
      else if (delta.oldRecord.signature != delta.newRecord.signature) {
        // A naive heuristic for now. In a full AST we'd inspect params & ret type exactly.
        cb.breakType = BreakType::ParameterChange;
        cb.severity = 0.8;
        cb.description = "Signature changed from '" + delta.oldRecord.signature + 
                         "' to '" + delta.newRecord.signature + "'";
        isBreaking = true;
      }
      // Check symbol type changes
      else if (delta.oldRecord.symbolType != delta.newRecord.symbolType) {
        cb.breakType = BreakType::TypeChange;
        cb.severity = 1.0;
        cb.description = "Symbol type changed (e.g., from Function to Class)";
        isBreaking = true;
      }

      if (isBreaking) {
        breaks.push_back(cb);
        if (semanticMemory != nullptr) {
          const std::string nodeId = symbolNodeId(delta);
          if (!nodeId.empty()) {
            semanticMemory->trackSymbolEvolution(
                nodeId, delta.symbolName, delta.newRecord.signature,
                "signature_change", semanticVersion);
          }
        }
      }
    }
  }

  // Find affected callers (fan-out)
  for (auto& cb : breaks) {
    (void)cb;
    // For now, if the symbol has an associated file, we'll mark the file dependencies
    // finding exactly which function called it requires a call graph. We'll use the file-level depGraph.
    // We would need the file path for the symbol, but our symbol record has fileId.
    // In a mature impl, we'd map fileId -> filePath -> depGraph.getDependents(filePath).
    // For simplicity, we leave affectedCallers empty or stubbed pending full call graph.
  }

  return breaks;
}

}  // namespace ultra::diff
