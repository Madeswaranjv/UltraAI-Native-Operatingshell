#include <gtest/gtest.h>
#include "memory/StateGraph.h"
//E:\Projects\Ultra\tests\test_snapshot_restore.cpp
using namespace ultra::memory;

TEST(Snapshot, RestoreRecreatesExactHash) {
    StateGraph g;

    StateNode n1;
    n1.nodeId = "A";
    n1.nodeType = NodeType::Task;
    g.addNode(n1);

    StateNode n2;
    n2.nodeId = "B";
    n2.nodeType = NodeType::Task;
    g.addNode(n2);

    StateEdge e;
    e.edgeId = "E1";
    e.sourceId = "A";
    e.targetId = "B";
    e.edgeType = EdgeType::DependsOn;
    g.addEdge(e);

    auto originalHash = g.getDeterministicHash();

    auto snap = g.snapshot(1);

    StateGraph g2;
    g2.restore(snap);

    auto restoredHash = g2.getDeterministicHash();

    EXPECT_EQ(originalHash, restoredHash);
}