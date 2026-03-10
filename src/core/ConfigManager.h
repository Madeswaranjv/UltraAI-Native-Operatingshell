#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::core {

class ConfigManager {
 public:
  explicit ConfigManager(const std::filesystem::path& configPath);

  const std::vector<std::string>& ignoreDirs() const noexcept;
  const std::vector<std::string>& supportedExtensions() const noexcept;
  const std::vector<std::string>& vendorPatterns() const noexcept;
  std::size_t maxFileSizeKb() const noexcept;
  bool loaded() const noexcept;

 private:
  void load();
  void parseIgnoreDirs(const std::string& content);
  void parseSupportedExtensions(const std::string& content);
  void parseVendorPatterns(const std::string& content);
  void parseMaxFileSizeKb(const std::string& content);
  static std::string extractArrayValues(const std::string& content,
                                        const std::string& key);
  static std::string extractStringValue(const std::string& content,
                                       const std::string& key);

  std::filesystem::path m_configPath;
  std::vector<std::string> m_ignoreDirs;
  std::vector<std::string> m_supportedExtensions;
  std::vector<std::string> m_vendorPatterns;
  std::size_t m_maxFileSizeKb{200};
  bool m_loaded{false};
};

}  // namespace ultra::core
