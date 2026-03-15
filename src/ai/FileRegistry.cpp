#include "FileRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>

namespace ultra::ai {

namespace {

constexpr std::array<const char*, 8> kCppExtensions{
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"};

constexpr std::array<const char*, 4> kJavaScriptExtensions{
    ".js", ".mjs", ".cjs", ".jsx"};

constexpr std::array<const char*, 2> kTypeScriptExtensions{
    ".ts", ".tsx"};

constexpr std::array<const char*, 2> kPythonExtensions{
    ".py", ".pyi"};

constexpr std::array<const char*, 1> kJavaExtensions{
    ".java"};

constexpr std::array<const char*, 1> kGoExtensions{
    ".go"};

constexpr std::array<const char*, 1> kRustExtensions{
    ".rs"};

constexpr std::array<const char*, 2> kCSharpExtensions{
    ".cs", ".csx"};

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool startsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

bool isInsideAiDirectory(const std::string& relativePath) {
  return relativePath == ".ultra/ai" || startsWith(relativePath, ".ultra/ai/");
}

std::string fromU8Path(const std::filesystem::path& path) {
  const auto u8 = path.generic_u8string();
  std::string out;
  out.reserve(u8.size());
  for (const auto ch : u8) {
    out.push_back(static_cast<char>(ch));
  }
  return out;
}

}  // namespace

std::vector<DiscoveredFile> FileRegistry::discoverProjectFiles(
    const std::filesystem::path& projectRoot) {

  std::vector<DiscoveredFile> discovered;

  const std::filesystem::path absoluteRoot =
      std::filesystem::absolute(projectRoot).lexically_normal();

  for (auto iterator = std::filesystem::recursive_directory_iterator(
           absoluteRoot,
           std::filesystem::directory_options::skip_permission_denied);
       iterator != std::filesystem::recursive_directory_iterator(); ++iterator) {

    const std::filesystem::path entryPath = iterator->path();
    const std::string relativePath = toRelativeUtf8Path(absoluteRoot, entryPath);

    if (iterator->is_directory()) {
      if (isInsideAiDirectory(relativePath)) {
        iterator.disable_recursion_pending();
      }
      continue;
    }

    if (!iterator->is_regular_file()) {
      continue;
    }

    if (isInsideAiDirectory(relativePath)) {
      continue;
    }

    const Language language = detectLanguage(entryPath);
    if (language == Language::Unknown) {
      continue;
    }

    DiscoveredFile file;
    file.absolutePath = entryPath;
    file.relativePath = relativePath;
    file.language = language;

    try {
      file.lastModified =
          fileTimeToUint64(std::filesystem::last_write_time(entryPath));
    } catch (...) {
      file.lastModified = 0;
    }

    discovered.push_back(std::move(file));
  }

  std::sort(discovered.begin(), discovered.end(),
            [](const DiscoveredFile& a, const DiscoveredFile& b) {
              return a.relativePath < b.relativePath;
            });

  for (std::size_t i = 0; i < discovered.size(); ++i) {
    discovered[i].fileId = static_cast<std::uint32_t>(i + 1);
  }

  return discovered;
}

std::vector<FileRecord> FileRegistry::deriveRecords(
    const std::vector<DiscoveredFile>& discoveredFiles) {

  std::vector<FileRecord> records;
  records.reserve(discoveredFiles.size());

  for (const auto& file : discoveredFiles) {
    FileRecord record;
    record.fileId = file.fileId;
    record.path = file.relativePath;
    record.language = file.language;
    record.lastModified = file.lastModified;
    record.hash = zeroHash();
    records.push_back(std::move(record));
  }

  return records;
}

std::map<std::string, FileRecord> FileRegistry::mapByPath(
    const std::vector<FileRecord>& records) {

  std::map<std::string, FileRecord> result;

  for (const auto& record : records) {
    result[record.path] = record;
  }

  return result;
}

std::map<std::uint32_t, std::string> FileRegistry::mapPathById(
    const std::vector<FileRecord>& records) {

  std::map<std::uint32_t, std::string> result;

  for (const auto& record : records) {
    result[record.fileId] = record.path;
  }

  return result;
}

Language FileRegistry::detectLanguage(const std::filesystem::path& path) {
    std::string ext = path.extension().string();

    if (ext == ".cpp" || ext == ".cc" || ext == ".hpp" || ext == ".h")
        return Language::Cpp;

    if (ext == ".js" || ext == ".cjs" || ext == ".mjs")
        return Language::JavaScript;

    if (ext == ".ts" || ext == ".tsx")
        return Language::TypeScript;

    if (ext == ".py")
        return Language::Python;

    if (ext == ".java")
        return Language::Java;

    if (ext == ".go")
        return Language::Go;

    if (ext == ".rs")
        return Language::Rust;

    if (ext == ".cs")
        return Language::CSharp;

    return Language::Unknown;
}

std::string FileRegistry::languageToString(const Language language) {

  switch (language) {

    case Language::Cpp: return "cpp";
    case Language::JavaScript: return "javascript";
    case Language::TypeScript: return "typescript";
    case Language::Python: return "python";
    case Language::Java: return "java";
    case Language::Go: return "go";
    case Language::Rust: return "rust";
    case Language::CSharp: return "csharp";

    default: return "unknown";
  }
}

std::string FileRegistry::toRelativeUtf8Path(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute) {

  const auto relative = absolute.lexically_relative(root);
  return fromU8Path(relative.lexically_normal());
}

std::uint64_t FileRegistry::fileTimeToUint64(
    const std::filesystem::file_time_type value) {

  const auto ticks = value.time_since_epoch().count();
  return static_cast<std::uint64_t>(ticks);
}

}  // namespace ultra::ai