#pragma once

#include "ScaffoldBase.h"

namespace ultra::scaffolds {

class ReactScaffold final : public ScaffoldBase {
 public:
  explicit ReactScaffold(IScaffoldEnvironment& environment);

  void generate(const std::string& name,
                const ScaffoldOptions& options) override;
};

}  // namespace ultra::scaffolds
