#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

#include "graph/DependencyGraph.h"

using namespace ultra::graph;

class DependencyGraphTest : public ::testing::Test {
protected:
    DependencyGraph graph;

    // Helper: find index of node in topo result
    static size_t positionOf(const std::vector<std::string>& result,
                             const std::string& node) {
        auto it = std::find(result.begin(), result.end(), node);
        EXPECT_TRUE(it != result.end());
        return static_cast<size_t>(std::distance(result.begin(), it));
    }

    // Helper: assert topological ordering constraint
    static void expectBefore(const std::vector<std::string>& result,
                             const std::string& before,
                             const std::string& after) {
        EXPECT_LT(positionOf(result, before),
                  positionOf(result, after));
    }
};

TEST_F(DependencyGraphTest, EmptyGraph) {
    EXPECT_EQ(graph.nodeCount(), 0);
    EXPECT_EQ(graph.edgeCount(), 0);
    EXPECT_FALSE(graph.hasCycle());
    EXPECT_TRUE(graph.topologicalSort().empty());
}

TEST_F(DependencyGraphTest, AddNodesAndDuplicates) {
    graph.addNode("a");
    graph.addNode("b");
    graph.addNode("a");
    EXPECT_EQ(graph.nodeCount(), 2);
}

TEST_F(DependencyGraphTest, AddEdgeCreatesNodes) {
    graph.addEdge("a", "b");
    EXPECT_EQ(graph.nodeCount(), 2);
    EXPECT_EQ(graph.edgeCount(), 1);
}

TEST_F(DependencyGraphTest, DuplicateEdgeIgnored) {
    graph.addEdge("a", "b");
    graph.addEdge("a", "b");
    EXPECT_EQ(graph.edgeCount(), 1);
}

TEST_F(DependencyGraphTest, LinearDependencyNoCycle) {
    graph.addEdge("a", "b");
    graph.addEdge("b", "c");

    EXPECT_FALSE(graph.hasCycle());

    auto result = graph.topologicalSort();
    ASSERT_EQ(result.size(), 3);

    expectBefore(result, "a", "b");
    expectBefore(result, "b", "c");
}

TEST_F(DependencyGraphTest, SelfCycleDetected) {
    graph.addEdge("a", "a");
    EXPECT_TRUE(graph.hasCycle());
    EXPECT_TRUE(graph.topologicalSort().empty());
}

TEST_F(DependencyGraphTest, SimpleCycleDetected) {
    graph.addEdge("a", "b");
    graph.addEdge("b", "c");
    graph.addEdge("c", "a");

    EXPECT_TRUE(graph.hasCycle());
    EXPECT_TRUE(graph.topologicalSort().empty());
}

TEST_F(DependencyGraphTest, DiamondGraphOrdering) {
    graph.addEdge("top", "left");
    graph.addEdge("top", "right");
    graph.addEdge("left", "bottom");
    graph.addEdge("right", "bottom");

    EXPECT_FALSE(graph.hasCycle());

    auto result = graph.topologicalSort();
    ASSERT_EQ(result.size(), 4);

    expectBefore(result, "top", "left");
    expectBefore(result, "top", "right");
    expectBefore(result, "left", "bottom");
    expectBefore(result, "right", "bottom");
}

TEST_F(DependencyGraphTest, DisconnectedComponents) {
    graph.addEdge("a", "b");
    graph.addEdge("c", "d");

    EXPECT_FALSE(graph.hasCycle());

    auto result = graph.topologicalSort();
    EXPECT_EQ(result.size(), 4);
}

TEST_F(DependencyGraphTest, LongLinearChain) {
    for (int i = 0; i < 10; ++i) {
        graph.addEdge("n" + std::to_string(i),
                      "n" + std::to_string(i + 1));
    }

    EXPECT_EQ(graph.nodeCount(), 11);
    EXPECT_FALSE(graph.hasCycle());

    auto result = graph.topologicalSort();
    ASSERT_EQ(result.size(), 11);

    for (int i = 0; i < 10; ++i) {
        expectBefore(result,
                     "n" + std::to_string(i),
                     "n" + std::to_string(i + 1));
    }
}

TEST_F(DependencyGraphTest, ComplexGraphOrdering) {
    graph.addEdge("a", "b");
    graph.addEdge("a", "c");
    graph.addEdge("b", "d");
    graph.addEdge("c", "d");
    graph.addEdge("d", "e");

    EXPECT_FALSE(graph.hasCycle());

    auto result = graph.topologicalSort();
    ASSERT_EQ(result.size(), 5);

    expectBefore(result, "a", "b");
    expectBefore(result, "a", "c");
    expectBefore(result, "b", "d");
    expectBefore(result, "c", "d");
    expectBefore(result, "d", "e");
}

TEST_F(DependencyGraphTest, SpecialCharacterNodes) {
    graph.addNode("file-name.cpp");
    graph.addNode("path/to/file");
    EXPECT_EQ(graph.nodeCount(), 2);
}

TEST_F(DependencyGraphTest, LargeFanOut) {
    for (int i = 0; i < 20; ++i) {
        graph.addEdge("root", "child_" + std::to_string(i));
    }

    EXPECT_EQ(graph.nodeCount(), 21);
    EXPECT_EQ(graph.edgeCount(), 20);
    EXPECT_FALSE(graph.hasCycle());
}

TEST_F(DependencyGraphTest, MixedConnectedAndDisconnected) {
    graph.addEdge("a", "b");
    graph.addEdge("b", "c");
    graph.addNode("isolated");

    EXPECT_EQ(graph.nodeCount(), 4);
    EXPECT_FALSE(graph.hasCycle());

    auto result = graph.topologicalSort();
    EXPECT_EQ(result.size(), 4);
}

TEST_F(DependencyGraphTest, RemoveNodeDropsIncomingAndOutgoingEdges) {
    graph.addEdge("a", "b");
    graph.addEdge("b", "c");
    graph.addEdge("d", "b");

    graph.removeNode("b");

    EXPECT_EQ(graph.nodeCount(), 3);
    EXPECT_EQ(graph.edgeCount(), 0);
    EXPECT_TRUE(graph.getDependencies("a").empty());
    EXPECT_TRUE(graph.getDependencies("d").empty());
}

TEST_F(DependencyGraphTest, UpdateNodeReplacesOutgoingDependencies) {
    graph.addEdge("src", "old_a");
    graph.addEdge("src", "old_b");

    graph.updateNode("src", {"new_a", "new_b", "new_a"});

    const auto deps = graph.getDependencies("src");
    ASSERT_EQ(deps.size(), 2);
    EXPECT_EQ(deps[0], "new_a");
    EXPECT_EQ(deps[1], "new_b");
    EXPECT_EQ(graph.edgeCount(), 2);
}
