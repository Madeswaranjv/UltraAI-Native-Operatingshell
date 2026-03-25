#include "HttpTransport.h"

// WinHTTP is always available on Windows — no vcpkg required.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <codecvt>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <vector>

// Link: target_link_libraries(ultra_core PRIVATE winhttp)  ← add to CMakeLists.txt
// #pragma comment(lib, "winhttp.lib") also works but CMake is cleaner.

namespace ultra::ai::http {

namespace {

// WinHTTP only accepts wide strings for hostnames, paths, headers.
std::wstring toWide(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int len = MultiByteToWideChar(CP_UTF8, 0,
                                      utf8.data(),
                                      static_cast<int>(utf8.size()),
                                      nullptr, 0);
  if (len <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0,
                      utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), len);
  return wide;
}

// Parse "https://api.anthropic.com/v1/messages" into host + path + port + https flag.
struct ParsedUrl {
  std::wstring scheme;   // L"https" or L"http"
  std::wstring host;
  std::wstring path;     // "/v1/messages"
  INTERNET_PORT port{INTERNET_DEFAULT_HTTPS_PORT};
  bool useHttps{true};
};

bool parseUrl(const std::string& url, ParsedUrl& out, std::string& error) {
  const std::wstring wurl = toWide(url);

  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);

  // Set non-zero lengths to instruct WinHTTP to fill them.
  wchar_t scheme[16]{};
  wchar_t host[512]{};
  wchar_t path[4096]{};
  uc.lpszScheme = scheme;   uc.dwSchemeLength   = static_cast<DWORD>(std::size(scheme));
  uc.lpszHostName = host;   uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
  uc.lpszUrlPath  = path;   uc.dwUrlPathLength  = static_cast<DWORD>(std::size(path));

  if (!WinHttpCrackUrl(wurl.c_str(), 0U, 0, &uc)) {
    error = "Failed to parse URL: " + url;
    return false;
  }

  out.scheme  = scheme;
  out.host    = host;
  out.path    = path;
  out.port    = uc.nPort;
  out.useHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);

  if (out.path.empty()) {
    out.path = L"/";
  }

  return true;
}

// RAII handle wrapper so we never leak WinHTTP handles.
struct WinHttpHandle {
  HINTERNET h{nullptr};
  explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
  ~WinHttpHandle() { if (h) { WinHttpCloseHandle(h); } }
  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;
  operator bool() const { return h != nullptr; }
};

}  // namespace

HttpResponse httpPost(const std::string& url,
                      const std::string& body,
                      const std::unordered_map<std::string, std::string>& headers,
                      std::uint32_t timeoutMs) {
  HttpResponse result;

  if (timeoutMs == 0U) {
    timeoutMs = 30'000U;
  }

  // 1. Parse URL
  ParsedUrl parsed;
  if (!parseUrl(url, parsed, result.errorMessage)) {
    return result;
  }

  // 2. Open session
  WinHttpHandle session(WinHttpOpen(
      L"UltraInfinity/1.0",
      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0));
  if (!session) {
    result.errorMessage = "WinHttpOpen failed: " + std::to_string(GetLastError());
    return result;
  }

  // Set timeouts: resolve, connect, send, receive (all in ms)
  WinHttpSetTimeouts(session.h,
                     static_cast<int>(timeoutMs),   // resolve
                     static_cast<int>(timeoutMs),   // connect
                     static_cast<int>(timeoutMs),   // send
                     static_cast<int>(timeoutMs));  // receive

  // 3. Connect
  WinHttpHandle connect(WinHttpConnect(session.h,
                                       parsed.host.c_str(),
                                       parsed.port,
                                       0));
  if (!connect) {
    result.errorMessage = "WinHttpConnect failed: " + std::to_string(GetLastError());
    return result;
  }

  // 4. Open request
  const DWORD flags = parsed.useHttps ? WINHTTP_FLAG_SECURE : 0U;
  WinHttpHandle request(WinHttpOpenRequest(connect.h,
                                           L"POST",
                                           parsed.path.c_str(),
                                           nullptr,                    // HTTP/1.1
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           flags));
  if (!request) {
    result.errorMessage = "WinHttpOpenRequest failed: " + std::to_string(GetLastError());
    return result;
  }

  // 5. Add headers
  // Always set Content-Type first.
  const std::wstring contentType = L"Content-Type: application/json\r\n";
  WinHttpAddRequestHeaders(request.h,
                           contentType.c_str(),
                           static_cast<DWORD>(-1L),
                           WINHTTP_ADDREQ_FLAG_ADD);

  for (const auto& [name, value] : headers) {
    const std::wstring header = toWide(name + ": " + value + "\r\n");
    WinHttpAddRequestHeaders(request.h,
                             header.c_str(),
                             static_cast<DWORD>(-1L),
                             WINHTTP_ADDREQ_FLAG_ADD);
  }

  // 6. Send request with body
  const BOOL sent = WinHttpSendRequest(
      request.h,
      WINHTTP_NO_ADDITIONAL_HEADERS, 0,
      const_cast<char*>(body.data()),
      static_cast<DWORD>(body.size()),
      static_cast<DWORD>(body.size()),
      0);

  if (!sent) {
    result.errorMessage = "WinHttpSendRequest failed: " + std::to_string(GetLastError());
    return result;
  }

  // 7. Receive response
  if (!WinHttpReceiveResponse(request.h, nullptr)) {
    result.errorMessage = "WinHttpReceiveResponse failed: " + std::to_string(GetLastError());
    return result;
  }

  // 8. Read status code
  DWORD statusCode = 0U;
  DWORD statusCodeSize = sizeof(statusCode);
  WinHttpQueryHeaders(request.h,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX,
                      &statusCode,
                      &statusCodeSize,
                      WINHTTP_NO_HEADER_INDEX);
  result.statusCode = static_cast<int>(statusCode);

  // 9. Read body
  std::string responseBody;
  DWORD available = 0U;
  while (WinHttpQueryDataAvailable(request.h, &available) && available > 0U) {
    std::vector<char> buffer(available + 1U, '\0');
    DWORD read = 0U;
    if (!WinHttpReadData(request.h, buffer.data(), available, &read)) {
      result.errorMessage = "WinHttpReadData failed: " + std::to_string(GetLastError());
      return result;
    }
    responseBody.append(buffer.data(), read);
  }

  result.body = std::move(responseBody);
  result.ok   = (result.statusCode >= 200 && result.statusCode < 300);

  if (!result.ok && result.errorMessage.empty()) {
    result.errorMessage = "HTTP " + std::to_string(result.statusCode);
  }

  return result;
}

}  // namespace ultra::ai::http