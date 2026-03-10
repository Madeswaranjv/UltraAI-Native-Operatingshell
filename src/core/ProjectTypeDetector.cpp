#include "ProjectTypeDetector.h"
#include <algorithm>
#include <cctype>

namespace ultra::core {

namespace {

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

}  // namespace

bool ProjectTypeDetector::hasFile(const std::filesystem::path& root,
                                  const char* fileName) noexcept {
  const std::filesystem::path filePath = root / fileName;
  return std::filesystem::exists(filePath) &&
         std::filesystem::is_regular_file(filePath);
}

ProjectType ProjectTypeDetector::detect(const std::filesystem::path& root) {
  if (hasFile(root, "package.json")) {
    return ProjectType::React;
  }
  if (hasFile(root, "CMakeLists.txt")) {
    return ProjectType::CMake;
  }
  if (hasFile(root, "Cargo.toml")) {
    return ProjectType::Rust;
  }
  if (hasFile(root, "Makefile")) {
    return ProjectType::Make;
  }
  if (hasFile(root, "pyproject.toml")) {
    return ProjectType::Python;
  }
  return ProjectType::Unknown;
}

std::optional<ProjectType> ProjectTypeDetector::fromString(
    const std::string& value) {
  const std::string normalized = toLower(value);
  if (normalized == "react" || normalized == "node") {
    return ProjectType::React;
  }
  if (normalized == "cmake") {
    return ProjectType::CMake;
  }
  if (normalized == "rust") {
    return ProjectType::Rust;
  }
  if (normalized == "make") {
    return ProjectType::Make;
  }
  if (normalized == "python" || normalized == "py") {
    return ProjectType::Python;
  }
  if (normalized == "unknown") {
    return ProjectType::Unknown;
  }
  return std::nullopt;
}

std::string ProjectTypeDetector::toString(ProjectType type) {
  switch (type) {
    case ProjectType::React:
      return "react";
    case ProjectType::CMake:
      return "cmake";
    case ProjectType::Rust:
      return "rust";
    case ProjectType::Make:
      return "make";
    case ProjectType::Python:
      return "python";
    case ProjectType::Unknown:
    default:
      return "unknown";
  }
}

}  // namespace ultra::core
