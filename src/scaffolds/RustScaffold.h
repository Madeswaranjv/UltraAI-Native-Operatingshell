#pragma once

#include "ScaffoldBase.h"

namespace ultra::scaffolds {

class RustScaffold final : public ScaffoldBase {
 public:
  explicit RustScaffold(IScaffoldEnvironment& environment);

  void generate(const std::string& name,
                const ScaffoldOptions& options) override;
};

}  // namespace ultra::scaffolds
