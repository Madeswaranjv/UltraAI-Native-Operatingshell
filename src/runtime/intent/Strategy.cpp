#include "Strategy.h"

namespace ultra::runtime::intent {

std::string toString(const ActionKind kind) {
  switch (kind) {
    case ActionKind::ModifySymbolBody:
      return "ModifySymbolBody";
    case ActionKind::RefactorModule:
      return "RefactorModule";
    case ActionKind::ReduceImpactRadius:
      return "ReduceImpactRadius";
    case ActionKind::ImproveCentrality:
      return "ImproveCentrality";
    case ActionKind::MinimizeTokenUsage:
      return "MinimizeTokenUsage";
    case ActionKind::AddDependency:
      return "AddDependency";
    case ActionKind::RemoveDependency:
      return "RemoveDependency";
    case ActionKind::RenameSymbol:
      return "RenameSymbol";
    case ActionKind::ChangeSignature:
      return "ChangeSignature";
    case ActionKind::UpdatePublicAPI:
      return "UpdatePublicAPI";
    case ActionKind::MoveAcrossModules:
      return "MoveAcrossModules";
  }
  return "ModifySymbolBody";
}

}  // namespace ultra::runtime::intent

