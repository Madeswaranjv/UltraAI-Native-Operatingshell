#include <gtest/gtest.h>
// E:\Projects\Ultra\tests\test_ipc_runtime.cpp

#include "ultra/ipc/ultra_ipc_client.h"
#include "ultra/ipc/ultra_ipc_server.h"
#include "ultra/runtime/ultra_daemon.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// Test fixture — creates and tears down a fresh temp directory per test
// ---------------------------------------------------------------------------

namespace {

class IpcRuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / "ultra_test" / "ipc_runtime";
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_, ec);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  fs::path root_;
};

}  // namespace

// ---------------------------------------------------------------------------
// UltraIPCServer — lifecycle tests
// ---------------------------------------------------------------------------

TEST_F(IpcRuntimeTest, ServerStartsSuccessfully) {
  ultra::ipc::UltraIPCServer server(root_);
  EXPECT_FALSE(server.isRunning());
  ASSERT_TRUE(server.start());
  EXPECT_TRUE(server.isRunning());
  server.stop();
  EXPECT_FALSE(server.isRunning());
}

TEST_F(IpcRuntimeTest, ServerStartIsIdempotent) {
  ultra::ipc::UltraIPCServer server(root_);
  ASSERT_TRUE(server.start());
  EXPECT_TRUE(server.start());  // second call must still return true
  EXPECT_TRUE(server.isRunning());
  server.stop();
}

TEST_F(IpcRuntimeTest, ServerCreatesStateDirectory) {
  ultra::ipc::UltraIPCServer server(root_);
  ASSERT_TRUE(server.start());
  EXPECT_TRUE(fs::exists(server.stateDirectory()));
  server.stop();
}

TEST_F(IpcRuntimeTest, ServerProjectRootMatchesConstructorArg) {
  ultra::ipc::UltraIPCServer server(root_);
  // weakly_canonical may adjust separators; compare generic strings
  EXPECT_EQ(server.projectRoot().generic_string(), root_.generic_string());
}

// ---------------------------------------------------------------------------
// UltraIPCClient — basic reachability without a live server
// ---------------------------------------------------------------------------

TEST_F(IpcRuntimeTest, ClientIsNotReachableWhenServerIsDown) {
  ultra::ipc::UltraIPCClient client(root_);
  EXPECT_FALSE(client.isReachable(std::chrono::milliseconds(200)));
}

TEST_F(IpcRuntimeTest, ClientSendRequestReturnsErrorWhenServerIsDown) {
  ultra::ipc::UltraIPCClient client(root_);
  const Json response = client.sendRequest(
      Json{{"type", "wake"}}, std::chrono::milliseconds(200));
  ASSERT_TRUE(response.is_object());
  EXPECT_EQ(response.value("status", std::string{}), "error");
}

TEST_F(IpcRuntimeTest, ClientProjectRootMatchesConstructorArg) {
  ultra::ipc::UltraIPCClient client(root_);
  EXPECT_EQ(client.projectRoot().generic_string(), root_.generic_string());
}

// ---------------------------------------------------------------------------
// UltraIPCServer + UltraIPCClient — round-trip communication
// ---------------------------------------------------------------------------

TEST_F(IpcRuntimeTest, WakeRequestReturnsOkStatus) {
  ultra::ipc::UltraIPCServer server(root_);
  ASSERT_TRUE(server.start());

  // Serve exactly one request from a background thread.
  std::thread serverThread([&server]() {
    server.processNextRequest(
        [](const Json& request) -> Json {
          const std::string type = request.value("type", std::string{});
          if (type == "wake") {
            return Json{{"status", "ok"}, {"result", "awake"}};
          }
          return Json{{"status", "error"}, {"error", "unsupported_request_type"}};
        },
        std::chrono::milliseconds(3000));
  });

  // Give the server a moment to enter poll/accept.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ultra::ipc::UltraIPCClient client(root_);
  const Json response = client.sendRequest(
      Json{{"type", "wake"}}, std::chrono::milliseconds(2000));

  serverThread.join();
  server.stop();

  ASSERT_TRUE(response.is_object());
  EXPECT_EQ(response.value("status", std::string{}), "ok");
  EXPECT_EQ(response.value("result", std::string{}), "awake");
}

TEST_F(IpcRuntimeTest, UnknownRequestTypeReturnsError) {
  ultra::ipc::UltraIPCServer server(root_);
  ASSERT_TRUE(server.start());

  std::thread serverThread([&server]() {
    server.processNextRequest(
        [](const Json& request) -> Json {
          const std::string type = request.value("type", std::string{});
          if (type != "wake" && type != "shutdown") {
            return Json{{"status", "error"}, {"error", "unsupported_request_type"}};
          }
          return Json{{"status", "ok"}, {"result", "ok"}};
        },
        std::chrono::milliseconds(3000));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ultra::ipc::UltraIPCClient client(root_);
  const Json response = client.sendRequest(
      Json{{"type", "nonexistent_command"}}, std::chrono::milliseconds(2000));

  serverThread.join();
  server.stop();

  ASSERT_TRUE(response.is_object());
  EXPECT_EQ(response.value("status", std::string{}), "error");
  EXPECT_EQ(response.value("error", std::string{}), "unsupported_request_type");
}

TEST_F(IpcRuntimeTest, ClientIsReachableWhenServerIsRunning) {
  ultra::ipc::UltraIPCServer server(root_);
  ASSERT_TRUE(server.start());

  std::thread serverThread([&server]() {
    server.processNextRequest(
        [](const Json&) -> Json {
          return Json{{"status", "ok"}, {"result", "awake"}};
        },
        std::chrono::milliseconds(3000));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ultra::ipc::UltraIPCClient client(root_);
  const bool reachable = client.isReachable(std::chrono::milliseconds(2000));

  serverThread.join();
  server.stop();

  EXPECT_TRUE(reachable);
}

TEST_F(IpcRuntimeTest, MultipleSequentialRequestsAreAllHandled) {
  ultra::ipc::UltraIPCServer server(root_);
  ASSERT_TRUE(server.start());

  constexpr int kRequests = 3;

  // Serve kRequests requests sequentially in a background thread.
  std::thread serverThread([&server]() {
    for (int i = 0; i < kRequests; ++i) {
      server.processNextRequest(
          [](const Json& request) -> Json {
            return Json{{"status", "ok"},
                        {"result", request.value("type", std::string{})}};
          },
          std::chrono::milliseconds(3000));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ultra::ipc::UltraIPCClient client(root_);
  for (int i = 0; i < kRequests; ++i) {
    const Json response = client.sendRequest(
        Json{{"type", "wake"}}, std::chrono::milliseconds(2000));
    ASSERT_TRUE(response.is_object()) << "Request " << i << " got no response";
    EXPECT_EQ(response.value("status", std::string{}), "ok") << "Request " << i;
  }

  serverThread.join();
  server.stop();
}

// ---------------------------------------------------------------------------
// UltraDaemon — static utility methods (no live process required)
// ---------------------------------------------------------------------------

TEST_F(IpcRuntimeTest, IsDaemonAliveReturnsFalseWithNoPidFile) {
  EXPECT_FALSE(ultra::runtime::UltraDaemon::isDaemonAlive(root_));
}

TEST_F(IpcRuntimeTest, IsDaemonAliveReturnsFalseWithInvalidPid) {
  const fs::path stateDir = ultra::runtime::UltraDaemon::daemonStateDirectory(root_);
  std::error_code ec;
  fs::create_directories(stateDir, ec);

  // Write a PID that is astronomically unlikely to be a live process.
  std::ofstream pidFile(ultra::runtime::UltraDaemon::daemonPidFile(root_),
                        std::ios::trunc);
  ASSERT_TRUE(pidFile.is_open());
  pidFile << 999999999LL << '\n';
  pidFile.close();

  EXPECT_FALSE(ultra::runtime::UltraDaemon::isDaemonAlive(root_));
}

TEST_F(IpcRuntimeTest, DaemonStateDirectoryIsInsideProjectRoot) {
  const fs::path stateDir =
      ultra::runtime::UltraDaemon::daemonStateDirectory(root_);
  // The state directory must be a child of the project root.
  const std::string stateDirStr = stateDir.generic_string();
  const std::string rootStr = root_.generic_string();
  EXPECT_EQ(stateDirStr.substr(0, rootStr.size()), rootStr);
}

TEST_F(IpcRuntimeTest, DaemonPidFileIsInsideStateDirectory) {
  const fs::path stateDir =
      ultra::runtime::UltraDaemon::daemonStateDirectory(root_);
  const fs::path pidFile =
      ultra::runtime::UltraDaemon::daemonPidFile(root_);
  // PID file must reside inside the state directory.
  const std::string pidStr = pidFile.generic_string();
  const std::string stateStr = stateDir.generic_string();
  EXPECT_EQ(pidStr.substr(0, stateStr.size()), stateStr);
}

// ---------------------------------------------------------------------------
// UltraDaemon — instance construction
// ---------------------------------------------------------------------------

TEST_F(IpcRuntimeTest, DaemonProjectRootMatchesConstructorArg) {
  ultra::runtime::UltraDaemon daemon(root_);
  EXPECT_EQ(daemon.projectRoot().generic_string(), root_.generic_string());
}

TEST_F(IpcRuntimeTest, DaemonIsNotRunningBeforeRun) {
  ultra::runtime::UltraDaemon daemon(root_);
  EXPECT_FALSE(daemon.isRunning());
}
