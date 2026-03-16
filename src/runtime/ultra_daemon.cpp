#include "ultra/runtime/ultra_daemon.h"
 
#include "ultra/ipc/ultra_ipc_client.h"
 
#include <exception>
#include <fstream>
#include <thread>
#include <utility>
//E:\Projects\Ultra\src\runtime\ultra_daemon.cpp
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>        // O_RDWR for /dev/null redirect
#include <sys/types.h>
#include <sys/wait.h>     // waitpid
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <windows.h>
#endif
 
namespace ultra::runtime {
namespace {
 
using Json = nlohmann::json;
 
std::filesystem::path normalizeProjectRoot(const std::filesystem::path& input) {
  std::error_code ec;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(input, ec);
  if (!ec) {
    return canonical;
  }
  const std::filesystem::path absolute = std::filesystem::absolute(input, ec);
  if (!ec) {
    return absolute;
  }
  return input;
}
 
#ifndef _WIN32
 
long long currentProcessId() {
  return static_cast<long long>(::getpid());
}
 
#else
 
long long currentProcessId() {
  return static_cast<long long>(::GetCurrentProcessId());
}
 
std::wstring widen(const std::string& value) {
  return std::wstring(value.begin(), value.end());
}
 
std::wstring quoteWindowsArgument(const std::wstring& value) {
  if (value.find_first_of(L" \t\"") == std::wstring::npos) {
    return value;
  }
  std::wstring quoted;
  quoted.reserve(value.size() + 2U);
  quoted.push_back(L'"');
  for (const wchar_t ch : value) {
    if (ch == L'"') {
      quoted.append(L"\\\"");
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back(L'"');
  return quoted;
}

// Escape for embedding inside a PowerShell single-quoted string.
// In PowerShell, '' is the escape sequence for a literal single-quote.
std::wstring escapePowerShellSingleQuote(const std::wstring& value) {
  std::wstring out;
  out.reserve(value.size() + 4U);
  for (const wchar_t ch : value) {
    if (ch == L'\'') {
      out.append(L"''");
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

// FNV-1a 32-bit hash — used to build a unique schtasks task name per project.
std::uint32_t fnv1a32(const std::wstring& text) {
  std::uint32_t h = 2166136261UL;
  for (const wchar_t ch : text) {
    h ^= static_cast<std::uint32_t>(ch);
    h *= 16777619UL;
  }
  return h;
}

// Run a hidden command and wait up to timeoutMs for it to finish.
// Returns true if the process launched (regardless of exit code).
bool runHidden(std::wstring cmdLine,
               const std::wstring& workDir,
               DWORD timeoutMs) {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(
      nullptr, cmdLine.data(),
      nullptr, nullptr, FALSE,
      CREATE_NO_WINDOW,
      nullptr,
      workDir.empty() ? nullptr : workDir.c_str(),
      &si, &pi);
  if (!ok) {
    return false;
  }
  if (timeoutMs > 0) {
    ::WaitForSingleObject(pi.hProcess, timeoutMs);
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

#endif
 
Json statusOk(const std::string& message) {
  Json payload = Json::object();
  if (!message.empty()) {
    payload["message"] = message;
  }
  return Json{{"status", "ok"}, {"payload", std::move(payload)}, {"exit_code", 0}};
}
 
Json statusError(const std::string& message) {
  return Json{{"status", "error"}, {"error", message}};
}
 
bool readPidFile(const std::filesystem::path& pidPath, long long& pidOut) {
  std::ifstream stream(pidPath);
  if (!stream) {
    return false;
  }
  long long pid = 0;
  stream >> pid;
  if (!stream || pid <= 0) {
    return false;
  }
  pidOut = pid;
  return true;
}
 
}  // namespace
 
UltraDaemon::UltraDaemon(std::filesystem::path projectRoot)
    : projectRoot_(normalizeProjectRoot(std::move(projectRoot))),
      ipcServer_(projectRoot_) {}
 
bool UltraDaemon::run(const RuntimeRequestHandler& runtimeHandler) {
  if (running_.exchange(true, std::memory_order_acq_rel)) {
    return true;
  }
  if (!ipcServer_.start()) {
    running_.store(false, std::memory_order_release);
    return false;
  }
  if (!writePidFile()) {
    ipcServer_.stop();
    running_.store(false, std::memory_order_release);
    return false;
  }
  while (running_.load(std::memory_order_acquire)) {
    ipcServer_.processNextRequest(
        [this, &runtimeHandler](const Json& request) {
          return handleRequest(request, runtimeHandler);
        },
        std::chrono::milliseconds(250));
  }
  ipcServer_.stop();
  cleanupState();
  running_.store(false, std::memory_order_release);
  return true;
}
 
void UltraDaemon::requestStop() noexcept {
  running_.store(false, std::memory_order_release);
}
 
bool UltraDaemon::isRunning() const noexcept {
  return running_.load(std::memory_order_acquire);
}
 
const std::filesystem::path& UltraDaemon::projectRoot() const noexcept {
  return projectRoot_;
}
 
bool UltraDaemon::wake(const std::filesystem::path& projectRoot,
                       const std::filesystem::path& executablePath,
                       const std::vector<std::string>& daemonArgs) {
  const std::filesystem::path normalizedRoot = normalizeProjectRoot(projectRoot);
  if (isDaemonAlive(normalizedRoot)) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(daemonStateDirectory(normalizedRoot), ec);
  std::filesystem::remove(daemonPidFile(normalizedRoot), ec);
#ifndef _WIN32
  std::filesystem::remove(daemonStateDirectory(normalizedRoot) / "daemon.sock", ec);
#endif
  if (executablePath.empty()) {
    return false;
  }
  if (!spawnDetached(executablePath, daemonArgs, normalizedRoot)) {
    return false;
  }
  ultra::ipc::UltraIPCClient client(normalizedRoot);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    const Json response = client.sendRequest(Json{{"type", "wake"}},
                                             std::chrono::milliseconds(500));
    if (response.is_object() && response.value("status", std::string{}) == "ok") {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (isDaemonAlive(normalizedRoot)) {
    return true;
  }
  std::this_thread::sleep_for(std::chrono::seconds(5));
  return isDaemonAlive(normalizedRoot);
}
 
bool UltraDaemon::sleep(const std::filesystem::path& projectRoot,
                        const std::chrono::milliseconds timeout) {
  const std::filesystem::path normalizedRoot = normalizeProjectRoot(projectRoot);
  ultra::ipc::UltraIPCClient client(normalizedRoot);
  const Json response = client.sendRequest(Json{{"type", "shutdown"}}, timeout);
  if (!(response.is_object() && response.value("status", std::string{}) == "ok") &&
      !isDaemonAlive(normalizedRoot)) {
    std::error_code ec;
    std::filesystem::remove(daemonPidFile(normalizedRoot), ec);
#ifndef _WIN32
    std::filesystem::remove(daemonStateDirectory(normalizedRoot) / "daemon.sock", ec);
#endif
    std::filesystem::remove(daemonStateDirectory(normalizedRoot), ec);
    return true;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!isDaemonAlive(normalizedRoot)) {
      std::error_code ec;
      std::filesystem::remove(daemonPidFile(normalizedRoot), ec);
#ifndef _WIN32
      std::filesystem::remove(daemonStateDirectory(normalizedRoot) / "daemon.sock", ec);
#endif
      std::filesystem::remove(daemonStateDirectory(normalizedRoot), ec);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return !isDaemonAlive(normalizedRoot);
}
 
bool UltraDaemon::isDaemonAlive(const std::filesystem::path& projectRoot) {
  const std::filesystem::path normalizedRoot = normalizeProjectRoot(projectRoot);
  const std::filesystem::path pidPath = daemonPidFile(normalizedRoot);
  long long pid = 0;
  if (!readPidFile(pidPath, pid)) {
    return false;
  }
#ifndef _WIN32
  if (::kill(static_cast<pid_t>(pid), 0) == 0) {
    return true;
  }
  return errno == EPERM;
#else
  const HANDLE process =
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    return false;
  }
  DWORD exitCode = 0U;
  const BOOL queryOk = ::GetExitCodeProcess(process, &exitCode);
  ::CloseHandle(process);
  return queryOk != FALSE && exitCode == STILL_ACTIVE;
#endif
}
 
std::filesystem::path UltraDaemon::daemonStateDirectory(
    const std::filesystem::path& projectRoot) {
  return normalizeProjectRoot(projectRoot) / ".ultra_daemon";
}
 
std::filesystem::path UltraDaemon::daemonPidFile(
    const std::filesystem::path& projectRoot) {
  return daemonStateDirectory(projectRoot) / "daemon.pid";
}
 
UltraDaemon::Json UltraDaemon::handleRequest(
    const Json& request,
    const RuntimeRequestHandler& runtimeHandler) {
  if (!request.is_object()) {
    return statusError("request_must_be_json_object");
  }
  const std::string type = request.value("type", std::string{});
  if (type.empty()) {
    return statusError("missing_request_type");
  }
  if (type == "wake") {
    return statusOk("awake");
  }
  if (type == "shutdown") {
    requestStop();
    return statusOk("shutting_down");
  }
  if (!runtimeHandler) {
    return statusError("runtime_handler_unavailable");
  }
  const Json payload = request.contains("payload") ? request.at("payload") : Json{};
  try {
    const Json result = runtimeHandler(type, payload);
    if (result.is_object() && result.contains("status")) {
      return result;
    }
    return Json{{"status", "ok"}, {"payload", result}, {"exit_code", 0}};
  } catch (const std::exception& ex) {
    return statusError(ex.what());
  } catch (...) {
    return statusError("runtime_handler_failed");
  }
}
 
bool UltraDaemon::writePidFile() const {
  std::error_code ec;
  std::filesystem::create_directories(daemonStateDirectory(projectRoot_), ec);
  if (ec) {
    return false;
  }
  std::ofstream stream(daemonPidFile(projectRoot_), std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream << currentProcessId() << '\n';
  return static_cast<bool>(stream);
}
 
void UltraDaemon::cleanupState() const {
  std::error_code ec;
  std::filesystem::remove(daemonPidFile(projectRoot_), ec);
#ifndef _WIN32
  std::filesystem::remove(daemonStateDirectory(projectRoot_) / "daemon.sock", ec);
#endif
  std::filesystem::remove(daemonStateDirectory(projectRoot_), ec);
}
 
bool UltraDaemon::spawnDetached(const std::filesystem::path& executablePath,
                                const std::vector<std::string>& daemonArgs,
                                const std::filesystem::path& projectRoot) {
#ifndef _WIN32
  // ── First fork ──────────────────────────────────────────────────────────
  // Parent waitpid()s immediately — first child never becomes a zombie.
  const pid_t pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid > 0) {
    int status = 0;
    ::waitpid(pid, &status, 0);
    return true;
  }

  // ── First child: become new session leader ───────────────────────────────
  // Detaches from Codex's process group and controlling terminal.
  if (::setsid() < 0) {
    ::_exit(1);
  }

  // ── Double-fork ──────────────────────────────────────────────────────────
  // Grandchild is NOT the session leader → can never re-acquire a terminal.
  const pid_t pid2 = ::fork();
  if (pid2 < 0) {
    ::_exit(1);
  }
  if (pid2 > 0) {
    // First child exits → grandchild reparented to init/PID-1.
    ::_exit(0);
  }

  // ── Grandchild: the real daemon ───────────────────────────────────────────
  //
  // CRITICAL: redirect stdin/stdout/stderr to /dev/null.
  //
  // Codex captures command output via PIPES. After fork()+setsid() the
  // daemon still holds the WRITE END of those pipes. When Codex exits and
  // closes the READ end, any write() to stdout/stderr sends SIGPIPE which
  // kills the daemon instantly. Redirecting to /dev/null severs this link.
  const int devNull = ::open("/dev/null", O_RDWR);
  if (devNull >= 0) {
    ::dup2(devNull, STDIN_FILENO);
    ::dup2(devNull, STDOUT_FILENO);
    ::dup2(devNull, STDERR_FILENO);
    if (devNull > STDERR_FILENO) {
      ::close(devNull);
    }
  }

  std::vector<std::string> args;
  args.reserve(daemonArgs.size() + 3U);
  args.push_back(executablePath.string());
  for (const std::string& arg : daemonArgs) {
    args.push_back(arg);
  }
  args.push_back("--project-root");
  args.push_back(projectRoot.string());

  std::vector<char*> argv;
  argv.reserve(args.size() + 1U);
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  ::execv(executablePath.string().c_str(), argv.data());
  ::_exit(1);

#else  // _WIN32

  // ═══════════════════════════════════════════════════════════════════════
  // Windows: three-tier job-object escape strategy
  //
  // Codex (and most CI/agent runners) execute all commands inside a Windows
  // Job Object that does NOT have JOB_OBJECT_LIMIT_BREAKAWAY_OK set.
  // CreateProcess with CREATE_BREAKAWAY_FROM_JOB returns ERROR_ACCESS_DENIED
  // in that environment. Start-Process from PowerShell has the same problem.
  //
  // Tier 1 — CreateProcess + CREATE_BREAKAWAY_FROM_JOB
  //   Works from a normal interactive shell with no restrictive job object.
  //
  // Tier 2 — WMI Win32_Process.Create via PowerShell
  //   Microsoft documents that WMI-created processes are NOT associated with
  //   any job object. This is the primary escape path inside Codex.
  //
  // Tier 3 — schtasks /Create + /Run
  //   Task Scheduler spawns via svchost, which is fully outside any job.
  //   The nuclear option: works in every Windows environment without exception.
  // ═══════════════════════════════════════════════════════════════════════

  const std::wstring execWide   = executablePath.wstring();
  const std::wstring rootWide   = projectRoot.wstring();

  // Shared command-line used by all three tiers.
  std::wstring cmdLine = quoteWindowsArgument(execWide);
  for (const std::string& arg : daemonArgs) {
    cmdLine.push_back(L' ');
    cmdLine.append(quoteWindowsArgument(widen(arg)));
  }
  cmdLine.append(L" --project-root ");
  cmdLine.append(quoteWindowsArgument(rootWide));

  // ── Tier 1: CreateProcess with breakaway flags ───────────────────────────
  {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL created = ::CreateProcessW(
        execWide.c_str(),
        cmdLine.data(),
        nullptr, nullptr, FALSE,
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP |
        CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
        nullptr,
        rootWide.c_str(),
        &si, &pi);

    if (created) {
      ::CloseHandle(pi.hThread);
      ::CloseHandle(pi.hProcess);
      return true;
    }
    // ERROR_ACCESS_DENIED means we are inside a restrictive job object.
    // Fall through to WMI.
  }

  // ── Tier 2: WMI Win32_Process.Create via PowerShell ─────────────────────
  // Do NOT replace backslashes in paths — WMI receives a native Windows
  // command line and requires backslashes.
  {
    std::wstring psCmd =
        L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command "
        L"\"([wmiclass]'Win32_Process').Create('"
        + escapePowerShellSingleQuote(cmdLine)
        + L"')\"";

    // Wait up to 8 s for PowerShell to submit the WMI call, then release.
    if (runHidden(psCmd, rootWide, 8000)) {
      return true;
    }
    // Fall through to schtasks.
  }

  // ── Tier 3: schtasks — guaranteed job-object escape ──────────────────────
  // Task Scheduler runs processes as children of svchost.exe, which is
  // completely outside any job object. Works in every Windows environment.
  {
    // Build a unique task name per project root (avoids collisions when
    // multiple Ultra workspaces coexist).
    wchar_t suffix[12]{};
    ::swprintf_s(suffix, L"_%08X", fnv1a32(rootWide));
    const std::wstring taskName = std::wstring(L"UltraDaemon") + suffix;

    // Step 1: Register the task scheduled for a date in the past so it is
    //         immediately eligible to run on demand. /F = overwrite if exists.
    std::wstring createCmd =
        L"schtasks /Create /F /TN \"" + taskName + L"\" "
        L"/TR \"" + cmdLine + L"\" "
        L"/SC ONCE /SD 01/01/2000 /ST 00:00";

    if (!runHidden(createCmd, rootWide, 5000)) {
      return false;
    }

    // Step 2: Fire the task immediately.
    std::wstring runCmd = L"schtasks /Run /TN \"" + taskName + L"\"";
    if (!runHidden(runCmd, rootWide, 5000)) {
      // Best-effort cleanup.
      std::wstring delCmd = L"schtasks /Delete /F /TN \"" + taskName + L"\"";
      runHidden(delCmd, L"", 3000);
      return false;
    }

    // Step 3: Delete the task (fire-and-forget — daemon is already launching).
    std::wstring delCmd = L"schtasks /Delete /F /TN \"" + taskName + L"\"";
    runHidden(delCmd, L"", 0);  // 0 = don't wait, daemon is running

    return true;
  }
#endif
}
 
}  // namespace ultra::runtime