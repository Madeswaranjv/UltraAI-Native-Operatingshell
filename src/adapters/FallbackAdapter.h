#pragma once

#include "ProjectAdapter.h"
#include "../core/ProjectType.h"
#include <filesystem>
#include <string>

namespace ultra::adapters {

class FallbackAdapter final : public ProjectAdapter {
 public:
  FallbackAdapter(std::filesystem::path rootPath,
                  ultra::core::ProjectType projectType);

  void install(const ultra::cli::CommandOptions& options) override;
  void dev(const ultra::cli::CommandOptions& options) override;
  void build(const ultra::cli::CommandOptions& options) override;
  void test(const ultra::cli::CommandOptions& options) override;
  void run(const ultra::cli::CommandOptions& options) override;
  void clean(const ultra::cli::CommandOptions& options) override;
  int lastExitCode() const noexcept override;

 private:
  enum class Action { Install, Dev, Build, Test, Run, Clean };

  void executeAction(Action action, const ultra::cli::CommandOptions& options);
  void cleanPythonProject(const ultra::cli::CommandOptions& options);
  std::string commandFor(Action action,
                         const ultra::cli::CommandOptions& options) const;
  bool ensurePythonAvailable(const ultra::cli::CommandOptions& options) const;
  bool ensureDjangoTooling(const ultra::cli::CommandOptions& options) const;
  bool isDjangoProject() const;

  std::filesystem::path m_rootPath;
  ultra::core::ProjectType m_projectType;
  int m_lastExitCode{0};
};

}  // namespace ultra::adapters
