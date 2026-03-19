#pragma once

#include "model_types.h"

namespace ultra::runtime::model {

class IModel {
 public:
  virtual ~IModel() = default;
  [[nodiscard]] virtual ModelResponse generate(
      const ModelRequest& request) = 0;
};

}  // namespace ultra::runtime::model

