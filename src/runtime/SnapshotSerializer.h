#pragma once

#include "../memory/StateSnapshot.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace ultra::runtime {

struct SnapshotHeader {
  std::uint32_t formatVersion{1U};
  std::uint64_t snapshotVersion{0ULL};
};

class SnapshotSerializer {
 public:
  static constexpr std::uint32_t kFormatVersion = 1U;

  static bool save(const memory::StateSnapshot& snapshot,
                   const std::filesystem::path& path);
  static bool load(const std::filesystem::path& path,
                   memory::StateSnapshot& snapshotOut);

 private:
  static bool writeString(std::ostream& output, const std::string& value);
  static bool readString(std::istream& input, std::string& valueOut);
};

}  // namespace ultra::runtime

