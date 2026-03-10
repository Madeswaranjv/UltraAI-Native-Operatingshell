#include "ConfigManager.h"
#include <fstream>
#include <sstream>

namespace ultra::core {

namespace {

const std::vector<std::string> kDefaultIgnoreDirs{"build", ".git", ".vs"};
const std::vector<std::string> kDefaultSupportedExtensions{".cpp", ".hpp", ".h"};
const std::vector<std::string> kDefaultVendorPatterns{
    "stb_", "glad", "nuklear", "minimp3"};

}  // namespace

ConfigManager::ConfigManager(const std::filesystem::path& configPath)
    : m_configPath(configPath), m_ignoreDirs(kDefaultIgnoreDirs),
      m_supportedExtensions(kDefaultSupportedExtensions),
      m_vendorPatterns(kDefaultVendorPatterns) {
  load();
}

void ConfigManager::load() {
  if (!std::filesystem::exists(m_configPath) ||
      !std::filesystem::is_regular_file(m_configPath)) {
    m_loaded = false;
    return;
  }
  std::ifstream f(m_configPath);
  if (!f) {
    m_loaded = false;
    return;
  }
  std::stringstream buf;
  buf << f.rdbuf();
  std::string content = buf.str();
  parseIgnoreDirs(content);
  parseSupportedExtensions(content);
  parseVendorPatterns(content);
  parseMaxFileSizeKb(content);
  m_loaded = true;
}

std::string ConfigManager::extractArrayValues(const std::string& content,
                                              const std::string& key) {
  std::string searchKey = "\"" + key + "\"";
  auto pos = content.find(searchKey);
  if (pos == std::string::npos) return {};
  pos = content.find('[', pos);
  if (pos == std::string::npos) return {};
  auto end = content.find(']', pos);
  if (end == std::string::npos) return {};
  return content.substr(pos, end - pos + 1);
}

std::string ConfigManager::extractStringValue(const std::string& content,
                                              const std::string& key) {
  std::string searchKey = "\"" + key + "\"";
  auto pos = content.find(searchKey);
  if (pos == std::string::npos) return {};
  pos = content.find(':', pos);
  if (pos == std::string::npos) return {};
  pos = content.find_first_not_of(" \t", pos + 1);
  if (pos == std::string::npos) return {};
  if (content[pos] == '"') {
    auto end = content.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return content.substr(pos + 1, end - pos - 1);
  }
  auto end = content.find_first_of(",}", pos);
  if (end == std::string::npos) return {};
  std::string num = content.substr(pos, end - pos);
  return num;
}

void ConfigManager::parseIgnoreDirs(const std::string& content) {
  std::string arr = extractArrayValues(content, "ignore_dirs");
  if (arr.empty()) return;
  m_ignoreDirs.clear();
  for (size_t i = 0; i < arr.size();) {
    auto q = arr.find('"', i);
    if (q == std::string::npos) break;
    auto r = arr.find('"', q + 1);
    if (r == std::string::npos) break;
    m_ignoreDirs.push_back(arr.substr(q + 1, r - q - 1));
    i = r + 1;
  }
}

void ConfigManager::parseSupportedExtensions(const std::string& content) {
  std::string arr = extractArrayValues(content, "supported_extensions");
  if (arr.empty()) return;
  m_supportedExtensions.clear();
  for (size_t i = 0; i < arr.size();) {
    auto q = arr.find('"', i);
    if (q == std::string::npos) break;
    auto r = arr.find('"', q + 1);
    if (r == std::string::npos) break;
    m_supportedExtensions.push_back(arr.substr(q + 1, r - q - 1));
    i = r + 1;
  }
}

void ConfigManager::parseVendorPatterns(const std::string& content) {
  std::string arr = extractArrayValues(content, "vendor_patterns");
  if (arr.empty()) return;
  m_vendorPatterns.clear();
  for (size_t i = 0; i < arr.size();) {
    auto q = arr.find('"', i);
    if (q == std::string::npos) break;
    auto r = arr.find('"', q + 1);
    if (r == std::string::npos) break;
    m_vendorPatterns.push_back(arr.substr(q + 1, r - q - 1));
    i = r + 1;
  }
}

void ConfigManager::parseMaxFileSizeKb(const std::string& content) {
  std::string val = extractStringValue(content, "max_file_size_kb");
  if (val.empty()) return;
  try {
    unsigned long n = std::stoul(val);
    m_maxFileSizeKb = static_cast<std::size_t>(n);
  } catch (...) {
  }
}

const std::vector<std::string>& ConfigManager::ignoreDirs() const noexcept {
  return m_ignoreDirs;
}

const std::vector<std::string>&
ConfigManager::supportedExtensions() const noexcept {
  return m_supportedExtensions;
}

const std::vector<std::string>& ConfigManager::vendorPatterns() const noexcept {
  return m_vendorPatterns;
}

std::size_t ConfigManager::maxFileSizeKb() const noexcept {
  return m_maxFileSizeKb;
}

bool ConfigManager::loaded() const noexcept {
  return m_loaded;
}

}  // namespace ultra::core
