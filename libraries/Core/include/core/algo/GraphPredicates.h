//
// Created by Spencer Martin on 7/23/25.
//

#ifndef GRAPHPREDICATES_H
#define GRAPHPREDICATES_H

#include <core/ds/Vector.h>
#include <core/ds/Graph.h>

namespace GraphPredicates {
    
    // Predicate to check if a directed graph is acyclic (DAG)
    struct DirectedAcyclic {
        template<typename GraphType>
        static bool check(const GraphType& graph) {
            static_assert(GraphType::StructureModifier::is_directed, 
                         "DirectedAcyclic predicate can only be used with directed graphs");
            
            // Use DFS with three colors: white (unvisited), gray (visiting), black (visited)
            enum Color { White, Gray, Black };
            
            // Use vertex annotation to map vertices to colors
            // We need a non-const reference to the graph for annotations
            auto& mutableGraph = const_cast<GraphType&>(graph);
            VertexAnnotation<Color, GraphType> colors(mutableGraph, White);
            
            // Helper lambda for DFS cycle detection
            auto hasCycleDFS = [&](auto vertex, auto& self) -> bool {
                colors[vertex] = Gray;
                
                // Check all outgoing edges
                for (auto edge : graph.outgoingEdges(vertex)) {
                    auto target = graph.getTarget(edge);
                    
                    if (colors[target] == Gray) {
                        // Found back edge - cycle detected
                        return true;
                    }
                    
                    if (colors[target] == White && self(target, self)) {
                        return true;
                    }
                }
                
                colors[vertex] = Black;
                return false;
            };
            
            // Check each vertex as a potential starting point
            for (auto vertex : graph.vertices()) {
                if (colors[vertex] == White) {
                    if (hasCycleDFS(vertex, hasCycleDFS)) {
                        return false; // Cycle found - not a DAG
                    }
                }
            }
            
            return true; // No cycles found - is a DAG
        }
    };
    
    // Predicate to check if graph is connected (for undirected graphs)
    struct Connected {
        template<typename GraphType>
        static bool check(const GraphType& graph) {
            static_assert(GraphType::StructureModifier::is_undirected,
                         "Connected predicate can only be used with undirected graphs");
            
            if (graph.getVertexCount() == 0) return true;
            
            // Use vertex annotation to track visited vertices
            auto& mutableGraph = const_cast<GraphType&>(graph);
            VertexAnnotation<bool, GraphType> visited(mutableGraph, false);
            
            // Start DFS from first vertex
            auto vertices = graph.vertices();
            auto startVertex = *vertices.begin();
            
            auto dfs = [&](auto vertex, auto& self) -> void {
                visited[vertex] = true;
                for (auto edge : graph.incidentEdges(vertex)) {
                    auto neighbor = (graph.getSource(edge) == vertex) ? 
                                   graph.getTarget(edge) : graph.getSource(edge);
                    if (!visited[neighbor]) {
                        self(neighbor, self);
                    }
                }
            };
            
            dfs(startVertex, dfs);
            
            // Check if all vertices were visited
            for (auto vertex : graph.vertices()) {
                if (!visited[vertex]) {
                    return false;
                }
            }
            
            return true;
        }
    };
    
    // Predicate to check if directed graph is strongly connected
    struct StronglyConnected {
        template<typename GraphType>
        static bool check(const GraphType& graph) {
            static_assert(GraphType::StructureModifier::is_directed,
                         "StronglyConnected predicate can only be used with directed graphs");
            
            if (graph.getVertexCount() == 0) return true;
            
            auto& mutableGraph = const_cast<GraphType&>(graph);
            
            // For strong connectivity, every vertex must be reachable from every other vertex
            // We'll check if all vertices are reachable from the first vertex,
            // then check the reverse (using incoming edges as if they were outgoing)
            
            auto isReachableFromVertex = [&](auto startVertex, bool useOutgoing) -> bool {
                VertexAnnotation<bool, GraphType> visited(mutableGraph, false);
                
                auto dfs = [&](auto vertex, auto& self) -> void {
                    visited[vertex] = true;
                    auto edges = useOutgoing ? graph.outgoingEdges(vertex) : graph.incomingEdges(vertex);
                    for (auto edge : edges) {
                        auto neighbor = useOutgoing ? graph.getTarget(edge) : graph.getSource(edge);
                        if (!visited[neighbor]) {
                            self(neighbor, self);
                        }
                    }
                };
                
                dfs(startVertex, dfs);
                
                // Check if all vertices were visited
                for (auto vertex : graph.vertices()) {
                    if (!visited[vertex]) {
                        return false;
                    }
                }
                return true;
            };
            
            auto vertices = graph.vertices();
            auto startVertex = *vertices.begin();
            
            // Check reachability in both directions
            return isReachableFromVertex(startVertex, true) && 
                   isReachableFromVertex(startVertex, false);
        }
    };

    struct NonnegativeWeight {
        template<typename GraphType>
        requires GraphType::EdgeDecorator::is_weighted && OrderedSemigroup<typename GraphType::EdgeDecorator::WeightType>
        static bool check(const GraphType& graph) {
            for (auto edge : graph.edges()) {
                auto weight = graph.template getEdgeWeight<typename GraphType::EdgeDecorator::WeightType>(edge);
                if constexpr (requires { { 0 } -> convertible_to<typename GraphType::EdgeDecorator::WeightType>; }) {
                    if (weight < 0) { return false; }
                } else {
                    //for ordered semigroups without identity, this captures the notion of negativity
                    if (weight + weight < weight) { return false; }
                }
            }
            return true; // All weights are non-negative
        }
    };
}

#endif //GRAPHPREDICATES_H