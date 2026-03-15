#pragma once

#include "ILanguageAdapter.h"
#include <filesystem>
#include <memory>
//E:\Projects\Ultra\src\language\AdapterFactory.h
namespace ultra::language {

class AdapterFactory {
 public:
  static std::unique_ptr<ILanguageAdapter> create(
      const std::filesystem::path& root);
};

}  // namespace ultra::language
