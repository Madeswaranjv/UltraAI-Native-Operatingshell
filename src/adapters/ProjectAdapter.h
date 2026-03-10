#pragma once

#include "../cli/CommandOptions.h"

namespace ultra::adapters {

class ProjectAdapter {
 public:
  virtual void install(const ultra::cli::CommandOptions& options) = 0;
  virtual void dev(const ultra::cli::CommandOptions& options) = 0;
  virtual void build(const ultra::cli::CommandOptions& options) = 0;
  virtual void test(const ultra::cli::CommandOptions& options) = 0;
  virtual void run(const ultra::cli::CommandOptions& options) = 0;
  virtual void clean(const ultra::cli::CommandOptions& options) = 0;
  virtual int lastExitCode() const noexcept = 0;
  virtual ~ProjectAdapter() = default;
};

}  // namespace ultra::adapters
