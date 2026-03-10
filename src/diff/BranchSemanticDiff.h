#pragma once

#include <string>
#include <vector>

namespace ultra::diff::semantic {

using NodeID = std::string;

enum class DiffType {
  Added = 0,
  Removed = 1,
  Renamed = 2,
  Moved = 3,
  Modified = 4
};

struct SymbolDiff {
  NodeID id;
  DiffType type{DiffType::Modified};
};

enum class SignatureChange {
  Signature = 0,
  Visibility = 1,
  Relocation = 2,
  Rename = 3
};

struct SignatureDiff {
  NodeID id;
  SignatureChange change{SignatureChange::Signature};
};

struct DependencyDiff {
  NodeID from;
  NodeID to;
  DiffType type{DiffType::Modified};
};

enum class RiskLevel {
  LOW = 0,
  MEDIUM = 1,
  HIGH = 2
};

struct BranchDiffReport {
  std::vector<SymbolDiff> symbols;
  std::vector<SignatureDiff> signatures;
  std::vector<DependencyDiff> dependencies;
  RiskLevel overallRisk{RiskLevel::LOW};
  double impactScore{0.0};
};

inline const char* toString(const DiffType type) {
  switch (type) {
    case DiffType::Added:
      return "Added";
    case DiffType::Removed:
      return "Removed";
    case DiffType::Renamed:
      return "Renamed";
    case DiffType::Moved:
      return "Moved";
    case DiffType::Modified:
      return "Modified";
  }
  return "Modified";
}

inline const char* toString(const SignatureChange change) {
  switch (change) {
    case SignatureChange::Signature:
      return "SIGNATURE";
    case SignatureChange::Visibility:
      return "VISIBILITY";
    case SignatureChange::Relocation:
      return "RELOCATION";
    case SignatureChange::Rename:
      return "RENAME";
  }
  return "SIGNATURE";
}

inline const char* toString(const RiskLevel level) {
  switch (level) {
    case RiskLevel::LOW:
      return "LOW";
    case RiskLevel::MEDIUM:
      return "MEDIUM";
    case RiskLevel::HIGH:
      return "HIGH";
  }
  return "LOW";
}

}  // namespace ultra::diff::semantic

