#pragma once

#include "ProjectAdapter.h"
#include <filesystem>
#include <string>

namespace ultra::adapters {

class RustAdapter final : public ProjectAdapter {
 public:
  explicit RustAdapter(std::filesystem::path rootPath);

  void install(const ultra::cli::CommandOptions& options) override;
  void dev(const ultra::cli::CommandOptions& options) override;
  void build(const ultra::cli::CommandOptions& options) override;
  void test(const ultra::cli::CommandOptions& options) override;
  void run(const ultra::cli::CommandOptions& options) override;
  void clean(const ultra::cli::CommandOptions& options) override;
  int lastExitCode() const noexcept override;

 private:
  bool ensureCargoAvailable(const ultra::cli::CommandOptions& options) const;
  int execute(const std::string& command,
              const ultra::cli::CommandOptions& options);

  std::filesystem::path m_rootPath;
  int m_lastExitCode{0};
};

}  // namespace ultra::adapters
