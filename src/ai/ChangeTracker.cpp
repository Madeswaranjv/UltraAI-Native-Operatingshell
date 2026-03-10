#include "ChangeTracker.h"

#include <array>
#include <fstream>

namespace ultra::ai {

namespace {

void writeUint32(std::ofstream& output, const std::uint32_t value) {
  output.put(static_cast<char>(value & 0xFFU));
  output.put(static_cast<char>((value >> 8U) & 0xFFU));
  output.put(static_cast<char>((value >> 16U) & 0xFFU));
  output.put(static_cast<char>((value >> 24U) & 0xFFU));
}

void writeUint64(std::ofstream& output, const std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    output.put(static_cast<char>((value >> (static_cast<std::uint64_t>(i) * 8ULL)) &
                                 0xFFULL));
  }
}

bool readUint32(std::ifstream& input, std::uint32_t& value) {
  std::array<std::uint8_t, 4> bytes{};
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()))) {
    return false;
  }
  value = static_cast<std::uint32_t>(bytes[0]) |
          (static_cast<std::uint32_t>(bytes[1]) << 8U) |
          (static_cast<std::uint32_t>(bytes[2]) << 16U) |
          (static_cast<std::uint32_t>(bytes[3]) << 24U);
  return true;
}

bool readUint64(std::ifstream& input, std::uint64_t& value) {
  std::array<std::uint8_t, 8> bytes{};
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()))) {
    return false;
  }
  value = 0ULL;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[static_cast<std::size_t>(i)])
             << (static_cast<std::uint64_t>(i) * 8ULL);
  }
  return true;
}

}  // namespace

bool ChangeTracker::append(const std::filesystem::path& logPath,
                           const std::vector<ChangeLogRecord>& records,
                           std::string& error) {
  if (records.empty()) {
    return true;
  }

  std::ofstream output(logPath, std::ios::binary | std::ios::app);
  if (!output) {
    error = "Failed to open changes.log for append: " + logPath.string();
    return false;
  }

  for (const ChangeLogRecord& record : records) {
    writeUint32(output, record.fileId);
    output.put(static_cast<char>(record.changeType));
    writeUint64(output, record.timestamp);
  }

  if (!output) {
    error = "Failed to write changes.log: " + logPath.string();
    return false;
  }

  return true;
}

bool ChangeTracker::readAll(const std::filesystem::path& logPath,
                            std::vector<ChangeLogRecord>& outRecords,
                            std::string& error) {
  outRecords.clear();
  if (!std::filesystem::exists(logPath)) {
    return true;
  }

  std::ifstream input(logPath, std::ios::binary);
  if (!input) {
    error = "Failed to open changes.log: " + logPath.string();
    return false;
  }

  while (true) {
    ChangeLogRecord record;
    std::uint32_t fileId = 0;
    if (!readUint32(input, fileId)) {
      if (input.eof()) {
        break;
      }
      error = "Failed reading file_id from changes.log";
      return false;
    }
    record.fileId = fileId;

    char typeByte = 0;
    if (!input.get(typeByte)) {
      error = "Failed reading change_type from changes.log";
      return false;
    }
    record.changeType = static_cast<ChangeType>(static_cast<std::uint8_t>(typeByte));

    std::uint64_t timestamp = 0;
    if (!readUint64(input, timestamp)) {
      error = "Failed reading timestamp from changes.log";
      return false;
    }
    record.timestamp = timestamp;
    outRecords.push_back(record);
  }

  return true;
}

}  // namespace ultra::ai
