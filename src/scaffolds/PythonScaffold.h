#pragma once

#include "ScaffoldBase.h"

namespace ultra::scaffolds {

class PythonScaffold final : public ScaffoldBase {
 public:
  explicit PythonScaffold(IScaffoldEnvironment& environment);

  void generate(const std::string& name,
                const ScaffoldOptions& options) override;
};

}  // namespace ultra::scaffolds
