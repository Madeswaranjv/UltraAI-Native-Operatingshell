// ============================================================================
// File: tests/test_state_graph.cpp
// Deterministic StateGraph tests (Phase 2 API)
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "memory/StateGraph.h"
#include "memory/StateNode.h"
#include "memory/StateEdge.h"

using namespace ultra::memory;

class StateGraphTest : public ::testing::Test {
 protected:
  StateGraph graph;

  StateNode createNode(const std::string& nodeId,
                       NodeType nodeType = NodeType::Task) {
    StateNode node;
    node.nodeId = nodeId;
    node.nodeType = nodeType;
    node.version = 1;
    node.data = nlohmann::json::object();
    return node;
  }

  StateEdge createEdge(const std::string& edgeId,
                       const std::string& sourceId,
                       const std::string& targetId,
                       EdgeType type = EdgeType::DependsOn,
                       double weight = 1.0) {
    StateEdge e;
    e.edgeId = edgeId;
    e.sourceId = sourceId;
    e.targetId = targetId;
    e.edgeType = type;
    e.weight = weight;
    return e;
  }
};

TEST_F(StateGraphTest, NodeInsertion) {
  graph.addNode(createNode("A"));
  EXPECT_EQ(graph.nodeCount(), 1);
}

TEST_F(StateGraphTest, EdgeInsertion) {
  graph.addNode(createNode("A"));
  graph.addNode(createNode("B"));
  graph.addEdge(createEdge("E1", "A", "B"));
  EXPECT_EQ(graph.edgeCount(), 1);
}

TEST_F(StateGraphTest, DuplicateNodeVersionIncrement) {
  graph.addNode(createNode("A"));
  graph.addNode(createNode("A"));

  EXPECT_EQ(graph.getNode("A").version, 2);
}

TEST_F(StateGraphTest, RemoveNodeAlsoRemovesEdges) {
  graph.addNode(createNode("A"));
  graph.addNode(createNode("B"));
  graph.addEdge(createEdge("E1", "A", "B"));

  graph.removeNode("A");

  EXPECT_EQ(graph.nodeCount(), 1);
  EXPECT_EQ(graph.edgeCount(), 0);
}

TEST_F(StateGraphTest, QueryByType) {
  graph.addNode(createNode("T1", NodeType::Task));
  graph.addNode(createNode("M1", NodeType::Module));

  auto tasks = graph.queryByType(NodeType::Task);
  EXPECT_EQ(tasks.size(), 1);
  EXPECT_EQ(tasks[0].nodeId, "T1");
}

TEST_F(StateGraphTest, DeterministicHashStable) {
  graph.addNode(createNode("A"));
  graph.addNode(createNode("B"));
  graph.addEdge(createEdge("E1", "A", "B"));

  auto h1 = graph.getDeterministicHash();
  auto h2 = graph.getDeterministicHash();

  EXPECT_EQ(h1, h2);
}

TEST_F(StateGraphTest, SnapshotCreationPhase2) {
  graph.addNode(createNode("A"));
  graph.addNode(createNode("B"));

  StateSnapshot snap = graph.snapshot(1);

  EXPECT_EQ(snap.id, 1);
  EXPECT_EQ(snap.nodes.size(), 2);
  EXPECT_EQ(snap.edges.size(), 0);
  EXPECT_FALSE(snap.graphHash.empty());
}

TEST_F(StateGraphTest, SnapshotIsolationPhase2) {
  graph.addNode(createNode("A"));

  StateSnapshot snap1 = graph.snapshot(1);

  graph.addNode(createNode("B"));

  StateSnapshot snap2 = graph.snapshot(2);

  EXPECT_EQ(snap1.nodes.size(), 1);
  EXPECT_EQ(snap2.nodes.size(), 2);
}

TEST_F(StateGraphTest, RestoreFromSnapshotPhase2) {
  graph.addNode(createNode("A"));
  graph.addNode(createNode("B"));
  graph.addEdge(createEdge("E1", "A", "B"));

  auto originalHash = graph.getDeterministicHash();
  StateSnapshot snap = graph.snapshot(1);

  StateGraph newGraph;
  newGraph.restore(snap);

  auto restoredHash = newGraph.getDeterministicHash();

  EXPECT_EQ(originalHash, restoredHash);
}

TEST_F(StateGraphTest, RestoreClearsOldStatePhase2) {
  graph.addNode(createNode("OLD"));

  StateSnapshot snap;
  snap.id = 10;
  snap.nodes.push_back(createNode("NEW"));
  snap.graphHash = "dummy";

  graph.restore(snap);

  EXPECT_EQ(graph.nodeCount(), 1);
  EXPECT_EQ(graph.getNode("NEW").nodeId, "NEW");
  EXPECT_EQ(graph.getNode("OLD").nodeId, "");
}