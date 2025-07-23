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

}

#endif //GRAPHALGORITHMS_H
