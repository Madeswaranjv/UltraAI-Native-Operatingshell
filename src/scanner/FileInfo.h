#pragma once
//E:\Projects\Ultra\src\scanner\FileInfo.h
#include <cstdint>
#include <filesystem>

namespace ultra::scanner {

enum class FileType {
  Source,
  Header,
  Build,
  Config,
  Documentation,
  Script,
  Other
};

struct FileInfo {
  std::filesystem::path path;
  FileType type{FileType::Other};
  std::uintmax_t size{0};
};

}  // namespace ultra::scanner