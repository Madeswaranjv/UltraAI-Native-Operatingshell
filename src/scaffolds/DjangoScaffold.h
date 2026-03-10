#pragma once

#include "ScaffoldBase.h"

namespace ultra::scaffolds {

class DjangoScaffold final : public ScaffoldBase {
 public:
  explicit DjangoScaffold(IScaffoldEnvironment& environment);

  void generate(const std::string& name,
                const ScaffoldOptions& options) override;
};

}  // namespace ultra::scaffolds
