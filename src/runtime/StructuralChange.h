#pragma once

namespace ultra::runtime {

enum class StructuralChangeType {
  BODY_CHANGE,
  SIGNATURE_CHANGE,
  API_REMOVAL,
  DEPENDENCY_CHANGE,
  SYMBOL_RENAME
};

}  // namespace ultra::runtime

