#pragma once

#include <filesystem>

namespace ultra::authority {

struct AuthorityContextRequest;
struct AuthorityContextResult;

class ContextSliceProvider {
 public:
  explicit ContextSliceProvider(
      std::filesystem::path projectRoot = std::filesystem::current_path());

  [[nodiscard]] AuthorityContextResult getSlice(
      const AuthorityContextRequest& request) const;

 private:
  std::filesystem::path projectRoot_;
};

}  // namespace ultra::authority
