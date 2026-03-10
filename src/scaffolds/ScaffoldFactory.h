#pragma once

#include "ScaffoldBase.h"
#include <memory>
#include <string>
#include <vector>

namespace ultra::scaffolds {

std::vector<std::string> supportedStacks();

bool isSupportedStack(const std::string& stack);

std::unique_ptr<ScaffoldBase> createScaffold(const std::string& stack,
                                             IScaffoldEnvironment& environment);

}  // namespace ultra::scaffolds
