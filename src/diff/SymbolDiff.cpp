#include "SymbolDiff.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace ultra::diff {

namespace {

// Deterministic ordering inside name groups
bool symbolRecordLess(const ultra::ai::SymbolRecord& a,
                      const ultra::ai::SymbolRecord& b) {
  if (a.symbolType != b.symbolType)
    return static_cast<int>(a.symbolType) <
           static_cast<int>(b.symbolType);

  if (a.visibility != b.visibility)
    return static_cast<int>(a.visibility) <
           static_cast<int>(b.visibility);

  return a.signature < b.signature;
}

// Structural equality (ignores symbolId, fileId, lineNumber)
bool structurallyEqual(const ultra::ai::SymbolRecord& a,
                       const ultra::ai::SymbolRecord& b) {
  return a.symbolType == b.symbolType &&
         a.visibility == b.visibility &&
         a.signature == b.signature;
}

} // namespace


std::vector<SymbolDelta> SymbolDiff::compare(
    const std::vector<ultra::ai::SymbolRecord>& oldSymbols,
    const std::vector<ultra::ai::SymbolRecord>& newSymbols) {

  std::vector<SymbolDelta> deltas;

  // Group by name (name = identity)
  std::map<std::string, std::vector<ultra::ai::SymbolRecord>> oldGrouped;
  std::map<std::string, std::vector<ultra::ai::SymbolRecord>> newGrouped;

  for (const auto& s : oldSymbols)
    oldGrouped[s.name].push_back(s);

  for (const auto& s : newSymbols)
    newGrouped[s.name].push_back(s);

  // Deterministic ordering inside each name group
  for (auto& [_, vec] : oldGrouped)
    std::sort(vec.begin(), vec.end(), symbolRecordLess);

  for (auto& [_, vec] : newGrouped)
    std::sort(vec.begin(), vec.end(), symbolRecordLess);

  // Union of all names
  std::set<std::string> allNames;
  for (const auto& [n, _] : oldGrouped) allNames.insert(n);
  for (const auto& [n, _] : newGrouped) allNames.insert(n);

  const std::vector<ultra::ai::SymbolRecord> emptyVec;

  for (const auto& name : allNames) {

    const auto& oldVec =
        (oldGrouped.count(name)) ? oldGrouped[name] : emptyVec;

    const auto& newVec =
        (newGrouped.count(name)) ? newGrouped[name] : emptyVec;

    size_t oldSize = oldVec.size();
    size_t newSize = newVec.size();
    size_t minSize = std::min(oldSize, newSize);

    // Compare pairwise (same name only)
    for (size_t i = 0; i < minSize; ++i) {
      const auto& oldRec = oldVec[i];
      const auto& newRec = newVec[i];

      if (!structurallyEqual(oldRec, newRec)) {
        SymbolDelta delta;
        delta.symbolName = name;
        delta.changeType = ultra::types::ChangeType::Modified;
        delta.oldRecord = oldRec;
        delta.newRecord = newRec;
        deltas.push_back(delta);
      }
    }

    // Extra old → Removed
    for (size_t i = minSize; i < oldSize; ++i) {
      SymbolDelta delta;
      delta.symbolName = name;
      delta.changeType = ultra::types::ChangeType::Removed;
      delta.oldRecord = oldVec[i];
      deltas.push_back(delta);
    }

    // Extra new → Added
    for (size_t i = minSize; i < newSize; ++i) {
      SymbolDelta delta;
      delta.symbolName = name;
      delta.changeType = ultra::types::ChangeType::Added;
      delta.newRecord = newVec[i];
      deltas.push_back(delta);
    }
  }

  // Final deterministic ordering
  std::sort(deltas.begin(), deltas.end(),
            [](const SymbolDelta& a, const SymbolDelta& b) {
              if (a.symbolName != b.symbolName)
                return a.symbolName < b.symbolName;
              return static_cast<int>(a.changeType) <
                     static_cast<int>(b.changeType);
            });

  return deltas;
}

} // namespace ultra::diff