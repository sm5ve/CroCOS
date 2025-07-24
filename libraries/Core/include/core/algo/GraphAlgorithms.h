//
// Created by Spencer Martin on 7/22/25.
//

#ifndef GRAPHALGORITHMS_H
#define GRAPHALGORITHMS_H

#include <core/ds/Graph.h>
#include <core/ds/Vector.h>

#include <core/algo/GraphPredicates.h>

namespace algorithm::graph {
    
    // Topological sort using Kahn's algorithm
    // Only works on DAGs (enforced by concept)
    template <typename G>
    Vector<typename G::Vertex> topologicalSort(const G& graph) requires
    GraphProperties::GraphHasPredicate<G, GraphPredicates::DirectedAcyclic> {
        
        Vector<typename G::Vertex> result;
        Vector<typename G::Vertex> queue;
        
        // Track in-degrees using vertex annotation
        VertexAnnotation<size_t, G> inDegree(graph, 0);
        
        // Initialize in-degrees
        for (auto vertex : graph.vertices()) {
            inDegree[vertex] = graph.inDegree(vertex);
            
            // Add vertices with in-degree 0 to the queue
            if (inDegree[vertex] == 0) {
                queue.push(vertex);
            }
        }
        
        // Process vertices in topological order
        while (queue.getSize() > 0) {
            // Remove a vertex with in-degree 0
            auto current = queue.pop();
            result.push(current);
            
            // For each outgoing edge, reduce in-degree of target vertex
            for (auto edge : graph.outgoingEdges(current)) {
                auto target = graph.getTarget(edge);
                --inDegree[target];
                
                // If target now has in-degree 0, add it to queue
                if (inDegree[target] == 0) {
                    queue.push(target);
                }
            }
        }
        
        return result;
    }

    template <typename G>
    void printAsDOT(Core::PrintStream& ps, const G& graph){
        if constexpr (G::StructureModifier::is_simple_graph) {
            ps << "strict ";
        }
        if constexpr (G::StructureModifier::is_directed) {
            ps << "digraph ";
        }
        else {
            ps << "graph ";
        }

        ps << "{\n";

        VertexAnnotation<size_t, G> indexedVertices(graph, 0);
        size_t currentIndex = 0;
        for (auto vertex : graph.vertices()) {
            indexedVertices[vertex] = currentIndex++;
            ps << "\t v" << indexedVertices[vertex];
            if constexpr (G::VertexDecorator::is_labeled && Core::Printable<typename G::VertexDecorator::LabelType>) {
               ps << " [label=\"" << graph.getVertexLabel(vertex) << "\"]";
            }
            ps << ";\n";
        }

        ps << "\n";

        for (auto edge : graph.edges()) {
            ps << "\t v" << indexedVertices[graph.getSource(edge)];
            if constexpr (G::StructureModifier::is_directed) {
                ps << " -> ";
            } else {
                ps << " -- ";
            }
            ps << indexedVertices[graph.getTarget(edge)];
            if constexpr (G::EdgeDecorator::is_labeled && Core::Printable<typename G::EdgeDecorator::LabelType>) {
                ps << " [label=\"" << graph.getEdgeLabel(edge) << "\"]";
            }
            ps << ";\n";
        }

        ps << "}\n";
    }

    template <typename G>
    requires (!G::EdgeDecorator::is_weighted) || GraphProperties::GraphHasPredicate<G, GraphPredicates::NonnegativeWeight>
    Optional<Vector<typename G::Vertex>> dijkstra(const typename G::Vertex source, const typename G::Vertex target, const G& graph) {

        return {};
    }
}

#endif //GRAPHALGORITHMS_H
