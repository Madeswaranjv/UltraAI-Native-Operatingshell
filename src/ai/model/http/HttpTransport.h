#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ultra::ai::http {

struct HttpResponse {
  bool ok{false};
  int statusCode{0};
  std::string body;
  std::string errorMessage;
};

// Synchronous HTTP POST using WinHTTP (Windows) or a POSIX socket (future).
// url        — full URL, e.g. "https://api.anthropic.com/v1/messages"
// body       — JSON string to send as request body
// headers    — additional HTTP headers; Content-Type is always set to application/json
// timeoutMs  — connect + receive timeout in milliseconds; 0 means 30 000 ms
HttpResponse httpPost(const std::string& url,
                      const std::string& body,
                      const std::unordered_map<std::string, std::string>& headers,
                      std::uint32_t timeoutMs = 0U);

}  // namespace ultra::ai::http