#include "ultra/ipc/ultra_ipc_client.h"
 
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
//E:\Projects\Ultra\src\ipc\ultra_ipc_client.cpp
#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
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
    const ssize_t written = ::send(fd, payload.data() + offset, payload.size() - offset, 0);
    if (written <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
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
    const BOOL ok = ::WriteFile(handle,
                                payload.data() + offset,
                                static_cast<DWORD>(payload.size() - offset),
                                &bytesWritten,
                                nullptr);
    if (!ok || bytesWritten == 0U) {
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
 
#endif
 
}  // namespace
 
UltraIPCClient::UltraIPCClient(std::filesystem::path projectRoot)
    : projectRoot_(normalizeProjectRoot(std::move(projectRoot)))
#ifndef _WIN32
      , socketPath_(socketPathFor(projectRoot_))
#else
      , pipeName_(pipeNameFor(projectRoot_))
#endif
{
}
 
UltraIPCClient::Json UltraIPCClient::sendRequest(
    const Json& request,
    const std::chrono::milliseconds timeout) const {
  if (!request.is_object()) {
    return errorResponse("request_must_be_json_object");
  }
 
#ifndef _WIN32
  const int socketFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (socketFd < 0) {
    return errorResponse("socket_create_failed");
  }
 
  const long timeoutMs = std::max<long>(1L, static_cast<long>(timeout.count()));
  ::timeval tv{};
  tv.tv_sec = timeoutMs / 1000L;
  tv.tv_usec = (timeoutMs % 1000L) * 1000L;
  ::setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
 
  ::sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string socketPathString = socketPath_.string();
  if (socketPathString.size() >= sizeof(address.sun_path)) {
    ::close(socketFd);
    return errorResponse("socket_path_too_long");
  }
  std::strncpy(address.sun_path, socketPathString.c_str(), sizeof(address.sun_path) - 1U);
 
  if (::connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(socketFd);
    return errorResponse("daemon_unreachable");
  }
 
  std::string payload = request.dump();
  payload.push_back('\n');
  if (!writeAllFd(socketFd, payload)) {
    ::close(socketFd);
    return errorResponse("request_send_failed");
  }
 
  std::string responseLine;
  if (!readLineFd(socketFd, responseLine)) {
    ::close(socketFd);
    return errorResponse("response_receive_failed");
  }
  ::close(socketFd);
 
  Json response = Json::parse(responseLine, nullptr, false);
  if (response.is_discarded()) {
    return errorResponse("invalid_json_response");
  }
  return response;
#else
  const auto deadline = std::chrono::steady_clock::now() + timeout;
 
  HANDLE pipeHandle = INVALID_HANDLE_VALUE;
  while (true) {
    pipeHandle = ::CreateFileW(pipeName_.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0,
                               nullptr,
                               OPEN_EXISTING,
                               0,
                               nullptr);
    if (pipeHandle != INVALID_HANDLE_VALUE) {
      break;
    }
 
    const DWORD error = ::GetLastError();
    if (error != ERROR_PIPE_BUSY) {
      return errorResponse("daemon_unreachable");
    }
 
    if (std::chrono::steady_clock::now() >= deadline) {
      return errorResponse("daemon_unreachable");
    }
 
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    const DWORD waitMs =
        static_cast<DWORD>(std::max<std::int64_t>(1, remaining.count()));
    if (!::WaitNamedPipeW(pipeName_.c_str(), waitMs)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return errorResponse("daemon_unreachable");
      }
    }
  }
 
  // Protocol is newline-delimited JSON (byte stream).
  // PIPE_READMODE_MESSAGE causes ReadFile(1-byte buf) to return FALSE+ERROR_MORE_DATA
  // for every byte except the last — readLineHandle would always read exactly 1 byte
  // and return empty, producing response_receive_failed on every single request.
  DWORD readMode = PIPE_READMODE_BYTE;
  ::SetNamedPipeHandleState(pipeHandle, &readMode, nullptr, nullptr);
 
  std::string payload = request.dump();
  payload.push_back('\n');
  if (!writeAllHandle(pipeHandle, payload)) {
    ::CloseHandle(pipeHandle);
    return errorResponse("request_send_failed");
  }
 
  std::string responseLine;
  if (!readLineHandle(pipeHandle, responseLine)) {
    ::CloseHandle(pipeHandle);
    return errorResponse("response_receive_failed");
  }
  ::CloseHandle(pipeHandle);
 
  Json response = Json::parse(responseLine, nullptr, false);
  if (response.is_discarded()) {
    return errorResponse("invalid_json_response");
  }
  return response;
#endif
}
 
bool UltraIPCClient::isReachable(const std::chrono::milliseconds timeout) const {
  const Json response = sendRequest(Json{{"type", "wake"}}, timeout);
  return response.is_object() && response.value("status", std::string{}) == "ok";
}
 
const std::filesystem::path& UltraIPCClient::projectRoot() const noexcept {
  return projectRoot_;
}
 
std::filesystem::path UltraIPCClient::stateDirectory() const {
  return stateDirectoryFor(projectRoot_);
}
 
#ifndef _WIN32
std::filesystem::path UltraIPCClient::socketPath() const {
  return socketPath_;
}
#else
const std::wstring& UltraIPCClient::pipeName() const noexcept {
  return pipeName_;
}
#endif
 
}  // namespace ultra::ipc