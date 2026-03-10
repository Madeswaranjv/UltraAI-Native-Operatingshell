#include "ScaffoldFactory.h"
#include "CMakeScaffold.h"
#include "DjangoScaffold.h"
#include "NextScaffold.h"
#include "PythonScaffold.h"
#include "ReactScaffold.h"
#include "RustScaffold.h"
#include <algorithm>
#include <cctype>

namespace ultra::scaffolds {

namespace {

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

}  // namespace

std::vector<std::string> supportedStacks() {
  return {"react", "next", "django", "python", "cmake", "rust"};
}

bool isSupportedStack(const std::string& stack) {
  const std::string normalized = toLower(stack);
  const std::vector<std::string> stacks = supportedStacks();
  return std::find(stacks.begin(), stacks.end(), normalized) != stacks.end();
}

std::unique_ptr<ScaffoldBase> createScaffold(const std::string& stack,
                                             IScaffoldEnvironment& environment) {
  const std::string normalized = toLower(stack);
  if (normalized == "react") {
    return std::make_unique<ReactScaffold>(environment);
  }
  if (normalized == "next") {
    return std::make_unique<NextScaffold>(environment);
  }
  if (normalized == "django") {
    return std::make_unique<DjangoScaffold>(environment);
  }
  if (normalized == "python") {
    return std::make_unique<PythonScaffold>(environment);
  }
  if (normalized == "cmake") {
    return std::make_unique<CMakeScaffold>(environment);
  }
  if (normalized == "rust") {
    return std::make_unique<RustScaffold>(environment);
  }
  return nullptr;
}

}  // namespace ultra::scaffolds
