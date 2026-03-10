#pragma once

#include "../core/ProjectType.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ultra::config {

struct ModuleConfig {
  std::string name;
  std::filesystem::path path;
  ultra::core::ProjectType type{ultra::core::ProjectType::Unknown};
};

struct UltraConfig {
  std::string version;
  std::vector<ModuleConfig> modules;
};

class ConfigLoader {
 public:
  explicit ConfigLoader(std::filesystem::path root);

  bool load();

  bool hasConfig() const noexcept;
  bool valid() const noexcept;
  const UltraConfig& config() const noexcept;
  const std::filesystem::path& configPath() const noexcept;
  const std::string& lastError() const noexcept;

 private:
  bool parse(const std::string& content);
  bool parseModules(const std::string& modulesBody);
  void fail(std::string message);

  static std::optional<std::string> extractStringField(const std::string& content,
                                                        const std::string& key);
  static std::optional<std::string> extractObjectField(const std::string& content,
                                                       const std::string& key);
  static void skipWhitespace(const std::string& text, std::size_t& pos);
  static bool consumeQuotedString(const std::string& text,
                                  std::size_t& pos,
                                  std::string& out);
  static bool findMatchingBrace(const std::string& text,
                                std::size_t openPos,
                                std::size_t& closePos);

  std::filesystem::path m_root;
  std::filesystem::path m_configPath;
  bool m_hasConfig{false};
  bool m_valid{false};
  UltraConfig m_config;
  std::string m_lastError;
};

}  // namespace ultra::config
