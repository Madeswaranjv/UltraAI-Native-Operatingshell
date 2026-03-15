#pragma once
//E:\Projects\Ultra\src\language\CppAdapter.h
#include "ILanguageAdapter.h"
#include <memory>

namespace ultra::language {

class CppAdapter : public ILanguageAdapter {
 public:
  std::vector<ultra::scanner::FileInfo> scan(
      const std::filesystem::path& root) override;

  ultra::graph::DependencyGraph buildGraph(
      const std::vector<ultra::scanner::FileInfo>& files) override;

  void analyze(const std::filesystem::path& root) override;
  void build(const std::filesystem::path& root) override;
  void buildIncremental(const std::filesystem::path& root) override;
  void buildFast(const std::filesystem::path& root) override;
  nlohmann::json generateContext(const std::filesystem::path& root) override;
  nlohmann::json generateContextWithAst(
      const std::filesystem::path& root) override;
  bool applyPatch(const std::filesystem::path& root,
                  const std::filesystem::path& diffFile) override;

  int getLastBuildExitCode() const override { return lastBuildExitCode_; }

 private:
  int lastBuildExitCode_{0};
};

}  // namespace ultra::language
