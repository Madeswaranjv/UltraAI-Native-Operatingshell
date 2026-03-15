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
#include <sys/types.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    const Json response = client.sendRequest(Json{{"type", "wake"}},
                                             std::chrono::milliseconds(500));
    if (response.is_object() && response.value("status", std::string{}) == "ok") {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
 
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
 
  // Forward ALL non-built-in request types to the runtimeHandler.
  // Previously only "ai_query" and "command" were forwarded; every other
  // type ("ai_status", "rebuild_ai", "ai_context", "ai_source", "ai_impact",
  // "ai_metrics", etc.) fell through to unsupported_request_type.
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
  const pid_t pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid > 0) {
    return true;
  }
 
  if (::setsid() < 0) {
    ::_exit(1);
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
#else
  std::wstring executableWide = executablePath.wstring();
  std::wstring commandLine = quoteWindowsArgument(executableWide);
  for (const std::string& arg : daemonArgs) {
    commandLine.push_back(L' ');
    commandLine.append(quoteWindowsArgument(widen(arg)));
  }
  commandLine.append(L" --project-root ");
  commandLine.append(quoteWindowsArgument(projectRoot.wstring()));
 
  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
 
  PROCESS_INFORMATION processInfo{};
  const BOOL created = ::CreateProcessW(executableWide.c_str(),
                                        commandLine.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
                                        nullptr,
                                        projectRoot.wstring().c_str(),
                                        &startupInfo,
                                        &processInfo);
  if (!created) {
    return false;
  }
 
  ::CloseHandle(processInfo.hThread);
  ::CloseHandle(processInfo.hProcess);
  return true;
#endif
}
 
}  // namespace ultra::runtime
