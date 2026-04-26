#include "PatchManager.h"
#include "../core/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

namespace ultra::patch {

PatchManager::PatchManager() {}

void PatchManager::clearBackups() {
  for (const auto& [orig, bak] : backups_) {
    std::error_code ec;
    std::filesystem::remove(bak, ec);
  }
  backups_.clear();
}

// Trim + collapse internal whitespace runs for context-line comparison.
std::string PatchManager::normalizeWS(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool inSpace = true; // treat leading space as already-trimmed
  for (char c : s) {
    if (c == ' ' || c == '\t') {
      if (!inSpace && !out.empty()) { out += ' '; inSpace = true; }
    } else {
      out += c;
      inSpace = false;
    }
  }
  // trim trailing
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool PatchManager::linesMatch(const std::string& a, const std::string& b) {
  return a == b || normalizeWS(a) == normalizeWS(b);
}

void PatchManager::backupFile(const std::filesystem::path& file) {
  if (backups_.count(file) > 0) return;
  std::filesystem::path bakPath = file;
  bakPath += ".ultra.bak";
  std::error_code ec;
  std::filesystem::copy_file(file, bakPath, std::filesystem::copy_options::overwrite_existing, ec);
  if (!ec) {
    backups_[file] = bakPath;
  }
}

void PatchManager::restoreFile(const std::filesystem::path& file) {
  if (backups_.count(file) > 0) {
    std::error_code ec;
    std::filesystem::copy_file(backups_[file], file, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(backups_[file], ec);
    backups_.erase(file);
  }
}

bool PatchManager::rollback() {
  if (backups_.empty()) return false;
  for (const auto& [orig, bak] : backups_) {
    std::error_code ec;
    std::filesystem::copy_file(bak, orig, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(bak, ec);
  }
  backups_.clear();
  return true;
}

ApplyResult PatchManager::applyExactLineReplace(const std::filesystem::path& projectPath,
                                                const std::string& file,
                                                int startLine,
                                                int endLine,
                                                const std::string& replacement) {
  ApplyResult result;
  std::filesystem::path targetPath = projectPath / file;
  if (!std::filesystem::exists(targetPath)) {
    result.error = "File not found: " + targetPath.string();
    return result;
  }

  backupFile(targetPath);

  std::ifstream in(targetPath);
  if (!in) {
    result.error = "Could not read file: " + targetPath.string();
    return result;
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  in.close();

  if (startLine < 1 || startLine > lines.size() + 1 || endLine < startLine - 1 || endLine > lines.size()) {
    result.error = "Invalid line range";
    return result;
  }

  std::vector<std::string> newLines;
  for (int i = 0; i < startLine - 1; ++i) {
    newLines.push_back(lines[i]);
  }

  std::stringstream ss(replacement);
  std::string rLine;
  int linesAdded = 0;
  while (std::getline(ss, rLine)) {
    newLines.push_back(rLine);
    linesAdded++;
  }

  for (int i = endLine; i < lines.size(); ++i) {
    newLines.push_back(lines[i]);
  }

  std::ofstream out(targetPath);
  if (!out) {
    result.error = "Could not write file: " + targetPath.string();
    return result;
  }

  for (size_t i = 0; i < newLines.size(); ++i) {
    out << newLines[i];
    if (i + 1 < newLines.size()) out << '\n';
  }
  
  result.success = true;
  result.filesModified = 1;
  result.linesAdded = linesAdded;
  result.linesRemoved = endLine - startLine + 1;
  result.rollbackAvailable = true;
  return result;
}

ApplyResult PatchManager::applyUnifiedDiff(const std::filesystem::path& projectPath,
                                           const std::string& diffText) {
  ApplyResult result;
  std::stringstream ss(diffText);
  std::string line;

  std::string currentFile;
  std::vector<std::string> fileLines;
  std::vector<std::string> newFileLines;
  bool processingHunk = false;
  int srcLineIdx = 0;

  auto commitCurrentFile = [&]() -> bool {
    if (!currentFile.empty() && processingHunk) {
      std::filesystem::path targetPath = projectPath / currentFile;
      while (srcLineIdx < static_cast<int>(fileLines.size())) {
        newFileLines.push_back(fileLines[srcLineIdx++]);
      }
      backupFile(targetPath);
      std::ofstream out(targetPath);
      if (!out) {
        result.error = "Could not write file: " + targetPath.string();
        return false;
      }
      for (std::size_t i = 0; i < newFileLines.size(); ++i) {
        out << newFileLines[i];
        if (i + 1 < newFileLines.size()) out << '\n';
      }
      result.filesModified++;
    }
    return true;
  };

  auto applyHunk = [&](const std::vector<std::string>& hunkLines, int oldStart) -> bool {
    // Collect expected old-side lines (context + removed)
    std::vector<std::string> expectedOld;
    for (const auto& hl : hunkLines) {
      if (!hl.empty() && (hl[0] == ' ' || hl[0] == '-')) {
        expectedOld.push_back(hl.substr(1));
      }
    }

    // Try to find the best offset in ±100 lines
    const int targetIndex = std::max(0, oldStart - 1);
    const int maxOffset = 100;
    int bestOffset = INT32_MAX;
    bool found = false;

    for (int off = 0; off <= maxOffset && !found; ++off) {
      for (int sign : {1, -1}) {
        if (off == 0 && sign == -1) continue;
        const int testIndex = targetIndex + off * sign;
        if (testIndex < srcLineIdx ||
            testIndex + static_cast<int>(expectedOld.size()) >
                static_cast<int>(fileLines.size())) {
          continue;
        }
        bool match = true;
        for (std::size_t i = 0; i < expectedOld.size(); ++i) {
          if (!linesMatch(fileLines[static_cast<std::size_t>(testIndex) + i],
                          expectedOld[i])) {
            match = false;
            break;
          }
        }
        if (match) { bestOffset = testIndex; found = true; break; }
      }
    }

    if (!found) {
      result.error = "Context mismatch in " + currentFile +
                     " near line " + std::to_string(oldStart);
      return false;
    }

    // Emit lines before the hunk
    while (srcLineIdx < bestOffset) {
      newFileLines.push_back(fileLines[srcLineIdx++]);
    }

    // Apply hunk lines
    for (const auto& hl : hunkLines) {
      if (hl.empty()) continue;
      const char kind = hl[0];
      const std::string text = hl.substr(1);
      if (kind == ' ') {
        newFileLines.push_back(fileLines[srcLineIdx++]);
      } else if (kind == '-') {
        ++srcLineIdx;
        ++result.linesRemoved;
      } else if (kind == '+') {
        newFileLines.push_back(text);
        ++result.linesAdded;
      }
    }
    return true;
  };

  // Per-hunk state
  std::vector<std::string> currentHunkLines;
  int currentHunkOldStart = 0;

  auto flushHunk = [&]() -> bool {
    if (currentHunkLines.empty()) return true;
    bool ok = applyHunk(currentHunkLines, currentHunkOldStart);
    currentHunkLines.clear();
    currentHunkOldStart = 0;
    return ok;
  };

  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (line.rfind("--- ", 0) == 0) {
      // skip old-file header
    } else if (line.rfind("+++ ", 0) == 0) {
      if (!flushHunk()) return result;
      if (!commitCurrentFile()) return result;
      std::string pathPart = line.substr(4);
      const auto tab = pathPart.find('\t');
      if (tab != std::string::npos) pathPart.resize(tab);
      if (pathPart.size() >= 2 && pathPart[1] == '/' &&
          (pathPart[0] == 'a' || pathPart[0] == 'b')) {
        pathPart = pathPart.substr(2);
      }
      currentFile = pathPart;
      fileLines.clear();
      newFileLines.clear();
      std::ifstream in(projectPath / currentFile);
      if (in) {
        std::string fl;
        while (std::getline(in, fl)) {
          if (!fl.empty() && fl.back() == '\r') fl.pop_back();
          fileLines.push_back(fl);
        }
      }
      srcLineIdx = 0;
      processingHunk = false;

    } else if (line.rfind("@@ ", 0) == 0 || line.rfind("@@", 0) == 0) {
      if (!flushHunk()) return result;
      processingHunk = true;
      currentHunkOldStart = 0;
      const auto minus = line.find('-');
      if (minus != std::string::npos) {
        currentHunkOldStart = std::stoi(line.substr(minus + 1));
      }
      currentHunkOldStart = std::max(1, currentHunkOldStart);

    } else if (processingHunk) {
      currentHunkLines.push_back(line);
    }
  }

  if (!flushHunk()) return result;
  if (!commitCurrentFile()) return result;

  result.success = true;
  result.rollbackAvailable = true;
  return result;
}

}  // namespace ultra::patch
