#pragma once

#include "PatchManager.h"
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::patch {

// ─────────────────────────────────────────────────────────────────────────────
// Describes which part of ULTRA triggered the patch request.
// Used only for logging; does not change strategy order.
// ─────────────────────────────────────────────────────────────────────────────
enum class PatchSource {
  CLI,        // ultra apply_patch <dir> <diff>
  ToolRouter, // AI runtime via ToolRouter::execute_apply_patch
  Runtime,    // autonomous fix loop
};

// ─────────────────────────────────────────────────────────────────────────────
// Which concrete patcher successfully applied the diff.
// ─────────────────────────────────────────────────────────────────────────────
enum class PatchStrategy {
  PatchManager,    // Strategy 1 – custom C++ patcher (no git required)
  GitApply,        // Strategy 2 – git apply fallback
  ExactLineReplace // Strategy 3 – deterministic line replace
};

// ─────────────────────────────────────────────────────────────────────────────
// Request to apply a unified diff.
// ─────────────────────────────────────────────────────────────────────────────
struct UnifiedPatchRequest {
  std::filesystem::path projectPath; // absolute path to project root
  std::string diffText;              // raw diff (BOM/CRLF normalization applied internally)
  PatchSource source{PatchSource::CLI};
  bool noBuild{false};    // skip post-patch build validation
  std::string hintFile;   // relative path hint for bare hunk-only diffs (no --- / +++ headers)
};

// ─────────────────────────────────────────────────────────────────────────────
// Result from UnifiedPatchService::apply().
// Always populated — callers should check ok first.
// ─────────────────────────────────────────────────────────────────────────────
struct UnifiedPatchResult {
  bool ok{false};
  PatchStrategy strategyUsed{PatchStrategy::PatchManager};
  std::vector<std::string> filesChanged;
  bool validated{false};      // build/test validation ran and passed
  bool rollbackAvailable{false};
  bool alreadyApplied{false}; // diff was already present in files — clean success
  int buildExitCode{0};
  std::string error;
  std::string log; // step-by-step human-readable audit trail
};

// ─────────────────────────────────────────────────────────────────────────────
// UnifiedPatchService
//
// Single orchestrator for all C++ patch requests.  Routes through three
// strategies in order, picking the first that succeeds:
//
//   1. PatchManager::applyUnifiedDiff  (no git dependency, in-memory,
//      improved with fuzzy offset + whitespace normalization)
//   2. git apply --whitespace=fix --ignore-space-change
//      (only when a git repository is detected)
//   3. PatchManager::applyExactLineReplace
//      (single-hunk diffs only, deterministic)
// ─────────────────────────────────────────────────────────────────────────────
class UnifiedPatchService {
 public:
  UnifiedPatchService() = default;

  // Apply the diff described by req.  Returns a fully-populated result
  // regardless of success or failure.  Thread-compatible (create one instance
  // per call site — PatchManager holds mutable backup state).
  UnifiedPatchResult apply(const UnifiedPatchRequest& request);

  // Roll back the most recently applied patch (uses PatchManager's .ultra.bak
  // mechanism).  Returns true if at least one file was reverted.
  bool rollback();

  // ── Static helpers exposed for testing ────────────────────────────────────
  static std::string stripBom(std::string text);
  static std::string normalizeCRLF(std::string text);
  static std::string injectHintHeader(const std::string& diffText,
                                      const std::string& hintFile);
  static bool isGitRepo(const std::filesystem::path& dir);

 private:
  // Strategy implementations — each returns an empty optional on failure.
  std::optional<UnifiedPatchResult> tryPatchManager(
      const UnifiedPatchRequest& req, const std::string& normalized);
  std::optional<UnifiedPatchResult> tryGitApply(
      const UnifiedPatchRequest& req, const std::string& normalized);
  std::optional<UnifiedPatchResult> tryExactLineReplace(
      const UnifiedPatchRequest& req, const std::string& normalized);

  // Checks whether all added lines already exist in the target files.
  // Returns true → patch is already applied, emit clean success.
  bool isAlreadyApplied(const UnifiedPatchRequest& req,
                        const std::string& normalized) const;

  // Collect relative paths of files that changed between two calls.
  std::vector<std::string> detectChangedFiles(
      const std::filesystem::path& root,
      const std::vector<std::filesystem::path>& targets) const;

  // Helpers for git apply strategy
  static std::string quoteForShell(const std::string& raw);
  static std::string runShellCommand(const std::string& cmd);
  static int runShellCommandExitCode(const std::string& cmd);

  // Write the normalized diff to a temp file; return its path.
  static std::filesystem::path writeTempDiff(const std::string& content);

  PatchManager patchManager_;
  std::string log_;

  void logLine(const std::string& msg);
};

// ─────────────────────────────────────────────────────────────────────────────
// Utility: serialise a UnifiedPatchResult to a JSON string.
// Used by CLIEngine and ToolRouter to produce output.
// ─────────────────────────────────────────────────────────────────────────────
std::string patchResultToJson(const UnifiedPatchResult& result);
std::string patchSourceName(PatchSource source);
std::string patchStrategyName(PatchStrategy strategy);

} // namespace ultra::patch
