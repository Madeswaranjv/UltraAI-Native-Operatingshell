#include "ipc_server.h"
//E:\Projects\Ultra\src\runtime\ipc_server.cpp
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <limits.h>
#endif
#endif

namespace ultra::runtime {

namespace {

std::filesystem::path runtimeDirFor(const std::filesystem::path& projectRoot) {
  return std::filesystem::absolute(projectRoot).lexically_normal() / ".ultra" /
         "runtime";
}

std::filesystem::path ipcDirFor(const std::filesystem::path& projectRoot) {
  return runtimeDirFor(projectRoot) / "ipc";
}

std::filesystem::path requestsDirFor(const std::filesystem::path& projectRoot) {
  return ipcDirFor(projectRoot) / "requests";
}

std::filesystem::path responsesDirFor(const std::filesystem::path& projectRoot) {
  return ipcDirFor(projectRoot) / "responses";
}

std::filesystem::path daemonLockPathFor(const std::filesystem::path& projectRoot) {
  return runtimeDirFor(projectRoot) / "daemon.lock";
}

std::filesystem::path compatDaemonLockPathFor(
    const std::filesystem::path& projectRoot) {
  return ipcDirFor(projectRoot) / "daemon.lock";
}

std::filesystem::path heartbeatPathFor(const std::filesystem::path& projectRoot) {
  return runtimeDirFor(projectRoot) / "daemon_heartbeat";
}

std::filesystem::path compatHeartbeatPathFor(
    const std::filesystem::path& projectRoot) {
  return ipcDirFor(projectRoot) / "heartbeat.json";
}

std::filesystem::path socketPathFor(const std::filesystem::path& projectRoot) {
  return runtimeDirFor(projectRoot) / "daemon.sock";
}

std::filesystem::path namedPipeMarkerPathFor(
    const std::filesystem::path& projectRoot) {
  return runtimeDirFor(projectRoot) / "daemon.pipe";
}

std::uint64_t unixTimeMillisNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

unsigned long processId() {
#ifdef _WIN32
  return static_cast<unsigned long>(::_getpid());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

bool readJsonFile(const std::filesystem::path& path,
                  nlohmann::json& payload,
                  std::string& error) {
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      error = "Failed to open JSON file: " + path.string();
      return false;
    }
    input >> payload;
    return true;
  } catch (const std::exception& ex) {
    error = std::string("Failed to parse JSON file: ") + ex.what();
    return false;
  }
}

bool clearDirectoryFiles(const std::filesystem::path& dir, std::string& error) {
  try {
    if (!std::filesystem::exists(dir)) {
      return true;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      std::error_code ec;
      std::filesystem::remove_all(entry.path(), ec);
      if (ec) {
        error = "Failed to remove stale IPC artifact: " + entry.path().string();
        return false;
      }
    }
    return true;
  } catch (const std::exception& ex) {
    error = std::string("Failed to clear IPC directory: ") + ex.what();
    return false;
  }
}

bool processExists(const unsigned long pid) {
  if (pid == 0UL) {
    return false;
  }
#ifdef _WIN32
  HANDLE process =
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    return false;
  }
  DWORD exitCode = 0;
  const BOOL ok = ::GetExitCodeProcess(process, &exitCode);
  ::CloseHandle(process);
  return ok != FALSE && exitCode == STILL_ACTIVE;
#else
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

bool terminateProcessByPid(const unsigned long pid, std::string& error) {
  if (pid == 0UL) {
    error = "Daemon pid is missing.";
    return false;
  }
#ifdef _WIN32
  HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                 static_cast<DWORD>(pid));
  if (process == nullptr) {
    error = "Failed to open daemon process for termination.";
    return false;
  }
  const BOOL terminated = ::TerminateProcess(process, 1U);
  if (terminated != FALSE) {
    (void)::WaitForSingleObject(process, 2000U);
  }
  ::CloseHandle(process);
  if (terminated == FALSE) {
    error = "Failed to terminate daemon process.";
    return false;
  }
  return true;
#else
  if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0 && errno != ESRCH) {
    error = std::string("Failed to terminate daemon process: ") +
            std::strerror(errno);
    return false;
  }
  return true;
#endif
}

std::size_t processMemoryUsageBytes(const unsigned long pid) {
  if (pid == 0UL) {
    return 0U;
  }
#ifdef _WIN32
  HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                 FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    return 0U;
  }
  PROCESS_MEMORY_COUNTERS_EX counters{};
  const BOOL ok = ::GetProcessMemoryInfo(
      process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
      sizeof(counters));
  ::CloseHandle(process);
  if (ok == FALSE) {
    return 0U;
  }
  return static_cast<std::size_t>(counters.WorkingSetSize);
#elif defined(__linux__)
  std::ifstream statm("/proc/" + std::to_string(pid) + "/statm");
  long totalPages = 0;
  long residentPages = 0;
  if (!(statm >> totalPages >> residentPages)) {
    return 0U;
  }
  (void)totalPages;
  const long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) {
    return 0U;
  }
  return static_cast<std::size_t>(residentPages) *
         static_cast<std::size_t>(pageSize);
#else
  return 0U;
#endif
}

std::filesystem::path currentExecutablePath() {
#ifdef _WIN32
  std::vector<char> buffer(MAX_PATH, '\0');
  while (true) {
    const DWORD length = ::GetModuleFileNameA(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (length == 0U) {
      return {};
    }
    if (length < buffer.size() - 1U) {
      return std::filesystem::path(
                 std::string(buffer.data(), static_cast<std::size_t>(length)))
          .lexically_normal();
    }
    buffer.resize(buffer.size() * 2U);
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0U;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1U, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::path(buffer.data()).lexically_normal();
#elif defined(__linux__)
  std::vector<char> buffer(PATH_MAX, '\0');
  const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (length <= 0) {
    return {};
  }
  return std::filesystem::path(
             std::string(buffer.data(), static_cast<std::size_t>(length)))
      .lexically_normal();
#else
  return {};
#endif
}

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::filesystem::path processExecutablePath(const unsigned long pid) {
#ifdef _WIN32
  if (pid == 0UL) {
    return {};
  }
  HANDLE processHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                       static_cast<DWORD>(pid));
  if (processHandle == nullptr) {
    return {};
  }

  std::vector<char> buffer(MAX_PATH, '\0');
  DWORD size = static_cast<DWORD>(buffer.size());
  while (::QueryFullProcessImageNameA(processHandle, 0, buffer.data(), &size) ==
         FALSE) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_INSUFFICIENT_BUFFER) {
      buffer.resize(buffer.size() * 2U);
      size = static_cast<DWORD>(buffer.size());
      continue;
    }
    ::CloseHandle(processHandle);
    return {};
  }
  ::CloseHandle(processHandle);
  return std::filesystem::path(
             std::string(buffer.data(), static_cast<std::size_t>(size)))
      .lexically_normal();
#elif defined(__linux__)
  if (pid == 0UL) {
    return {};
  }
  std::vector<char> buffer(PATH_MAX, '\0');
  const std::string procPath = "/proc/" + std::to_string(pid) + "/exe";
  const ssize_t length = ::readlink(procPath.c_str(), buffer.data(), buffer.size());
  if (length <= 0) {
    return {};
  }
  return std::filesystem::path(
             std::string(buffer.data(), static_cast<std::size_t>(length)))
      .lexically_normal();
#else
  (void)pid;
  return {};
#endif
}

bool processMatchesCurrentExecutable(const unsigned long pid) {
  const std::filesystem::path selfPath = currentExecutablePath();
  const std::filesystem::path pidPath = processExecutablePath(pid);
  if (selfPath.empty() || pidPath.empty()) {
    return false;
  }

  const std::string selfName = lowerAscii(selfPath.filename().string());
  const std::string pidName = lowerAscii(pidPath.filename().string());
  return !selfName.empty() && selfName == pidName;
}

#ifdef _WIN32
std::string narrow(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int sourceLength = static_cast<int>(value.size());
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), sourceLength,
                                         nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(size), '\0');
  const int converted = ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                              sourceLength, out.data(), size,
                                              nullptr, nullptr);
  if (converted <= 0) {
    return {};
  }
  return out;
}

std::string formatWindowsError(const DWORD errorCode) {
  LPWSTR buffer = nullptr;
  const DWORD flags =
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = ::FormatMessageW(flags, nullptr, errorCode, 0,
                                        reinterpret_cast<LPWSTR>(&buffer), 0,
                                        nullptr);
  std::wstring message;
  if (length != 0U && buffer != nullptr) {
    message.assign(buffer, buffer + length);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ')) {
      message.pop_back();
    }
  }
  if (buffer != nullptr) {
    ::LocalFree(buffer);
  }
  if (message.empty()) {
    return "Win32 error " + std::to_string(static_cast<unsigned long>(errorCode));
  }
  return narrow(message) + " (code " +
         std::to_string(static_cast<unsigned long>(errorCode)) + ")";
}

bool spawnDetachedWithCreateProcess(const std::filesystem::path& executable,
                                    const std::filesystem::path& projectRoot,
                                    const bool requestBreakaway,
                                    std::string& error) {
  std::wstring commandLine =
      L"\"" + executable.wstring() + L"\" wake_ai --uair-child --workspace \"" +
      projectRoot.wstring() + L"\"";
  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  startupInfo.dwFlags = STARTF_USESHOWWINDOW;
  startupInfo.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION processInfo{};
  std::wstring workingDirectory = projectRoot.wstring();
  const DWORD creationFlags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP |
                              CREATE_NO_WINDOW |
                              (requestBreakaway ? CREATE_BREAKAWAY_FROM_JOB : 0U);
  if (::CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr,
                       nullptr, FALSE, creationFlags, nullptr,
                       workingDirectory.c_str(), &startupInfo, &processInfo) ==
      FALSE) {
    error = "Failed to launch UAIR child with CreateProcessW: " +
            formatWindowsError(::GetLastError());
    return false;
  }
  ::CloseHandle(processInfo.hThread);
  ::CloseHandle(processInfo.hProcess);
  return true;
}

bool spawnDetachedWithShellExecute(const std::filesystem::path& executable,
                                   const std::filesystem::path& projectRoot,
                                   std::string& error) {
  const std::wstring executablePath = executable.wstring();
  const std::wstring parameters =
      L"wake_ai --uair-child --workspace \"" + projectRoot.wstring() + L"\"";
  const std::wstring workingDirectory = projectRoot.wstring();
  SHELLEXECUTEINFOW execInfo{};
  execInfo.cbSize = sizeof(execInfo);
  execInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  execInfo.lpVerb = L"open";
  execInfo.lpFile = executablePath.c_str();
  execInfo.lpParameters = parameters.c_str();
  execInfo.lpDirectory = workingDirectory.c_str();
  execInfo.nShow = SW_HIDE;
  if (::ShellExecuteExW(&execInfo) == FALSE) {
    error = "Failed to launch UAIR child with ShellExecuteExW: " +
            formatWindowsError(::GetLastError());
    return false;
  }
  if (execInfo.hProcess != nullptr) {
    ::CloseHandle(execInfo.hProcess);
  }
  return true;
}
#endif

bool spawnDetachedProcess(const std::filesystem::path& projectRoot,
                          std::string& error) {
  const std::filesystem::path executable = currentExecutablePath();
  if (executable.empty()) {
    error = "Failed to resolve current executable path for daemon spawn.";
    return false;
  }

#ifdef _WIN32
  std::string createProcessError;
  if (spawnDetachedWithCreateProcess(executable, projectRoot, true,
                                     createProcessError)) {
    return true;
  }
  if (spawnDetachedWithCreateProcess(executable, projectRoot, false,
                                     createProcessError)) {
    return true;
  }
  std::string shellExecuteError;
  if (spawnDetachedWithShellExecute(executable, projectRoot, shellExecuteError)) {
    return true;
  }
  error = createProcessError + "; fallback: " + shellExecuteError;
  return false;
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    error = std::string("Failed to fork detached UAIR child: ") +
            std::strerror(errno);
    return false;
  }
  if (pid == 0) {
    ::setsid();
    ::chdir(projectRoot.string().c_str());
    ::setenv("ULTRA_UAIR_CHILD", "1", 1);
    ::execl(executable.string().c_str(), executable.string().c_str(), "wake_ai",
            "--uair-child", "--workspace", projectRoot.string().c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }
  return true;
#endif
}

IpcResponse buildErrorResponse(const std::string& message) {
  IpcResponse response;
  response.ok = false;
  response.exitCode = 1;
  response.message = message;
  response.payload = nlohmann::json::object();
  return response;
}

}  // namespace

IpcServer::IpcServer(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()),
      runtimeDir_(runtimeDirFor(projectRoot_)),
      ipcDir_(ipcDirFor(projectRoot_)),
      requestsDir_(requestsDirFor(projectRoot_)),
      responsesDir_(responsesDirFor(projectRoot_)),
      lockFile_(daemonLockPathFor(projectRoot_)),
      heartbeatFile_(heartbeatPathFor(projectRoot_)),
      socketFile_(socketPathFor(projectRoot_)),
      transport_(ultra::ipc::makePlatformTransport(projectRoot_)) {}

bool IpcServer::start(std::string& error) {
  std::cout << "[UAIR] daemon start\n";
  try {
    std::filesystem::create_directories(runtimeDir_);
    std::filesystem::create_directories(requestsDir_);
    std::filesystem::create_directories(responsesDir_);
  } catch (const std::exception& ex) {
    error = std::string("Failed to create IPC directories: ") + ex.what();
    return false;
  }

  if (daemonLockPresent(projectRoot_)) {
    const DaemonHealthReport health = inspectDaemon(projectRoot_);
    if (health.state == DaemonHealthState::Dead ||
        health.state == DaemonHealthState::Corrupt ||
        health.state == DaemonHealthState::Stale) {
      if (!cleanupStaleArtifacts(projectRoot_, error)) {
        return false;
      }
    } else {
      error = health.message.empty() ? "Daemon already running." : health.message;
      return false;
    }
  }

  if (!clearDirectoryFiles(requestsDir_, error) ||
      !clearDirectoryFiles(responsesDir_, error)) {
    return false;
  }
  {
    std::error_code ec;
    std::filesystem::remove(socketFile_, ec);
    std::filesystem::remove(namedPipeMarkerPathFor(projectRoot_), ec);
    std::filesystem::remove(heartbeatFile_, ec);
    std::filesystem::remove(compatHeartbeatPathFor(projectRoot_), ec);
  }

  if (!transport_) {
    error = "IPC transport initialization failed.";
    return false;
  }
  if (!transport_->start(error)) {
    return false;
  }

  nlohmann::json lockPayload;
  lockPayload["pid"] = currentProcessId();
  lockPayload["started_at"] = unixTimeMillisNow();
  lockPayload["start_timestamp"] = lockPayload["started_at"];
  lockPayload["transport_endpoint"] = socketFile_.generic_string();
  lockPayload["status"] = "running";

  if (!writeJsonAtomically(lockFile_, lockPayload, error)) {
    return false;
  }
  if (!writeJsonAtomically(compatDaemonLockPathFor(projectRoot_), lockPayload,
                           error)) {
    return false;
  }
  std::cout << "[UAIR] daemon lock created pid=" << currentProcessId() << '\n';

  std::ofstream socketOut(socketFile_, std::ios::binary | std::ios::trunc);
  if (!socketOut) {
    error = "Failed to create daemon socket marker.";
    return false;
  }
  socketOut << "ultra-daemon-socket";

  started_ = true;

  DaemonHeartbeat heartbeat;
  heartbeat.runtimeActive = true;
  if (!writeHeartbeat(heartbeat, error)) {
    stop();
    return false;
  }
  std::cout << "[UAIR] heartbeat written\n";
  std::cout << "[UAIR] daemon healthy\n";

  return true;
}

void IpcServer::stop() {
  std::cout << "[UAIR] daemon shutdown\n";
  started_ = false;
  if (transport_) {
    transport_->stop();
  }

  std::error_code ec;
  std::filesystem::remove(lockFile_, ec);
  std::filesystem::remove(compatDaemonLockPathFor(projectRoot_), ec);
  std::filesystem::remove(heartbeatFile_, ec);
  std::filesystem::remove(compatHeartbeatPathFor(projectRoot_), ec);
  std::filesystem::remove(socketFile_, ec);
  std::filesystem::remove(namedPipeMarkerPathFor(projectRoot_), ec);
}

bool IpcServer::writeHeartbeat(const DaemonHeartbeat& heartbeat,
                               std::string& error) const {
  nlohmann::json payload;
  payload["pid"] = currentProcessId();
  payload["lastHeartbeat"] =
      heartbeat.lastHeartbeat == 0U ? unixTimeMillisNow() : heartbeat.lastHeartbeat;
  payload["runtimeActive"] = heartbeat.runtimeActive;
  payload["indexedFiles"] = heartbeat.indexedFiles;
  payload["symbolsIndexed"] = heartbeat.symbolsIndexed;
  payload["dependenciesIndexed"] = heartbeat.dependenciesIndexed;
  payload["graphNodes"] = heartbeat.graphNodes;
  payload["graphEdges"] = heartbeat.graphEdges;
  payload["pendingChanges"] = heartbeat.pendingChanges;
  payload["hotSliceSize"] = heartbeat.hotSliceSize;
  payload["snapshotCount"] = heartbeat.snapshotCount;
  payload["activeBranchCount"] = heartbeat.activeBranchCount;
  payload["memoryUsageBytes"] = currentProcessMemoryUsageBytes();

  if (!writeJsonAtomically(heartbeatFile_, payload, error)) {
    return false;
  }
  return writeJsonAtomically(compatHeartbeatPathFor(projectRoot_), payload,
                             error);
}

bool IpcServer::processRequests(const RequestHandler& handler,
                                const std::size_t maxRequests,
                                std::string& error) {
  if (!started_) {
    return true;
  }
  if (!transport_) {
    error = "IPC transport is not initialized.";
    return false;
  }

  std::size_t processed = 0U;
  while (processed < maxRequests) {
    ultra::ipc::TransportMessage message;
    if (!transport_->receive(message, std::chrono::milliseconds(20), error)) {
      return false;
    }
    if (message.id.empty()) {
      break;
    }

    IpcRequest request;
    request.id = message.id;
    request.command = message.command;
    request.type = message.command;
    request.payload = message.payload.is_object() ? message.payload
                                                  : nlohmann::json::object();

    IpcResponse response;
    try {
      response = handler(request);
    } catch (const std::exception& ex) {
      response = buildErrorResponse(
          std::string("Unhandled daemon request failure: ") + ex.what());
    } catch (...) {
      response = buildErrorResponse("Unhandled daemon request failure.");
    }

    nlohmann::json responseJson;
    responseJson["id"] = request.id;
    responseJson["status"] = response.ok ? "ok" : "error";
    responseJson["ok"] = response.ok;
    responseJson["exit_code"] = response.exitCode;
    responseJson["message"] = response.message;
    responseJson["payload"] = response.payload;
    responseJson["data"] = response.payload;

    if (!transport_->respond(request.id, responseJson, error)) {
      return false;
    }

    ++processed;
  }

  return true;
}

bool IpcServer::daemonLockPresent(const std::filesystem::path& projectRoot) {
  return std::filesystem::exists(daemonLockPathFor(projectRoot)) ||
         std::filesystem::exists(compatDaemonLockPathFor(projectRoot));
}

DaemonHealthReport IpcServer::inspectDaemon(
    const std::filesystem::path& projectRoot,
    const std::chrono::milliseconds freshness) {
  DaemonHealthReport report;

  const std::filesystem::path primaryLock = daemonLockPathFor(projectRoot);
  const std::filesystem::path compatLock = compatDaemonLockPathFor(projectRoot);
  const std::filesystem::path lockPath =
      std::filesystem::exists(primaryLock) ? primaryLock : compatLock;

  if (!std::filesystem::exists(lockPath)) {
    report.state = DaemonHealthState::Missing;
    report.message = "Daemon lock file is missing.";
    return report;
  }

  nlohmann::json lockInfo;
  std::string parseError;
  if (!readJsonFile(lockPath, lockInfo, parseError)) {
    report.state = DaemonHealthState::Corrupt;
    report.message = parseError;
    return report;
  }

  report.pid = lockInfo.value("pid", 0UL);
  report.startedAt = lockInfo.value("started_at", 0ULL);
  if (report.pid == 0UL) {
    report.state = DaemonHealthState::Corrupt;
    report.message = "Daemon lock file is missing a valid pid.";
    return report;
  }
  if (!processExists(report.pid)) {
    report.state = DaemonHealthState::Dead;
    report.message = "Daemon process is not alive.";
    return report;
  }
  if (!processMatchesCurrentExecutable(report.pid)) {
    report.state = DaemonHealthState::Dead;
    report.message =
        "Daemon lock pid does not reference the running ultra daemon process.";
    return report;
  }
  report.memoryUsageBytes = processMemoryUsageBytes(report.pid);

  const std::filesystem::path primaryHeartbeat = heartbeatPathFor(projectRoot);
  const std::filesystem::path compatHeartbeat = compatHeartbeatPathFor(projectRoot);
  const std::filesystem::path heartbeatPath =
      std::filesystem::exists(primaryHeartbeat) ? primaryHeartbeat : compatHeartbeat;

  if (!std::filesystem::exists(heartbeatPath)) {
    report.state = DaemonHealthState::Stale;
    report.message = "Daemon heartbeat is missing.";
    return report;
  }

  nlohmann::json heartbeat;
  if (!readJsonFile(heartbeatPath, heartbeat, parseError)) {
    report.state = DaemonHealthState::Corrupt;
    report.message = parseError;
    return report;
  }

  const unsigned long heartbeatPid = heartbeat.value("pid", 0UL);
  if (heartbeatPid != 0UL && heartbeatPid != report.pid) {
    report.state = DaemonHealthState::Corrupt;
    report.message = "Daemon heartbeat pid does not match lock pid.";
    return report;
  }

  report.lastHeartbeat = heartbeat.value("lastHeartbeat", 0ULL);
  report.runtimeActive = heartbeat.value("runtimeActive", false);
  report.indexedFiles = heartbeat.value("indexedFiles", 0U);
  report.symbolsIndexed = heartbeat.value("symbolsIndexed", 0U);
  report.dependenciesIndexed = heartbeat.value("dependenciesIndexed", 0U);
  report.graphNodes = heartbeat.value("graphNodes", 0U);
  report.graphEdges = heartbeat.value("graphEdges", 0U);
  report.pendingChanges = heartbeat.value("pendingChanges", 0U);
  if (heartbeat.contains("memoryUsageBytes") &&
      heartbeat["memoryUsageBytes"].is_number_unsigned()) {
    report.memoryUsageBytes = heartbeat.value("memoryUsageBytes",
                                              report.memoryUsageBytes);
  }

  if (report.lastHeartbeat == 0U) {
    report.state = DaemonHealthState::Stale;
    report.message = "Daemon heartbeat timestamp is missing.";
    return report;
  }

  const std::uint64_t now = unixTimeMillisNow();
  const std::uint64_t age =
      now > report.lastHeartbeat ? (now - report.lastHeartbeat) : 0U;
  if (age > static_cast<std::uint64_t>(freshness.count())) {
    report.state = DaemonHealthState::Stale;
    report.message =
        "Daemon heartbeat is stale (" + std::to_string(age) + " ms old).";
    return report;
  }

  report.state = DaemonHealthState::Healthy;
  report.message = "Daemon is healthy.";
  return report;
}

bool IpcServer::waitForHealthy(const std::filesystem::path& projectRoot,
                               const std::chrono::milliseconds timeout,
                               DaemonHealthReport& reportOut,
                               std::string& error,
                               const std::chrono::milliseconds freshness) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    reportOut = inspectDaemon(projectRoot, freshness);
    if (reportOut.healthy()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (std::chrono::steady_clock::now() < deadline);

  error = reportOut.message.empty() ? "Timed out waiting for healthy UAIR daemon."
                                    : reportOut.message;
  return false;
}

bool IpcServer::cleanupStaleArtifacts(const std::filesystem::path& projectRoot,
                                      std::string& error) {
  try {
    std::filesystem::create_directories(requestsDirFor(projectRoot));
    std::filesystem::create_directories(responsesDirFor(projectRoot));
  } catch (const std::exception& ex) {
    error = std::string("Failed to prepare IPC directories: ") + ex.what();
    return false;
  }

  if (!clearDirectoryFiles(requestsDirFor(projectRoot), error) ||
      !clearDirectoryFiles(responsesDirFor(projectRoot), error)) {
    return false;
  }

  std::error_code ec;
  std::filesystem::remove(daemonLockPathFor(projectRoot), ec);
  std::filesystem::remove(compatDaemonLockPathFor(projectRoot), ec);
  std::filesystem::remove(heartbeatPathFor(projectRoot), ec);
  std::filesystem::remove(compatHeartbeatPathFor(projectRoot), ec);
  std::filesystem::remove(socketPathFor(projectRoot), ec);
  std::filesystem::remove(namedPipeMarkerPathFor(projectRoot), ec);
  return true;
}

bool IpcServer::terminateProcessForProject(
    const std::filesystem::path& projectRoot,
    std::string& error,
    const std::chrono::milliseconds waitTimeout) {
  const DaemonHealthReport health =
      inspectDaemon(projectRoot, std::chrono::hours(24));
  if (health.state == DaemonHealthState::Missing ||
      health.state == DaemonHealthState::Dead) {
    return cleanupStaleArtifacts(projectRoot, error);
  }

  if (!terminateProcessByPid(health.pid, error)) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + waitTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!processExists(health.pid)) {
      return cleanupStaleArtifacts(projectRoot, error);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  error = "Timed out waiting for daemon process to terminate.";
  return false;
}

bool IpcServer::spawnDetached(const std::filesystem::path& projectRoot,
                              std::string& error) {
  return spawnDetachedProcess(projectRoot, error);
}

unsigned long IpcServer::currentProcessId() {
  return processId();
}

std::size_t IpcServer::currentProcessMemoryUsageBytes() {
  return processMemoryUsageBytes(currentProcessId());
}

bool IpcServer::writeJsonAtomically(const std::filesystem::path& outputPath,
                                    const nlohmann::json& payload,
                                    std::string& error) const {
  try {
    const std::filesystem::path tmpPath =
        outputPath.string() + ".tmp-" + std::to_string(processId());
    {
      std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
      if (!out) {
        error = "Failed to open temporary IPC file: " + tmpPath.string();
        return false;
      }
      out << payload.dump(2);
      out.flush();
      if (!out) {
        error = "Failed to write temporary IPC file: " + tmpPath.string();
        return false;
      }
    }

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
    std::filesystem::rename(tmpPath, outputPath);
    return true;
  } catch (const std::exception& ex) {
    error = std::string("Failed to write IPC payload: ") + ex.what();
    return false;
  }
}

}  // namespace ultra::runtime
