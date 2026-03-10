#include <gtest/gtest.h>

#include "runtime/ipc_server.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

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

TEST_F(IpcRuntimeTest, HeartbeatProducesHealthyDaemonInspection) {
  ultra::runtime::IpcServer server(root_);
  std::string error;
  ASSERT_TRUE(server.start(error)) << error;

  ultra::runtime::DaemonHeartbeat heartbeat;
  heartbeat.runtimeActive = true;
  heartbeat.indexedFiles = 12U;
  heartbeat.symbolsIndexed = 48U;
  heartbeat.graphNodes = 9U;
  heartbeat.graphEdges = 7U;
  ASSERT_TRUE(server.writeHeartbeat(heartbeat, error)) << error;

  const ultra::runtime::DaemonHealthReport report =
      ultra::runtime::IpcServer::inspectDaemon(root_);
  EXPECT_EQ(report.state, ultra::runtime::DaemonHealthState::Healthy);
  EXPECT_EQ(report.pid, ultra::runtime::IpcServer::currentProcessId());
  EXPECT_EQ(report.indexedFiles, 12U);
  EXPECT_EQ(report.symbolsIndexed, 48U);
  EXPECT_EQ(report.graphNodes, 9U);
  EXPECT_EQ(report.graphEdges, 7U);

  server.stop();
}

TEST_F(IpcRuntimeTest, InvalidPidIsReportedAsDeadDaemon) {
  const fs::path ipcDir = root_ / ".ultra" / "runtime" / "ipc";
  fs::create_directories(ipcDir);

  {
    std::ofstream lock(ipcDir / "daemon.lock", std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(lock.is_open());
    lock << R"({"pid":99999999,"status":"running"})";
  }

  const ultra::runtime::DaemonHealthReport report =
      ultra::runtime::IpcServer::inspectDaemon(root_);
  EXPECT_EQ(report.state, ultra::runtime::DaemonHealthState::Dead);
}
