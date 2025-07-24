//
// Created by Spencer Martin on 7/22/25.
//

#ifndef GRAPHALGORITHMS_H
#define GRAPHALGORITHMS_H

#include <core/ds/Graph.h>
#include <core/ds/Vector.h>
#include <core/ds/Heap.h>

#include <core/algo/GraphPredicates.h>
#include <core/PrintStream.h>

namespace algorithm::graph {
    
    // Helper type for Dijkstra's algorithm - represents a vertex with its distance
    template <typename Vertex, typename Distance = size_t>
    struct VertexDistance {
        Vertex vertex;
        Distance distance;
        
        VertexDistance(Vertex v, Distance d) : vertex(v), distance(d) {}
    };
    
    // Comparator for min-heap priority queue in Dijkstra's algorithm
    // We want the vertex with the smallest distance to be at the top
    template <typename Vertex, typename Distance = size_t>
    struct DijkstraComparator {
        bool operator()(const VertexDistance<Vertex, Distance>& a, const VertexDistance<Vertex, Distance>& b) const {
            return a.distance > b.distance; // Greater than for min-heap behavior
        }
    };
    
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
        using Vertex = typename G::Vertex;
        using Distance = size_t;
        
        // Distance to each vertex (Optional means unvisited/unreachable)
        VertexAnnotation<Optional<Distance>, G> distances(graph, Optional<Distance>{});
        // Previous vertex in shortest path (for path reconstruction)
        VertexAnnotation<Optional<Vertex>, G> previous(graph, Optional<Vertex>{});
        // Track visited vertices to avoid processing them again
        VertexAnnotation<bool, G> visited(graph, false);
        
        // Initialize source
        distances[source] = 0;
        
        // Priority queue for vertices to process (min-heap by distance)
        Heap<VertexDistance<Vertex, Distance>, DijkstraComparator<Vertex, Distance>> pq;
        pq.push(VertexDistance<Vertex, Distance>(source, 0));
        
        while (!pq.empty()) {
            auto current = pq.pop();
            Vertex currentVertex = current.vertex;
            Distance currentDistance = current.distance;
            
            // Skip if already visited (we might have outdated entries in the queue)
            if (visited[currentVertex]) {
                continue;
            }
            
            visited[currentVertex] = true;
            
            // Early termination if we reached the target
            if (currentVertex == target) {
                break;
            }
            
            // Process all outgoing edges
            for (auto edge : graph.outgoingEdges(currentVertex)) {
                Vertex neighbor = graph.getTarget(edge);
                
                // Skip if already visited
                if (visited[neighbor]) {
                    continue;
                }
                
                // Calculate edge weight
                Distance edgeWeight = 1; // Default weight for unweighted graphs
                if constexpr (G::EdgeDecorator::is_weighted) {
                    edgeWeight = static_cast<Distance>(graph.getEdgeWeight(edge));
                }
                
                Distance newDistance = currentDistance + edgeWeight;
                
                // Update distance if we found a shorter path
                if (!distances[neighbor].occupied() || newDistance < *distances[neighbor]) {
                    distances[neighbor] = newDistance;
                    previous[neighbor] = currentVertex;
                    pq.push(VertexDistance<Vertex, Distance>(neighbor, newDistance));
                }
            }
        }
        
        // Check if target is reachable
        if (!distances[target].occupied()) {
            return Optional<Vector<Vertex>>{}; // No path found
        }
        
        // Reconstruct path from target back to source
        Vector<Vertex> path;
        Optional<Vertex> current = target;
        
        while (current.occupied()) {
            path.push(*current);
            current = previous[*current];
        }
        
        // Reverse path to get source -> target order
        Vector<Vertex> result;
        for (size_t i = path.getSize(); i > 0; --i) {
            result.push(path[i - 1]);
        }
        
        return Optional<Vector<Vertex>>(move(result));
    }
}

#endif //GRAPHALGORITHMS_H
