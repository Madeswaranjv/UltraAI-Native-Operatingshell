#include "ChangeSimulator.h"

#include "../query/QueryPlanner.h"

#include <string>
#include <vector>

namespace ultra::engine::impact {

namespace {

std::string breakageToken(const ImpactedSymbol& symbol) {
  if (!symbol.definedIn.empty()) {
    return symbol.definedIn + "::" + symbol.name;
  }
  return symbol.name;
}

}  // namespace

SimulationResult ChangeSimulator::simulateSymbolChange(
    const ImpactPrediction& prediction) const {
  SimulationResult result;
  result.prediction = prediction;
  result.potentialBreakages = collectPotentialBreakages(prediction);
  result.runtimeStateMutated = false;
  return result;
}

SimulationResult ChangeSimulator::simulateFileChange(
    const ImpactPrediction& prediction) const {
  SimulationResult result;
  result.prediction = prediction;
  result.potentialBreakages = collectPotentialBreakages(prediction);
  result.runtimeStateMutated = false;
  return result;
}

std::vector<std::string> ChangeSimulator::collectPotentialBreakages(
    const ImpactPrediction& prediction) {
  std::vector<std::string> breakages;
  for (const ImpactedSymbol& symbol : prediction.symbols) {
    if (symbol.name.empty()) {
      continue;
    }
    if (symbol.publicApi || (!symbol.isRoot && symbol.depth <= 1U)) {
      breakages.push_back(breakageToken(symbol));
    }
  }

  if (breakages.empty()) {
    for (const ImpactedFile& file : prediction.files) {
      if (!file.isRoot && !file.path.empty()) {
        breakages.push_back(file.path);
      }
    }
  }

  query::QueryPlanner::sortAndDedupe(breakages);
  return breakages;
}

}  // namespace ultra::engine::impact
