#pragma once

#include "ScaffoldBase.h"

namespace ultra::scaffolds {

class CMakeScaffold final : public ScaffoldBase {
 public:
  explicit CMakeScaffold(IScaffoldEnvironment& environment);

  void generate(const std::string& name,
                const ScaffoldOptions& options) override;
};

}  // namespace ultra::scaffolds
