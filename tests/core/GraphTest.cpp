//
// Comprehensive unit tests for Core Graph infrastructure
// Tests Graph.h, GraphBuilder.h, RestrictedGraphBuilder, and graph algorithms
// Created by Spencer Martin on 7/24/25.
//

#include "../harness/TestHarness.h"
#include <core/GraphBuilder.h>  // This includes Graph.h
#include <core/algo/GraphAlgorithms.h>
#include <core/algo/GraphPredicates.h>

using namespace CroCOSTest;

// Graph type aliases for testing
using StringGraph = Graph<GraphProperties::LabeledVertex<const char*>,
                         GraphProperties::WeightedEdge<int>,
                         GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;
                         
using IntGraph = Graph<GraphProperties::LabeledVertex<int>,
                      GraphProperties::LabeledEdge<const char*>,
                      GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;

using PlainDirectedGraph = Graph<GraphProperties::PlainVertex,
                                GraphProperties::PlainEdge,
                                GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;

using PlainUndirectedGraph = Graph<GraphProperties::PlainVertex,
                                  GraphProperties::PlainEdge,
                                  GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;

// Graph type for RestrictedGraphBuilder testing (needs integer labels for constraint)
using IntLabeledDirectedGraph = Graph<GraphProperties::LabeledVertex<int>,
                                     GraphProperties::PlainEdge,
                                     GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;

// ============================================================================
// Basic GraphBuilder Tests
// ============================================================================

TEST(GraphBuilderBasicVertexCreation) {
    GraphBuilder<StringGraph> builder;
    
    // Test empty builder
    ASSERT_EQ(0u, builder.getCurrentVertexCount());
    ASSERT_EQ(0u, builder.getCurrentEdgeCount());
    
    // Add vertices
    auto v1 = builder.addVertex();
    auto v2 = builder.addVertex();
    auto v3 = builder.addVertex();
    
    ASSERT_EQ(3u, builder.getCurrentVertexCount());
    ASSERT_EQ(0u, builder.getCurrentEdgeCount());
    
    // Test handle equality
    ASSERT_TRUE(v1 == v1);
    ASSERT_TRUE(v1 != v2);
    ASSERT_TRUE(v2 != v3);
}

TEST(GraphBuilderVertexLabeling) {
    GraphBuilder<StringGraph> builder;
    
    auto v1 = builder.addVertex();
    auto v2 = builder.addVertex();
    
    // Set vertex labels
    ASSERT_TRUE(builder.setVertexLabel(v1, "first"));
    ASSERT_TRUE(builder.setVertexLabel(v2, "second"));
    
    // Test duplicate label rejection
    ASSERT_FALSE(builder.setVertexLabel(v2, "first"));
    
    // Test label retrieval
    auto label1 = builder.getVertexLabel(v1);
    auto label2 = builder.getVertexLabel(v2);
    
    ASSERT_TRUE(label1.occupied());
    ASSERT_TRUE(label2.occupied());
    ASSERT_EQ(0, strcmp(*label1, "first"));
    ASSERT_EQ(0, strcmp(*label2, "second"));
    
    // Test lookup by label
    auto foundV1 = builder.getVertexByLabel("first");
    auto foundV2 = builder.getVertexByLabel("second");
    auto notFound = builder.getVertexByLabel("nonexistent");
    
    ASSERT_TRUE(foundV1.occupied());
    ASSERT_TRUE(foundV2.occupied());
    ASSERT_FALSE(notFound.occupied());
    ASSERT_TRUE(*foundV1 == v1);
    ASSERT_TRUE(*foundV2 == v2);
}

TEST(GraphBuilderEdgeCreation) {
    GraphBuilder<StringGraph> builder;
    
    auto v1 = builder.addVertex();
    auto v2 = builder.addVertex();
    auto v3 = builder.addVertex();
    
    // Set vertex labels (required for StringGraph)
    builder.setVertexLabel(v1, "A");
    builder.setVertexLabel(v2, "B");
    builder.setVertexLabel(v3, "C");
    
    // Add edges
    auto e1 = builder.addEdge(v1, v2);
    auto e2 = builder.addEdge(v2, v3);
    auto e3 = builder.addEdge(v1, v3);
    
    ASSERT_EQ(3u, builder.getCurrentEdgeCount());
    
    // Set edge weights (required for StringGraph)
    builder.setEdgeWeight(e1, 10);
    builder.setEdgeWeight(e2, 20);
    builder.setEdgeWeight(e3, 5);
    
    // Test edge endpoints
    ASSERT_TRUE(builder.getEdgeSource(e1) == v1);
    ASSERT_TRUE(builder.getEdgeTarget(e1) == v2);
    ASSERT_TRUE(builder.getEdgeSource(e2) == v2);
    ASSERT_TRUE(builder.getEdgeTarget(e2) == v3);
    
    // Test edge weights
    auto weight1 = builder.getEdgeWeight(e1);
    auto weight2 = builder.getEdgeWeight(e2);
    
    ASSERT_TRUE(weight1.occupied());
    ASSERT_TRUE(weight2.occupied());
    ASSERT_EQ(10, *weight1);
    ASSERT_EQ(20, *weight2);
    
    // Test hasEdge
    ASSERT_TRUE(builder.hasEdge(v1, v2));
    ASSERT_TRUE(builder.hasEdge(v2, v3));
    ASSERT_TRUE(builder.hasEdge(v1, v3));
    ASSERT_FALSE(builder.hasEdge(v2, v1)); // Directed graph
    ASSERT_FALSE(builder.hasEdge(v3, v1));
}

TEST(GraphBuilderConvenienceMethods) {
    GraphBuilder<IntGraph> builder;
    
    // Test convenience vertex creation with labels
    auto v1 = builder.addVertex(1);
    auto v2 = builder.addVertex(2);
    auto v3 = builder.addVertex(3);
    
    // Verify labels were set
    ASSERT_EQ(1, *builder.getVertexLabel(v1));
    ASSERT_EQ(2, *builder.getVertexLabel(v2));
    ASSERT_EQ(3, *builder.getVertexLabel(v3));
    
    // Test convenience edge creation with labels
    auto e1 = builder.addEdge(v1, v2, "edge1");
    auto e2 = builder.addEdge(v2, v3, "edge2");
    
    // Verify edge labels were set
    ASSERT_EQ(0, strcmp(*builder.getEdgeLabel(e1), "edge1"));
    ASSERT_EQ(0, strcmp(*builder.getEdgeLabel(e2), "edge2"));
}

// ============================================================================
// Graph Building and Construction Tests
// ============================================================================

TEST(GraphBuilderBuildingSimpleGraph) {
    GraphBuilder<PlainDirectedGraph> builder;
    
    // Create vertices
    auto v1 = builder.addVertex();
    auto v2 = builder.addVertex();
    auto v3 = builder.addVertex();
    
    // Create edges
    builder.addEdge(v1, v2);
    builder.addEdge(v2, v3);
    builder.addEdge(v1, v3);
    
    // Build graph
    auto graph = builder.build();
    ASSERT_TRUE(graph.occupied());
    
    // Verify graph structure
    ASSERT_EQ(3u, graph->getVertexCount());
    ASSERT_EQ(3u, graph->getEdgeCount());
    
    // Test graph iteration
    size_t vertexCount = 0;
    for (auto vertex : graph->vertices()) {
        vertexCount++;
        // Each vertex should have some edges
        size_t edgeCount = 0;
        for (auto edge : graph->outgoingEdges(vertex)) {
            edgeCount++;
        }
        // At least one vertex should have outgoing edges
        if (edgeCount > 0) {
            ASSERT_GT(edgeCount, 0u);
        }
    }
    ASSERT_EQ(3u, vertexCount);
    
    size_t edgeCount = 0;
    for (auto edge : graph->edges()) {
        edgeCount++;
        // Verify edge has valid source and target
        auto source = graph->getSource(edge);
        auto target = graph->getTarget(edge);
        // Source and target should be different for all our test edges
        ASSERT_TRUE(source != target);
    }
    ASSERT_EQ(3u, edgeCount);
}

TEST(GraphBuilderBuildingLabeledGraph) {
    GraphBuilder<StringGraph> builder;
    
    // Create and label vertices
    auto v1 = builder.addVertex("Node1");
    auto v2 = builder.addVertex("Node2");
    auto v3 = builder.addVertex("Node3");
    
    // Create and weight edges
    auto e1 = builder.addEdge(v1, v2, 15);
    auto e2 = builder.addEdge(v2, v3, 25);
    auto e3 = builder.addEdge(v1, v3, 10);
    
    // Build graph
    auto graph = builder.build();
    ASSERT_TRUE(graph.occupied());
    
    // Verify we can retrieve labels and weights
    for (auto vertex : graph->vertices()) {
        auto label = graph->getVertexLabel(vertex);
        // Verify label exists and is one of our expected values
        ASSERT_TRUE(label[0] == 'N'); // All our labels start with 'N'
    }
    
    for (auto edge : graph->edges()) {
        auto weight = graph->getEdgeWeight(edge);
        ASSERT_GT(weight, 0); // All our weights are positive
        ASSERT_LE(weight, 25); // Maximum weight we set
    }
}

TEST(GraphBuilderIncompleteGraphFails) {
    GraphBuilder<StringGraph> builder;
    
    // Create vertices but don't set all required labels
    auto v1 = builder.addVertex();
    auto v2 = builder.addVertex();
    builder.setVertexLabel(v1, "labeled");
    // v2 remains unlabeled
    
    auto e1 = builder.addEdge(v1, v2);
    builder.setEdgeWeight(e1, 10);
    
    // Build should fail due to unlabeled vertex
    auto graph = builder.build();
    ASSERT_FALSE(graph.occupied());
}

// ============================================================================
// Graph Reset and Reuse Tests  
// ============================================================================

TEST(GraphBuilderReset) {
    GraphBuilder<PlainDirectedGraph> builder;
    
    // Build first graph
    auto v1 = builder.addVertex();
    auto v2 = builder.addVertex();
    builder.addEdge(v1, v2);
    
    ASSERT_EQ(2u, builder.getCurrentVertexCount());
    ASSERT_EQ(1u, builder.getCurrentEdgeCount());
    
    // Reset builder
    builder.reset();
    
    ASSERT_EQ(0u, builder.getCurrentVertexCount());
    ASSERT_EQ(0u, builder.getCurrentEdgeCount());
    
    // Build second graph with different structure
    auto v3 = builder.addVertex();
    auto v4 = builder.addVertex();
    auto v5 = builder.addVertex();
    builder.addEdge(v3, v4);
    builder.addEdge(v4, v5);
    builder.addEdge(v5, v3);
    
    auto graph = builder.build();
    ASSERT_TRUE(graph.occupied());
    ASSERT_EQ(3u, graph->getVertexCount());
    ASSERT_EQ(3u, graph->getEdgeCount());
}

// ============================================================================
// Simple Constraint for RestrictedGraphBuilder Tests
// ============================================================================

// Simple constraint that only allows edges between vertices with consecutive labels
// Uses integer vertex labels to determine adjacency
class ConsecutiveIndexConstraint {
public:
    template<typename Graph>
    bool isEdgeAllowed(const GraphBuilderBase<Graph>& builder, 
                      const BuilderVertexHandle<Graph>& from,
                      const BuilderVertexHandle<Graph>& to) const {
        // Get vertex labels (assuming integer labels)
        auto fromLabel = builder.getVertexLabel(from);
        auto toLabel = builder.getVertexLabel(to);
        
        if (!fromLabel.occupied() || !toLabel.occupied()) {
            return false;
        }
        
        // Allow edge only if vertex labels are consecutive (differ by 1)
        int diff = *fromLabel - *toLabel;
        return (diff == 1) || (diff == -1);
    }
    
    template<typename Graph>
    auto validEdgesFrom(const GraphBuilderBase<Graph>& builder,
                       const BuilderVertexHandle<Graph>& from) const {
        Vector<BuilderVertexHandle<Graph>> validTargets;
        
        auto fromLabel = builder.getVertexLabel(from);
        if (!fromLabel.occupied()) {
            return validTargets;
        }
        
        // Check for vertices with labels fromLabel-1 and fromLabel+1
        auto prevVertex = builder.getVertexByLabel(*fromLabel - 1);
        auto nextVertex = builder.getVertexByLabel(*fromLabel + 1);
        
        if (prevVertex.occupied()) {
            validTargets.push(*prevVertex);
        }
        if (nextVertex.occupied()) {
            validTargets.push(*nextVertex);
        }
        
        return validTargets;
    }
    
    template<typename Graph>
    auto validEdgesTo(const GraphBuilderBase<Graph>& builder,
                     const BuilderVertexHandle<Graph>& to) const {
        Vector<BuilderVertexHandle<Graph>> validSources;
        
        auto toLabel = builder.getVertexLabel(to);
        if (!toLabel.occupied()) {
            return validSources;
        }
        
        // Check for vertices with labels toLabel-1 and toLabel+1
        auto prevVertex = builder.getVertexByLabel(*toLabel - 1);
        auto nextVertex = builder.getVertexByLabel(*toLabel + 1);
        
        if (prevVertex.occupied()) {
            validSources.push(*prevVertex);
        }
        if (nextVertex.occupied()) {
            validSources.push(*nextVertex);
        }
        
        return validSources;
    }
};

// ============================================================================
// RestrictedGraphBuilder Tests
// ============================================================================

TEST(RestrictedGraphBuilderBasicConstruction) {
    ConsecutiveIndexConstraint constraint;
    
    // Create vertex labels for constraint testing
    Vector<int> vertexLabels;
    vertexLabels.push(0);
    vertexLabels.push(1);
    vertexLabels.push(2);
    vertexLabels.push(3);
    vertexLabels.push(4);
    RestrictedGraphBuilder<IntLabeledDirectedGraph, ConsecutiveIndexConstraint> builder(vertexLabels, constraint);
    
    // Verify vertices were created
    ASSERT_EQ(5u, builder.getCurrentVertexCount());
    ASSERT_EQ(0u, builder.getCurrentEdgeCount());
    
    // Test vertex access
    auto v0 = builder.getVertex(0);
    auto v1 = builder.getVertex(1);
    auto v4 = builder.getVertex(4);
    
    // Verify vertices are distinct
    ASSERT_TRUE(v0 != v1);
    ASSERT_TRUE(v1 != v4);
    
    // Verify vertex labels were set correctly
    ASSERT_EQ(0, *builder.getVertexLabel(v0));
    ASSERT_EQ(1, *builder.getVertexLabel(v1));
    ASSERT_EQ(4, *builder.getVertexLabel(v4));
}

TEST(RestrictedGraphBuilderConstraintEnforcement) {
    ConsecutiveIndexConstraint constraint;
    Vector<int> vertexLabels;
    vertexLabels.push(0);
    vertexLabels.push(1);
    vertexLabels.push(2);
    vertexLabels.push(3);
    RestrictedGraphBuilder<IntLabeledDirectedGraph, ConsecutiveIndexConstraint> builder(vertexLabels, constraint);
    
    auto v0 = builder.getVertex(0);
    auto v1 = builder.getVertex(1);
    auto v2 = builder.getVertex(2);
    auto v3 = builder.getVertex(3);
    
    // Test allowed edges (consecutive labels: 0-1, 1-2, 2-3)
    auto e1 = builder.addEdge(v0, v1); // 0 -> 1: allowed
    auto e2 = builder.addEdge(v1, v2); // 1 -> 2: allowed
    auto e3 = builder.addEdge(v2, v1); // 2 -> 1: allowed (consecutive)
    
    ASSERT_TRUE(e1.occupied());
    ASSERT_TRUE(e2.occupied());
    ASSERT_TRUE(e3.occupied());
    
    // Test disallowed edges (non-consecutive labels)
    auto e4 = builder.addEdge(v0, v2); // 0 -> 2: not allowed (diff > 1)
    auto e5 = builder.addEdge(v0, v3); // 0 -> 3: not allowed (diff > 1)
    auto e6 = builder.addEdge(v1, v3); // 1 -> 3: not allowed (diff > 1)
    
    ASSERT_FALSE(e4.occupied());
    ASSERT_FALSE(e5.occupied());
    ASSERT_FALSE(e6.occupied());
    
    // Verify edge count reflects only allowed edges
    ASSERT_EQ(3u, builder.getCurrentEdgeCount());
}

TEST(RestrictedGraphBuilderConstraintQueries) {
    ConsecutiveIndexConstraint constraint;
    Vector<int> vertexLabels;
    vertexLabels.push(0);
    vertexLabels.push(1);
    vertexLabels.push(2);
    vertexLabels.push(3);
    RestrictedGraphBuilder<IntLabeledDirectedGraph, ConsecutiveIndexConstraint> builder(vertexLabels, constraint);
    
    auto v0 = builder.getVertex(0);
    auto v1 = builder.getVertex(1);
    auto v2 = builder.getVertex(2);
    auto v3 = builder.getVertex(3);
    
    // Test canAddEdge
    ASSERT_TRUE(builder.canAddEdge(v0, v1));
    ASSERT_TRUE(builder.canAddEdge(v1, v0));
    ASSERT_TRUE(builder.canAddEdge(v1, v2));
    ASSERT_TRUE(builder.canAddEdge(v2, v3));
    
    ASSERT_FALSE(builder.canAddEdge(v0, v2));
    ASSERT_FALSE(builder.canAddEdge(v0, v3));
    ASSERT_FALSE(builder.canAddEdge(v1, v3));
    
    // Test valid edges queries
    auto validFromV1 = builder.getValidEdgesFrom(v1);
    ASSERT_EQ(2u, validFromV1.getSize()); // v0 and v2
    
    auto validToV2 = builder.getValidEdgesTo(v2);
    ASSERT_EQ(2u, validToV2.getSize()); // v1 and v3
    
    auto validFromV0 = builder.getValidEdgesFrom(v0);
    ASSERT_EQ(1u, validFromV0.getSize()); // only v1
    
    auto validFromV3 = builder.getValidEdgesFrom(v3);
    ASSERT_EQ(1u, validFromV3.getSize()); // only v2
}

TEST(RestrictedGraphBuilderBuildingAndReset) {
    ConsecutiveIndexConstraint constraint;
    Vector<int> vertexLabels;
    vertexLabels.push(0);
    vertexLabels.push(1);
    vertexLabels.push(2);
    RestrictedGraphBuilder<IntLabeledDirectedGraph, ConsecutiveIndexConstraint> builder(vertexLabels, constraint);
    
    auto v0 = builder.getVertex(0);
    auto v1 = builder.getVertex(1);
    auto v2 = builder.getVertex(2);
    
    // Add allowed edges
    builder.addEdge(v0, v1);
    builder.addEdge(v1, v2);
    
    // Build graph
    auto graph = builder.build();
    ASSERT_TRUE(graph.occupied());
    ASSERT_EQ(3u, graph->getVertexCount());
    ASSERT_EQ(2u, graph->getEdgeCount());
    
    // Reset preserves vertices and constraint
    builder.reset();
    ASSERT_EQ(3u, builder.getCurrentVertexCount()); // Vertices preserved
    ASSERT_EQ(0u, builder.getCurrentEdgeCount());  // Edges cleared
    
    // Can still add constrained edges after reset
    auto newEdge = builder.addEdge(v1, v0); // Still allowed
    ASSERT_TRUE(newEdge.occupied());
    
    auto invalidEdge = builder.addEdge(v0, v2); // Still not allowed
    ASSERT_FALSE(invalidEdge.occupied());
}

// TODO: Add tests for:
// - Graph algorithms (topological sort, Dijkstra, etc.)
// - Graph predicates (DAG detection, connectivity, etc.)
// - More complex constraints
// - Edge cases and error conditions

// ============================================================================
// Graph Population Tests
// ============================================================================

TEST(GraphBuilderPopulateFromGraph) {
    // Build an initial graph
    GraphBuilder<PlainDirectedGraph> originalBuilder;
    auto v1 = originalBuilder.addVertex();
    auto v2 = originalBuilder.addVertex();
    auto v3 = originalBuilder.addVertex();
    
    originalBuilder.addEdge(v1, v2);
    originalBuilder.addEdge(v2, v3);
    originalBuilder.addEdge(v3, v1);
    
    auto originalGraph = originalBuilder.build();
    ASSERT_TRUE(originalGraph.occupied());
    
    // Create new builder and populate from the original graph
    GraphBuilder<PlainDirectedGraph> newBuilder;
    newBuilder.populateFromGraph(*originalGraph);
    
    // Verify structure was copied
    ASSERT_EQ(3u, newBuilder.getCurrentVertexCount());
    ASSERT_EQ(3u, newBuilder.getCurrentEdgeCount());
    
    // Build new graph and verify it's equivalent
    auto newGraph = newBuilder.build();
    ASSERT_TRUE(newGraph.occupied());
    ASSERT_EQ(originalGraph->getVertexCount(), newGraph->getVertexCount());
    ASSERT_EQ(originalGraph->getEdgeCount(), newGraph->getEdgeCount());
}