#pragma once

#include "DependencyTable.h"
#include "FileRegistry.h"
#include "Hashing.h"
#include "SymbolTable.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::ai {

struct CoreIndex {
  std::uint32_t magic{0};
  std::uint32_t indexVersion{0};
  std::uint32_t schemaVersion{0};
  std::uint8_t runtimeActive{0};
  std::array<std::uint8_t, 3> reserved{};
  Sha256Hash filesTblHash{};
  Sha256Hash symbolsTblHash{};
  Sha256Hash depsTblHash{};
  Sha256Hash projectRootHash{};
  Sha256Hash indexHash{};
};

class IntegrityManager {
 public:
  static constexpr std::uint32_t kCoreMagic = 0x52494155U;  // "UAIR"
  static constexpr std::uint32_t kIndexVersion = 1U;
  static constexpr std::uint32_t kSchemaVersion = 2U;

  static CoreIndex buildCoreIndex(bool runtimeActive,
                                  const Sha256Hash& filesTblHash,
                                  const Sha256Hash& symbolsTblHash,
                                  const Sha256Hash& depsTblHash,
                                  const Sha256Hash& projectRootHash,
                                  const Sha256Hash& indexHash);

  static Sha256Hash computeProjectRootHash(
      const std::vector<FileRecord>& files);
  static bool computeTableHash(const std::filesystem::path& tablePath,
                               Sha256Hash& outHash,
                               std::string& error);
  static bool computeIndexHash(const std::filesystem::path& filesTablePath,
                               const std::filesystem::path& symbolsTablePath,
                               const std::filesystem::path& depsTablePath,
                               Sha256Hash& outHash,
                               std::string& error);

  static bool verify(const CoreIndex& core,
                     const Sha256Hash& filesTblHash,
                     const Sha256Hash& symbolsTblHash,
                     const Sha256Hash& depsTblHash,
                     const Sha256Hash& projectRootHash,
                     const Sha256Hash& indexHash,
                     std::string& error);
};

}  // namespace ultra::ai

