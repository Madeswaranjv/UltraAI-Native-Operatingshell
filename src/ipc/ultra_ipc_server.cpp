#include "ultra/ipc/ultra_ipc_server.h"
 
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
//E:\Projects\Ultra\src\ipc\ultra_ipc_server.cpp
#ifndef _WIN32
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
 
namespace ultra::ipc {
namespace {
 
using Json = nlohmann::json;
 
constexpr std::size_t kMaxMessageBytes = 1024U * 1024U;
 
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
 
std::filesystem::path stateDirectoryFor(const std::filesystem::path& projectRoot) {
  return projectRoot / ".ultra_daemon";
}
 
Json errorResponse(const std::string& message) {
  return Json{{"status", "error"}, {"error", message}};
}
 
#ifndef _WIN32
 
std::filesystem::path socketPathFor(const std::filesystem::path& projectRoot) {
  return stateDirectoryFor(projectRoot) / "daemon.sock";
}
 
bool writeAllFd(const int fd, const std::string& payload) {
  std::size_t offset = 0U;
  while (offset < payload.size()) {
    const ssize_t sent = ::send(fd, payload.data() + offset, payload.size() - offset, 0);
    if (sent <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}
 
bool readLineFd(const int fd, std::string& output) {
  output.clear();
  char byte = '\0';
  while (output.size() < kMaxMessageBytes) {
    const ssize_t received = ::recv(fd, &byte, 1, 0);
    if (received == 0) {
      break;
    }
    if (received < 0) {
      return false;
    }
    if (byte == '\n') {
      return true;
    }
    output.push_back(byte);
  }
  return !output.empty();
}
 
#else
 
std::string lowercase(std::string value) {
  std::transform(value.begin(),
                 value.end(),
                 value.begin(),
                 [](const unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}
 
std::uint64_t fnv1a64(const std::string& text) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ULL;
  }
  return hash;
}
 
std::wstring toWide(const std::string& input) {
  return std::wstring(input.begin(), input.end());
}
 
std::wstring pipeNameFor(const std::filesystem::path& projectRoot) {
  const std::string normalized = lowercase(normalizeProjectRoot(projectRoot).generic_string());
  std::ostringstream stream;
  stream << std::hex << fnv1a64(normalized);
  return L"\\\\.\\pipe\\ultra_" + toWide(stream.str());
}
 
bool writeAllHandle(HANDLE handle, const std::string& payload) {
  std::size_t offset = 0U;
  while (offset < payload.size()) {
    DWORD bytesWritten = 0U;
    const BOOL written =
        ::WriteFile(handle,
                    payload.data() + offset,
                    static_cast<DWORD>(payload.size() - offset),
                    &bytesWritten,
                    nullptr);
    if (!written || bytesWritten == 0U) {
      return false;
    }
    offset += static_cast<std::size_t>(bytesWritten);
  }
  return true;
}
 
bool readLineHandle(HANDLE handle, std::string& output) {
  output.clear();
  char byte = '\0';
  while (output.size() < kMaxMessageBytes) {
    DWORD bytesRead = 0U;
    const BOOL ok = ::ReadFile(handle, &byte, 1U, &bytesRead, nullptr);
    if (!ok || bytesRead == 0U) {
      break;
    }
    if (byte == '\n') {
      return true;
    }
    output.push_back(byte);
  }
  return !output.empty();
}
 
bool connectNamedPipeWithTimeout(HANDLE pipeHandle,
                                 const std::chrono::milliseconds timeout) {
  OVERLAPPED overlapped{};
  overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlapped.hEvent == nullptr) {
    return false;
  }
 
  const BOOL connectOk = ::ConnectNamedPipe(pipeHandle, &overlapped);
  if (connectOk != FALSE) {
    ::CloseHandle(overlapped.hEvent);
    return true;
  }
 
  const DWORD connectError = ::GetLastError();
  if (connectError == ERROR_PIPE_CONNECTED) {
    ::SetEvent(overlapped.hEvent);
    ::CloseHandle(overlapped.hEvent);
    return true;
  }
 
  if (connectError != ERROR_IO_PENDING) {
    ::CloseHandle(overlapped.hEvent);
    return false;
  }
 
  const DWORD waitMs = timeout.count() <= 0
                           ? INFINITE
                           : static_cast<DWORD>(timeout.count());
  const DWORD waitResult = ::WaitForSingleObject(overlapped.hEvent, waitMs);
  if (waitResult != WAIT_OBJECT_0) {
    ::CancelIoEx(pipeHandle, &overlapped);
    ::CloseHandle(overlapped.hEvent);
    return false;
  }
 
  DWORD transferred = 0U;
  const BOOL result = ::GetOverlappedResult(pipeHandle, &overlapped, &transferred, FALSE);
  ::CloseHandle(overlapped.hEvent);
  return result != FALSE;
}
 
#endif
 
}  // namespace
 
UltraIPCServer::UltraIPCServer(std::filesystem::path projectRoot)
    : projectRoot_(normalizeProjectRoot(std::move(projectRoot)))
#ifndef _WIN32
      , socketPath_(socketPathFor(projectRoot_))
#else
      , pipeName_(pipeNameFor(projectRoot_))
#endif
{
}
 
UltraIPCServer::~UltraIPCServer() {
  stop();
}
 
bool UltraIPCServer::start() {
  if (running_) {
    return true;
  }
 
  std::error_code ec;
  std::filesystem::create_directories(stateDirectory(), ec);
  if (ec) {
    return false;
  }
 
#ifndef _WIN32
  std::filesystem::remove(socketPath_, ec);
 
  listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listenFd_ < 0) {
    return false;
  }
 
  ::sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string socketPathString = socketPath_.string();
  if (socketPathString.size() >= sizeof(address.sun_path)) {
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  std::strncpy(address.sun_path, socketPathString.c_str(), sizeof(address.sun_path) - 1U);
 
  if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
 
  if (::listen(listenFd_, 16) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
#endif
 
  running_ = true;
  return true;
}
 
void UltraIPCServer::stop() {
  if (!running_) {
    return;
  }
 
#ifndef _WIN32
  if (listenFd_ >= 0) {
    ::close(listenFd_);
    listenFd_ = -1;
  }
  std::error_code ec;
  std::filesystem::remove(socketPath_, ec);
#endif
 
  running_ = false;
}
 
bool UltraIPCServer::processNextRequest(const RequestHandler& handler,
                                        const std::chrono::milliseconds timeout) {
  if (!running_ || !handler) {
    return false;
  }
 
#ifndef _WIN32
  ::pollfd pfd{};
  pfd.fd = listenFd_;
  pfd.events = POLLIN;
 
  const int timeoutMs = timeout.count() <= 0 ? 0 : static_cast<int>(timeout.count());
  const int pollResult = ::poll(&pfd, 1, timeoutMs);
  if (pollResult <= 0) {
    return false;
  }
 
  const int clientFd = ::accept(listenFd_, nullptr, nullptr);
  if (clientFd < 0) {
    return false;
  }
 
  std::string requestLine;
  const bool readOk = readLineFd(clientFd, requestLine);
  Json response;
  if (!readOk) {
    response = errorResponse("client_disconnected");
  } else {
    Json request = Json::parse(requestLine, nullptr, false);
    if (request.is_discarded()) {
      response = errorResponse("invalid_json");
    } else {
      try {
        response = handler(request);
      } catch (...) {
        response = errorResponse("request_handler_failed");
      }
    }
  }
 
  std::string payload = response.dump();
  payload.push_back('\n');
  writeAllFd(clientFd, payload);
  ::close(clientFd);
  return true;
#else
  const HANDLE pipeHandle = ::CreateNamedPipeW(
      pipeName_.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,  // byte-stream: newline-delimited JSON, NOT message-mode
      PIPE_UNLIMITED_INSTANCES,
      static_cast<DWORD>(kMaxMessageBytes),
      static_cast<DWORD>(kMaxMessageBytes),
      0,
      nullptr);
  if (pipeHandle == INVALID_HANDLE_VALUE) {
    return false;
  }
 
  if (!connectNamedPipeWithTimeout(pipeHandle, timeout)) {
    ::CloseHandle(pipeHandle);
    return false;
  }
 
  std::string requestLine;
  const bool readOk = readLineHandle(pipeHandle, requestLine);
  Json response;
  if (!readOk) {
    response = errorResponse("client_disconnected");
  } else {
    Json request = Json::parse(requestLine, nullptr, false);
    if (request.is_discarded()) {
      response = errorResponse("invalid_json");
    } else {
      try {
        response = handler(request);
      } catch (...) {
        response = errorResponse("request_handler_failed");
      }
    }
  }
 
  std::string payload = response.dump();
  payload.push_back('\n');
  writeAllHandle(pipeHandle, payload);
 
  ::FlushFileBuffers(pipeHandle);
  ::DisconnectNamedPipe(pipeHandle);
  ::CloseHandle(pipeHandle);
  return true;
#endif
}
 
bool UltraIPCServer::isRunning() const noexcept {
  return running_;
}
 
const std::filesystem::path& UltraIPCServer::projectRoot() const noexcept {
  return projectRoot_;
}
 
std::filesystem::path UltraIPCServer::stateDirectory() const {
  return stateDirectoryFor(projectRoot_);
}
 
#ifndef _WIN32
std::filesystem::path UltraIPCServer::socketPath() const {
  return socketPath_;
}
#else
const std::wstring& UltraIPCServer::pipeName() const noexcept {
  return pipeName_;
}
#endif
 
}  // namespace ultra::ipc