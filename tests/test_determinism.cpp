#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <unordered_map>
//E:\Projects\Ultra\tests\test_determinism.cpp
#include "memory/StateGraph.h"
#include "memory/StateNode.h"
#include "memory/StateEdge.h"
#include "context/ContextSnapshot.h"

using namespace ultra::memory;
using namespace ultra::context;

static StateNode makeNode(const std::string& id) {
  StateNode n;
  n.nodeId = id;
  n.nodeType = NodeType::Task;
  n.data = nlohmann::json::object();
  n.version = 1;
  return n;
}

static StateEdge makeEdge(const std::string& id, const std::string& src, const std::string& dst) {
  StateEdge e;
  e.edgeId = id;
  e.sourceId = src;
  e.targetId = dst;
  e.edgeType = EdgeType::DependsOn;
  e.weight = 1.0;
  return e;
}

TEST(Determinism, GraphHashStability) {
  StateGraph g1;
  g1.addNode(makeNode("a"));
  g1.addNode(makeNode("b"));
  g1.addEdge(makeEdge("e1", "a", "b"));
  std::string h1 = g1.getDeterministicHash();

  StateGraph g2;
  // Insert nodes in different order
  g2.addNode(makeNode("b"));
  g2.addNode(makeNode("a"));
  g2.addEdge(makeEdge("e1", "a", "b"));
  std::string h2 = g2.getDeterministicHash();

  EXPECT_EQ(h1, h2);
}

TEST(Determinism, JsonSaveStableAcrossInsertionOrder) {
  std::filesystem::path tmp = std::filesystem::temp_directory_path();
  auto f1 = tmp / "ultra_snapshot_test_1.txt";
  auto f2 = tmp / "ultra_snapshot_test_2.txt";

  std::unordered_map<std::string, std::string> s1;
  s1["/path/to/a.cpp"] = "hash_a";
  s1["/path/to/b.cpp"] = "hash_b";
  saveSnapshot(f1, s1);

  std::unordered_map<std::string, std::string> s2;
  s2["/path/to/b.cpp"] = "hash_b";
  s2["/path/to/a.cpp"] = "hash_a";
  saveSnapshot(f2, s2);

  // Read both files and compare bytes
  std::ifstream in1(f1, std::ios::binary);
  std::ifstream in2(f2, std::ios::binary);
  ASSERT_TRUE(in1.is_open());
  ASSERT_TRUE(in2.is_open());

  std::string b1((std::istreambuf_iterator<char>(in1)), std::istreambuf_iterator<char>());
  std::string b2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());

  EXPECT_EQ(b1, b2);

  // Cleanup
  std::error_code ec; std::filesystem::remove(f1, ec); std::filesystem::remove(f2, ec);
}
