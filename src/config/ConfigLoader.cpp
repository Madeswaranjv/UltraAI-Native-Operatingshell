#include "ConfigLoader.h"
#include "../core/ProjectTypeDetector.h"
#include <fstream>
#include <sstream>
#include <utility>

namespace ultra::config {

ConfigLoader::ConfigLoader(std::filesystem::path root)
    : m_root(std::move(root)), m_configPath(m_root / "ultra.config.json") {}

bool ConfigLoader::load() {
  m_hasConfig = false;
  m_valid = false;
  m_config = UltraConfig{};
  m_lastError.clear();

  if (!std::filesystem::exists(m_configPath)) {
    m_valid = true;
    return true;
  }

  m_hasConfig = true;
  if (!std::filesystem::is_regular_file(m_configPath)) {
    fail("ultra.config.json exists but is not a file.");
    return false;
  }

  std::ifstream input(m_configPath);
  if (!input) {
    fail("Failed to open ultra.config.json.");
    return false;
  }

  std::stringstream buffer;
  buffer << input.rdbuf();
  const std::string content = buffer.str();

  m_valid = parse(content);
  return m_valid;
}

bool ConfigLoader::hasConfig() const noexcept {
  return m_hasConfig;
}

bool ConfigLoader::valid() const noexcept {
  return m_valid;
}

const UltraConfig& ConfigLoader::config() const noexcept {
  return m_config;
}

const std::filesystem::path& ConfigLoader::configPath() const noexcept {
  return m_configPath;
}

const std::string& ConfigLoader::lastError() const noexcept {
  return m_lastError;
}

bool ConfigLoader::parse(const std::string& content) {
  const auto version = extractStringField(content, "version");
  if (!version.has_value()) {
    fail("ultra.config.json must contain string field 'version'.");
    return false;
  }

  m_config.version = version.value();
  if (m_config.version != "1.0") {
    fail("ultra.config.json version must be '1.0'.");
    return false;
  }

  const auto modules = extractObjectField(content, "modules");
  if (!modules.has_value()) {
    fail("ultra.config.json must contain object field 'modules'.");
    return false;
  }

  return parseModules(modules.value());
}

bool ConfigLoader::parseModules(const std::string& modulesBody) {
  m_config.modules.clear();

  std::size_t pos = 0;
  while (pos < modulesBody.size()) {
    skipWhitespace(modulesBody, pos);
    while (pos < modulesBody.size() && modulesBody[pos] == ',') {
      ++pos;
      skipWhitespace(modulesBody, pos);
    }
    if (pos >= modulesBody.size()) {
      break;
    }

    std::string moduleName;
    if (!consumeQuotedString(modulesBody, pos, moduleName)) {
      fail("Invalid module name in ultra.config.json 'modules' object.");
      return false;
    }

    skipWhitespace(modulesBody, pos);
    if (pos >= modulesBody.size() || modulesBody[pos] != ':') {
      fail("Expected ':' after module name '" + moduleName + "'.");
      return false;
    }
    ++pos;

    skipWhitespace(modulesBody, pos);
    if (pos >= modulesBody.size() || modulesBody[pos] != '{') {
      fail("Module '" + moduleName + "' must be an object.");
      return false;
    }

    std::size_t closePos = 0;
    if (!findMatchingBrace(modulesBody, pos, closePos)) {
      fail("Module '" + moduleName + "' object is malformed.");
      return false;
    }

    const std::string moduleBody =
        modulesBody.substr(pos + 1, closePos - pos - 1);
    pos = closePos + 1;

    const auto pathValue = extractStringField(moduleBody, "path");
    if (!pathValue.has_value()) {
      fail("Module '" + moduleName + "' is missing string field 'path'.");
      return false;
    }

    const auto typeValue = extractStringField(moduleBody, "type");
    if (!typeValue.has_value()) {
      fail("Module '" + moduleName + "' is missing string field 'type'.");
      return false;
    }

    const auto moduleType = ultra::core::ProjectTypeDetector::fromString(
        typeValue.value());
    if (!moduleType.has_value() ||
        moduleType.value() == ultra::core::ProjectType::Unknown) {
      fail("Module '" + moduleName + "' has unsupported type '" +
           typeValue.value() + "'.");
      return false;
    }

    ModuleConfig module;
    module.name = moduleName;

    const std::filesystem::path configuredPath(pathValue.value());
    module.path = configuredPath.is_absolute()
                      ? configuredPath.lexically_normal()
                      : (m_root / configuredPath).lexically_normal();
    module.type = moduleType.value();
    m_config.modules.push_back(std::move(module));
  }

  return true;
}

void ConfigLoader::fail(std::string message) {
  m_lastError = std::move(message);
  m_valid = false;
}

std::optional<std::string> ConfigLoader::extractStringField(
    const std::string& content,
    const std::string& key) {
  const std::string marker = '"' + key + '"';
  std::size_t keyPos = content.find(marker);
  if (keyPos == std::string::npos) {
    return std::nullopt;
  }

  std::size_t colonPos = content.find(':', keyPos + marker.size());
  if (colonPos == std::string::npos) {
    return std::nullopt;
  }

  std::size_t pos = colonPos + 1;
  skipWhitespace(content, pos);

  std::string value;
  if (!consumeQuotedString(content, pos, value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::string> ConfigLoader::extractObjectField(
    const std::string& content,
    const std::string& key) {
  const std::string marker = '"' + key + '"';
  std::size_t keyPos = content.find(marker);
  if (keyPos == std::string::npos) {
    return std::nullopt;
  }

  std::size_t colonPos = content.find(':', keyPos + marker.size());
  if (colonPos == std::string::npos) {
    return std::nullopt;
  }

  std::size_t pos = colonPos + 1;
  skipWhitespace(content, pos);
  if (pos >= content.size() || content[pos] != '{') {
    return std::nullopt;
  }

  std::size_t closePos = 0;
  if (!findMatchingBrace(content, pos, closePos)) {
    return std::nullopt;
  }

  return content.substr(pos + 1, closePos - pos - 1);
}

void ConfigLoader::skipWhitespace(const std::string& text, std::size_t& pos) {
  while (pos < text.size()) {
    const char c = text[pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++pos;
      continue;
    }
    break;
  }
}

bool ConfigLoader::consumeQuotedString(const std::string& text,
                                       std::size_t& pos,
                                       std::string& out) {
  if (pos >= text.size() || text[pos] != '"') {
    return false;
  }

  ++pos;
  out.clear();

  while (pos < text.size()) {
    const char c = text[pos];
    if (c == '\\') {
      if (pos + 1 >= text.size()) {
        return false;
      }
      out.push_back(text[pos + 1]);
      pos += 2;
      continue;
    }

    if (c == '"') {
      ++pos;
      return true;
    }

    out.push_back(c);
    ++pos;
  }

  return false;
}

bool ConfigLoader::findMatchingBrace(const std::string& text,
                                     std::size_t openPos,
                                     std::size_t& closePos) {
  if (openPos >= text.size() || text[openPos] != '{') {
    return false;
  }

  int depth = 0;
  bool inString = false;

  for (std::size_t i = openPos; i < text.size(); ++i) {
    const char c = text[i];

    if (inString) {
      if (c == '\\') {
        ++i;
        continue;
      }
      if (c == '"') {
        inString = false;
      }
      continue;
    }

    if (c == '"') {
      inString = true;
      continue;
    }

    if (c == '{') {
      ++depth;
      continue;
    }

    if (c == '}') {
      --depth;
      if (depth == 0) {
        closePos = i;
        return true;
      }
      if (depth < 0) {
        return false;
      }
    }
  }

  return false;
}

}  // namespace ultra::config
