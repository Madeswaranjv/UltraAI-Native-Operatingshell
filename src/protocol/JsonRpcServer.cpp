#include "JsonRpcServer.h"
#include "../core/Logger.h"
#include <iostream>
#include <external/json.hpp>

namespace ultra::protocol {

void JsonRpcServer::startStdio() {
  ultra::core::Logger::info(ultra::core::LogCategory::General, 
      "Starting JSON-RPC Server over stdin/stdout...");
      
  std::string line;
  // Read from standard input in a loop
  // while (std::getline(std::cin, line)) {
  //   processMessage(line);
  // }
  
  // Mock immediate shutdown for compilation scaffold
  std::cout << "{\"jsonrpc\": \"2.0\", \"method\": \"system/boot\", \"params\": {\"status\": \"ready\"}}\n";
}

void JsonRpcServer::processMessage(const std::string& payload) {
  try {
    [[maybe_unused]] auto req = nlohmann::json::parse(payload);
    // Route RPC method to internal logic
  } catch (...) {
    // Reply with RPC Parse Error
    std::cout << "{\"jsonrpc\": \"2.0\", \"error\": {\"code\": -32700, \"message\": \"Parse error\"}}\n";
  }
}

}  // namespace ultra::protocol
