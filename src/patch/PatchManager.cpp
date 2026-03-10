#include "PatchManager.h"
#include "DiffParser.h"
#include "../build/BuildEngine.h"
#include "../core/Logger.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace ultra::patch {

namespace {

const char* kBakSuffix = ".ultra.bak";

}

PatchManager::PatchManager(ultra::build::BuildEngine& buildEngine)
    : buildEngine_(buildEngine) {}

void PatchManager::backupFile(const std::filesystem::path& file) {
  std::filesystem::path bakPath = file;
  bakPath += kBakSuffix;
  std::filesystem::copy_file(file, bakPath,
                            std::filesystem::copy_options::overwrite_existing);
}

void PatchManager::restoreFile(const std::filesystem::path& file) {
  std::filesystem::path bakPath = file;
  bakPath += kBakSuffix;
  if (std::filesystem::exists(bakPath)) {
    std::filesystem::copy_file(bakPath, file,
                              std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(bakPath);
  }
}

bool PatchManager::applyOperation(const std::filesystem::path& projectPath,
                                  const PatchOperation& op) {
  std::filesystem::path targetPath = projectPath / op.targetFile;
  if (!std::filesystem::exists(targetPath) ||
      !std::filesystem::is_regular_file(targetPath)) {
    ultra::core::Logger::error(ultra::core::LogCategory::Patch,
                               "Target file not found: " + targetPath.string());
    return false;
  }
  std::ifstream in(targetPath);
  if (!in) {
    ultra::core::Logger::error(ultra::core::LogCategory::Patch,
                               "Cannot read file: " + targetPath.string());
    return false;
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  in.close();
  for (const std::string& toRemove : op.removedLines) {
    auto it = std::ranges::find(lines, toRemove);
    if (it != lines.end()) {
      lines.erase(it);
    }
  }
  for (const std::string& toAdd : op.addedLines) {
    lines.push_back(toAdd);
  }
  std::ofstream out(targetPath);
  if (!out) {
    ultra::core::Logger::error(ultra::core::LogCategory::Patch,
                               "Cannot write file: " + targetPath.string());
    return false;
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    out << lines[i];
    if (i + 1 < lines.size()) out << '\n';
  }
  return true;
}

ApplyResult PatchManager::applyPatch(const std::filesystem::path& projectPath,
                                     const std::filesystem::path& diffFile) {
  ApplyResult result;
  std::vector<PatchOperation> ops = DiffParser::parse(diffFile);
  if (ops.empty()) {
    ultra::core::Logger::error(ultra::core::LogCategory::Patch,
                               "No valid patch operations in diff file.");
    return result;
  }
  std::vector<std::filesystem::path> modifiedFiles;
  for (const PatchOperation& op : ops) {
    std::filesystem::path targetPath = projectPath / op.targetFile;
    if (!std::filesystem::exists(targetPath) ||
        !std::filesystem::is_regular_file(targetPath)) {
      ultra::core::Logger::error(ultra::core::LogCategory::Patch,
                               "Target file not found: " + targetPath.string());
      for (const auto& f : modifiedFiles) restoreFile(f);
      return result;
    }
    try {
      backupFile(targetPath);
    } catch (const std::filesystem::filesystem_error& e) {
      ultra::core::Logger::error(ultra::core::LogCategory::Patch,
                                 std::string("Backup failed: ") + e.what());
      return result;
    }
    if (!applyOperation(projectPath, op)) {
      for (const auto& f : modifiedFiles) restoreFile(f);
      restoreFile(targetPath);
      return result;
    }
    modifiedFiles.push_back(targetPath);
  }
  result.filesModified = modifiedFiles.size();
  int buildCode = buildEngine_.fullBuild(projectPath);
  if (buildCode != 0) {
    ultra::core::Logger::info(ultra::core::LogCategory::Patch,
                              "Rolling back changes...");
    for (const auto& f : modifiedFiles) {
      restoreFile(f);
    }
    return result;
  }
  result.success = true;
  for (const auto& f : modifiedFiles) {
    std::filesystem::path bakPath = f;
    bakPath += kBakSuffix;
    if (std::filesystem::exists(bakPath)) {
      std::filesystem::remove(bakPath);
    }
  }
  return result;
}

}  // namespace ultra::patch
