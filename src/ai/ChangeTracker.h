#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::ai {

enum class ChangeType : std::uint8_t {
  Added = 1,
  Modified = 2,
  Deleted = 3
};

struct ChangeLogRecord {
  std::uint32_t fileId{0};
  ChangeType changeType{ChangeType::Added};
  std::uint64_t timestamp{0};
};

class ChangeTracker {
 public:
  static bool append(const std::filesystem::path& logPath,
                     const std::vector<ChangeLogRecord>& records,
                     std::string& error);
  static bool readAll(const std::filesystem::path& logPath,
                      std::vector<ChangeLogRecord>& outRecords,
                      std::string& error);
};

}  // namespace ultra::ai

