#pragma once

#include <string>
#include <cstdint>

namespace ultra::service {

/// A lightweight HTTP server for servicing Ultra architecture queries.
class HttpServer {
 public:
  HttpServer() = default;
  ~HttpServer() = default;

  /// Start the HTTP server on the specified port. Blocking call.
  void start(std::uint16_t port);
};

}  // namespace ultra::service
