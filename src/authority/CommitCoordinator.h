#pragma once

#include <filesystem>
#include <string>

namespace ultra::authority {

struct AuthorityCommitRequest;
struct AuthorityRiskReport;

class CommitCoordinator {
 public:
  explicit CommitCoordinator(
      std::filesystem::path projectRoot = std::filesystem::current_path());

  [[nodiscard]] bool commit(const AuthorityCommitRequest& request,
                            const AuthorityRiskReport& riskReport,
                            std::string& error) const;

 private:
  std::filesystem::path projectRoot_;
};

}  // namespace ultra::authority
