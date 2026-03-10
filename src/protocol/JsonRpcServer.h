#pragma once

#include <string>

namespace ultra::protocol {

/// JSON-RPC 2.0 communication layer for Model Context Protocol / Language Servers.
class JsonRpcServer {
 public:
  JsonRpcServer() = default;

  /// Start listening on standard input / standard output for JSON RPC payloads.
  /// This implements the agent-mode lifecycle loop.
  void startStdio();

 private:
  void processMessage(const std::string& payload);
};

}  // namespace ultra::protocol
