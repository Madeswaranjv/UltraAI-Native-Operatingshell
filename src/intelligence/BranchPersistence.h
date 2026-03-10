#pragma once
//E:\Projects\Ultra\src\intelligence\BranchPersistence.h
#include "BranchStore.h"
#include <filesystem>

namespace ultra::intelligence {

/// Serializes and deserializes the branch registry to disk.
class BranchPersistence {
 public:
  explicit BranchPersistence(const std::filesystem::path& baseDir);

  /// Saves the entire store to disk.
  bool save(const BranchStore& store) const;

  /// Loads branches from disk into the store.
  bool load(BranchStore& store) const;

 private:
  std::filesystem::path branchesDir_;
};

}  // namespace ultra::intelligence
