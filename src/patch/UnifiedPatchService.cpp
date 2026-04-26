#include "UnifiedPatchService.h"
#include "../core/Logger.h"
#include "../build/BuildEngine.h"
#include "../platform/WindowsProcessExecutor.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <system_error>
#include <cstdio>

namespace ultra::patch {

// ─── Logging ──────────────────────────────────────────────────────────────────

void UnifiedPatchService::logLine(const std::string& msg) {
    log_ += msg + '\n';
    ultra::core::Logger::info(ultra::core::LogCategory::Patch, msg);
}

// ─── Normalization ────────────────────────────────────────────────────────────

std::string UnifiedPatchService::stripBom(std::string text) {
    // UTF-8 BOM: EF BB BF
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::string UnifiedPatchService::normalizeCRLF(std::string text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            result += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i; // skip the \n following \r
            }
        } else {
            result += text[i];
        }
    }
    return result;
}

// ─── Header injection for bare hunk-only diffs ───────────────────────────────

std::string UnifiedPatchService::injectHintHeader(const std::string& diffText,
                                                    const std::string& hintFile) {
    if (hintFile.empty()) return diffText;
    // If diff already has --- / +++ headers, don't touch it
    if (diffText.find("\n--- ") != std::string::npos ||
        diffText.rfind("--- ", 0) == 0) {
        return diffText;
    }
    // Inject minimal headers before the first @@
    const std::string header = "--- a/" + hintFile + "\n+++ b/" + hintFile + "\n";
    const auto atAt = diffText.find("@@");
    if (atAt == std::string::npos) return diffText;
    return header + diffText.substr(atAt);
}

// ─── Git repo detection ──────────────────────────────────────────────────────

bool UnifiedPatchService::isGitRepo(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::path candidate = dir;
    for (int depth = 0; depth < 8; ++depth) {
        const auto gitDir = candidate / ".git";
        if (std::filesystem::exists(gitDir, ec) && !ec) return true;
        const auto parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return false;
}

// ─── Temp file helper ────────────────────────────────────────────────────────

std::filesystem::path UnifiedPatchService::writeTempDiff(const std::string& content) {
    std::error_code ec;
    const auto tmpDir = std::filesystem::temp_directory_path(ec);
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path tmpPath =
        ec ? std::filesystem::path("ultra_unified.diff")
           : tmpDir / ("ultra_unified_" + std::to_string(suffix) + ".diff");
    // Write UTF-8 no-BOM, LF endings (content already normalized)
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (out) out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return tmpPath;
}

// ─── Shell helpers ────────────────────────────────────────────────────────────

std::string UnifiedPatchService::quoteForShell(const std::string& raw) {
    std::string q;
    q.reserve(raw.size() + 2);
    q.push_back('"');
    for (char ch : raw) {
        if (ch == '"') q += "\\\"";
        else q.push_back(ch);
    }
    q.push_back('"');
    return q;
}

std::string UnifiedPatchService::runShellCommand(const std::string& cmd) {
    ultra::platform::WindowsProcessExecutor exec;
    const auto result = exec.execute(cmd);
    std::string combined = result.stdOut;
    if (!result.stdErr.empty()) {
        if (!combined.empty() && combined.back() != '\n') combined += '\n';
        combined += result.stdErr;
    }
    return combined;
}

int UnifiedPatchService::runShellCommandExitCode(const std::string& cmd) {
    ultra::platform::WindowsProcessExecutor exec;
    return exec.execute(cmd).exitCode;
}

// ─── Already-applied detection ───────────────────────────────────────────────

bool UnifiedPatchService::isAlreadyApplied(const UnifiedPatchRequest& req,
                                            const std::string& normalized) const {
    // For each file in the diff, check if every "+" line already appears at
    // the expected location in the actual file.
    std::istringstream ss(normalized);
    std::string line;
    std::filesystem::path currentTarget;
    std::vector<std::string> addedLines;
    bool inHunk = false;
    int fileCount = 0;
    int totalAddedLines = 0;

    auto checkFile = [&]() -> bool {
        if (currentTarget.empty()) return true;
        if (addedLines.empty()) return true; // nothing to check in this file
        // Read target file
        std::error_code ec;
        const auto absPath = req.projectPath / currentTarget;
        if (!std::filesystem::exists(absPath, ec) || ec) return false;
        std::ifstream fin(absPath);
        if (!fin.is_open()) return false;
        std::string fl;
        std::vector<std::string> actualLines;
        while (std::getline(fin, fl)) {
            if (!fl.empty() && fl.back() == '\r') fl.pop_back();
            actualLines.push_back(fl);
        }
        for (const auto& addedLine : addedLines) {
            bool found = false;
            for (const auto& actual : actualLines) {
                if (actual == addedLine) { found = true; break; }
            }
            if (!found) return false;
        }
        return true;
    };

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("+++ ", 0) == 0) {
            if (!checkFile()) return false;
            std::string p = line.substr(4);
            const auto tab = p.find('\t');
            if (tab != std::string::npos) p.resize(tab);
            if (p.size() >= 2 && p[1] == '/' && (p[0] == 'a' || p[0] == 'b'))
                p = p.substr(2);
            currentTarget = std::filesystem::path(p).lexically_normal();
            addedLines.clear();
            inHunk = false;
            ++fileCount;
        } else if (line.rfind("@@", 0) == 0) {
            inHunk = true;
        } else if (inHunk && !line.empty() && line[0] == '+' &&
                   line.rfind("+++", 0) != 0) {
            addedLines.push_back(line.substr(1));
            ++totalAddedLines;
        }
    }

    // Vacuous cases: no files found in diff OR no added lines at all
    // → cannot claim "already applied" — let strategies handle it normally
    if (fileCount == 0 || totalAddedLines == 0) return false;

    return checkFile();
}

// ─── Changed files detection ──────────────────────────────────────────────────

std::vector<std::string> UnifiedPatchService::detectChangedFiles(
    const std::filesystem::path& root,
    const std::vector<std::filesystem::path>& targets) const {
    // Caller should snapshot before/after; we just return the target list here.
    // UnifiedPatchService trusts PatchManager's filesModified count.
    (void)root;
    std::vector<std::string> result;
    for (const auto& t : targets) result.push_back(t.generic_string());
    return result;
}

// ─── Strategy 1: PatchManager (improved with fuzzy/WS in PM itself) ──────────

std::optional<UnifiedPatchResult> UnifiedPatchService::tryPatchManager(
    const UnifiedPatchRequest& req, const std::string& normalized) {

    logLine("Strategy 1 [PatchManager]: attempting...");
    const ApplyResult r = patchManager_.applyUnifiedDiff(req.projectPath, normalized);
    if (!r.success) {
        logLine("Strategy 1 [PatchManager]: FAILED — " + r.error);
        patchManager_.rollback();
        return std::nullopt;
    }
    if (r.filesModified == 0) {
        // Diff parsed without error but touched no files — treat as invalid diff
        logLine("Strategy 1 [PatchManager]: FAILED — diff produced no file modifications (malformed or empty diff)");
        patchManager_.rollback();
        return std::nullopt;
    }

    // Clean up .ultra.bak files on success
    patchManager_.clearBackups();

    UnifiedPatchResult result;
    result.ok = true;
    result.strategyUsed = PatchStrategy::PatchManager;
    result.rollbackAvailable = false; // backups cleared
    logLine("Strategy 1 [PatchManager]: SUCCESS — files=" +
            std::to_string(r.filesModified) +
            " added=" + std::to_string(r.linesAdded) +
            " removed=" + std::to_string(r.linesRemoved));
    return result;
}

// ─── Strategy 2: git apply ─────────────────────────────────────────────────────

std::optional<UnifiedPatchResult> UnifiedPatchService::tryGitApply(
    const UnifiedPatchRequest& req, const std::string& normalized) {

    if (!isGitRepo(req.projectPath)) {
        logLine("Strategy 2 [git apply]: skipped — not a git repository");
        return std::nullopt;
    }

    logLine("Strategy 2 [git apply]: attempting...");
    const std::filesystem::path tmpDiff = writeTempDiff(normalized);

    // Use --ignore-space-change for whitespace drift tolerance (improvement over CppAdapter)
    const std::string applyCmd =
        "cd /d " + quoteForShell(req.projectPath.string()) +
        " && git apply --verbose --whitespace=fix --ignore-space-change " +
        quoteForShell(tmpDiff.string());

    // Capture stdout+stderr to temp files
    std::error_code ec;
    const auto tmpDir = std::filesystem::temp_directory_path(ec);
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto outFile = ec ? std::filesystem::path("ups_stdout.log")
                            : tmpDir / ("ups_stdout_" + std::to_string(suffix) + ".log");
    const auto errFile = ec ? std::filesystem::path("ups_stderr.log")
                            : tmpDir / ("ups_stderr_" + std::to_string(suffix) + ".log");
    const std::string redirected = applyCmd + " > " + quoteForShell(outFile.string()) +
                                   " 2> " + quoteForShell(errFile.string());

    const int exitCode = runShellCommandExitCode(redirected);

    std::string gitOut;
    {
        std::ifstream fout(outFile); if (fout) gitOut += std::string(std::istreambuf_iterator<char>(fout), {});
        std::ifstream ferr(errFile); if (ferr) gitOut += std::string(std::istreambuf_iterator<char>(ferr), {});
    }
    std::error_code rmEc;
    std::filesystem::remove(outFile, rmEc);
    std::filesystem::remove(errFile, rmEc);
    std::filesystem::remove(tmpDiff, rmEc);

    if (exitCode != 0) {
        logLine("Strategy 2 [git apply]: FAILED (exit=" + std::to_string(exitCode) + ") — " + gitOut);
        return std::nullopt;
    }

    UnifiedPatchResult result;
    result.ok = true;
    result.strategyUsed = PatchStrategy::GitApply;
    result.rollbackAvailable = true; // git apply -R possible
    logLine("Strategy 2 [git apply]: SUCCESS");
    return result;
}

// ─── Strategy 3: exact line replace ──────────────────────────────────────────

std::optional<UnifiedPatchResult> UnifiedPatchService::tryExactLineReplace(
    const UnifiedPatchRequest& req, const std::string& normalized) {

    // Only attempt for single-hunk single-file diffs
    const std::size_t hunkCount = [&]{
        std::size_t n = 0;
        std::size_t pos = 0;
        while ((pos = normalized.find("\n@@", pos)) != std::string::npos) { ++n; ++pos; }
        if (normalized.rfind("@@", 0) == 0) ++n;
        return n;
    }();

    if (hunkCount != 1) {
        logLine("Strategy 3 [ExactLineReplace]: skipped — " + std::to_string(hunkCount) + " hunks");
        return std::nullopt;
    }

    logLine("Strategy 3 [ExactLineReplace]: attempting...");

    // Parse single hunk
    std::string targetFile;
    int startLine = 0;
    int endLine = 0;
    std::string replacement;
    std::istringstream ss(normalized);
    std::string line;
    bool inHunk = false;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("+++ ", 0) == 0) {
            std::string p = line.substr(4);
            const auto tab = p.find('\t');
            if (tab != std::string::npos) p.resize(tab);
            if (p.size() >= 2 && p[1] == '/' && (p[0] == 'a' || p[0] == 'b'))
                p = p.substr(2);
            targetFile = p;
        } else if (line.rfind("@@ ", 0) == 0) {
            const auto minus = line.find('-');
            if (minus == std::string::npos) continue;
            startLine = std::stoi(line.substr(minus + 1));
            const auto comma = line.find(',', minus);
            const int count = (comma != std::string::npos) ? std::stoi(line.substr(comma + 1)) : 1;
            endLine = startLine + count - 1;
            inHunk = true;
        } else if (inHunk && !line.empty()) {
            if (line[0] == '+' && line.rfind("+++", 0) != 0) {
                if (!replacement.empty()) replacement += '\n';
                replacement += line.substr(1);
            }
        }
    }

    if (targetFile.empty() || startLine <= 0) {
        logLine("Strategy 3 [ExactLineReplace]: FAILED — could not parse hunk");
        return std::nullopt;
    }

    const ApplyResult r = patchManager_.applyExactLineReplace(
        req.projectPath, targetFile, startLine, endLine, replacement);

    if (!r.success) {
        logLine("Strategy 3 [ExactLineReplace]: FAILED — " + r.error);
        patchManager_.rollback();
        return std::nullopt;
    }

    patchManager_.clearBackups();
    UnifiedPatchResult result;
    result.ok = true;
    result.strategyUsed = PatchStrategy::ExactLineReplace;
    result.rollbackAvailable = false;
    result.filesChanged = { targetFile };
    logLine("Strategy 3 [ExactLineReplace]: SUCCESS");
    return result;
}

// ─── Main apply() ─────────────────────────────────────────────────────────────

UnifiedPatchResult UnifiedPatchService::apply(const UnifiedPatchRequest& request) {
    log_.clear();
    logLine("UnifiedPatchService::apply() source=" + patchSourceName(request.source));

    UnifiedPatchResult failure;

    // 1. Validate project path
    std::error_code ec;
    if (!std::filesystem::is_directory(request.projectPath, ec) || ec) {
        failure.error = "project_path is not a valid directory: " + request.projectPath.string();
        logLine("ERROR: " + failure.error);
        failure.log = log_;
        return failure;
    }

    // 2. Normalize: strip BOM, unify line endings, inject hint header
    std::string normalized = stripBom(request.diffText);
    normalized = normalizeCRLF(std::move(normalized));
    if (!request.hintFile.empty()) {
        normalized = injectHintHeader(normalized, request.hintFile);
    }

    if (normalized.empty()) {
        failure.error = "diff text is empty after normalization";
        logLine("ERROR: " + failure.error);
        failure.log = log_;
        return failure;
    }

    // 3. Already-applied detection
    if (isAlreadyApplied(request, normalized)) {
        logLine("Already-applied detection: diff already present in files — clean success");
        UnifiedPatchResult already;
        already.ok = true;
        already.alreadyApplied = true;
        already.strategyUsed = PatchStrategy::PatchManager;
        already.log = log_;
        return already;
    }

    // 4. Strategy cascade
    if (auto r = tryPatchManager(request, normalized)) {
        auto result = std::move(*r);
        result.log = log_;
        return result;
    }
    if (auto r = tryGitApply(request, normalized)) {
        auto result = std::move(*r);
        result.log = log_;
        return result;
    }
    if (auto r = tryExactLineReplace(request, normalized)) {
        auto result = std::move(*r);
        result.log = log_;
        return result;
    }

    failure.error = "All patch strategies failed. Check log for details.";
    logLine("FINAL: all strategies exhausted — " + failure.error);
    failure.log = log_;
    return failure;
}

// ─── Rollback ─────────────────────────────────────────────────────────────────

bool UnifiedPatchService::rollback() {
    return patchManager_.rollback();
}

// ─── JSON serialization ───────────────────────────────────────────────────────

std::string patchSourceName(PatchSource source) {
    switch (source) {
        case PatchSource::CLI:        return "cli";
        case PatchSource::ToolRouter: return "tool_router";
        case PatchSource::Runtime:    return "runtime";
    }
    return "unknown";
}

std::string patchStrategyName(PatchStrategy strategy) {
    switch (strategy) {
        case PatchStrategy::PatchManager:    return "patch_manager";
        case PatchStrategy::GitApply:        return "git_apply_fallback";
        case PatchStrategy::ExactLineReplace: return "exact_line_replace";
    }
    return "unknown";
}

std::string patchResultToJson(const UnifiedPatchResult& result) {
    std::string files = "[";
    for (std::size_t i = 0; i < result.filesChanged.size(); ++i) {
        if (i) files += ',';
        files += '"' + result.filesChanged[i] + '"';
    }
    files += ']';

    auto jsonStr = [](const std::string& s) -> std::string {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"')       out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else                out += c;
        }
        out += '"';
        return out;
    };

    std::string json = "{\n";
    json += "  \"ok\": " + std::string(result.ok ? "true" : "false") + ",\n";
    json += "  \"strategy_used\": " + jsonStr(patchStrategyName(result.strategyUsed)) + ",\n";
    json += "  \"files_changed\": " + files + ",\n";
    json += "  \"validated\": " + std::string(result.validated ? "true" : "false") + ",\n";
    json += "  \"rollback_available\": " + std::string(result.rollbackAvailable ? "true" : "false") + ",\n";
    json += "  \"already_applied\": " + std::string(result.alreadyApplied ? "true" : "false") + ",\n";
    json += "  \"build_exit_code\": " + std::to_string(result.buildExitCode) + ",\n";
    json += "  \"error\": " + jsonStr(result.error) + ",\n";
    json += "  \"log\": " + jsonStr(result.log) + "\n";
    json += "}";
    return json;
}

} // namespace ultra::patch
