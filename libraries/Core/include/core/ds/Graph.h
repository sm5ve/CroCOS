//
// Created by Spencer Martin on 7/17/25.
//

#ifndef GRAPH_H
#define GRAPH_H
#include "HashSet.h"
#include "SmartPointer.h"
#include "Tuple.h"
#include "core/Iterator.h"

namespace GraphProperties {
    struct None{};

    template <typename Color, typename Label>
    struct VertexDecorator {
        using ColorType = Color;
        using LabelType = Label;

        constexpr static bool is_colored = !is_same_v<Color, None>;
        constexpr static bool is_labeled = !is_same_v<Label, None>;
    };

    using PlainVertex = VertexDecorator<None, None>;
    template <typename T>
    using ColoredVertex = VertexDecorator<T, None>;
    template <typename T>
    using LabelledVertex = VertexDecorator<None, T>;
    template <typename T, typename U>
    using ColoredLabelledVertex = VertexDecorator<T, U>;

    template <typename Weight, typename Label>
    struct EdgeDecorator {
        using WeightType = Weight;
        using LabelType = Label;

        constexpr static bool is_weighted = !is_same_v<Weight, None>;
        constexpr static bool is_labeled = !is_same_v<Label, None>;
    };

    using PlainEdge = EdgeDecorator<None, None>;
    template <typename T>
    using WeightedEdge = EdgeDecorator<T, None>;
    template <typename T>
    using LabelledEdge = EdgeDecorator<None, T>;
    template <typename T, typename U>
    using WeightedLabelledEdge = EdgeDecorator<T, U>;

    template<typename T, typename GraphType>
    concept GraphPredicate = requires(const GraphType& graph) {
            { T::check(graph) } -> convertible_to<bool>;
    };

    struct DirectionPolicy {};
    struct Directed : DirectionPolicy {};
    struct Undirected : DirectionPolicy {};

    struct MultigraphPolicy {};
    struct SimpleGraph : MultigraphPolicy {};
    struct Multigraph : MultigraphPolicy {};

    template <typename Dir, typename Mult, typename... Predicates>
    struct StructureModifier {
        using directionPolicy = Dir;
        using multigraphPolicy = Mult;
        using predicates = type_list<Predicates...>;

        static constexpr bool is_directed = is_same_v<Dir, Directed>;
        static constexpr bool is_undirected = is_same_v<Dir, Undirected>;
        static constexpr bool is_multigraph = is_same_v<Mult, Multigraph>;
        static constexpr bool is_simple_graph = is_same_v<Mult, SimpleGraph>;

        template <typename GraphType>
        static bool check(const GraphType& graph) {
            return (true && ... && Predicates::check(graph));
        }
    };
}

namespace _GraphBuilder {
    template <typename G>
    class GraphBuilderImpl;
}

namespace _GraphInternal {
    template <bool Labeled, typename GraphType>
    struct VertexIteratorState;

    template <typename GraphType>
    struct VertexIteratorState<true, GraphType> {
        using type = decltype(GraphType::vertexLabels->begin());
    };

    template <typename GraphType>
    struct VertexIteratorState<false, GraphType> {
        using type = typename GraphType::VertexIndex;
    };

    template <bool Labeled, typename GraphType>
    struct EdgeIteratorState;

    template <typename GraphType>
    struct EdgeIteratorState<true, GraphType> {
        using type = decltype(GraphType::edgeLabels->begin());
    };

    template <typename GraphType>
    struct EdgeIteratorState<false, GraphType> {
        using type = typename GraphType::EdgeIndex;
    };
}

template <typename VertexDecorator_t, typename EdgeDecorator_t, typename StructureModifier_t>
class Graph{
public:
    class Vertex;
    class Edge;

    using VertexDecorator = VertexDecorator_t;
    using EdgeDecorator = EdgeDecorator_t;
    using StructureModifier = StructureModifier_t;
private:
    friend Vertex;
    friend Edge;
    friend _GraphInternal::VertexIteratorState<VertexDecorator::is_labeled, Graph>;
    friend _GraphInternal::EdgeIteratorState<EdgeDecorator::is_labeled, Graph>;

    using BasicIndex = size_t;
    using VertexIndex = BasicIndex;
    using EdgeIndex = BasicIndex;

    struct UndirectedVertexMetadata {
        BasicIndex start;
        BasicIndex size;

        [[nodiscard]] BasicIndex fromStart() const {
            return start;
        }

        [[nodiscard]] BasicIndex toStart() const {
            return start;
        }

        [[nodiscard]] BasicIndex fromEnd() const {
            return start + size;
        }

        [[nodiscard]] BasicIndex toEnd() const {
            return start + size;
        }

        [[nodiscard]] BasicIndex totalStart() const {
            return start;
        }

        [[nodiscard]] BasicIndex totalEnd() const {
            return start + size;
        }
    };

    struct DirectedVertexMetadata {
        BasicIndex start;
        BasicIndex fromSize;
        BasicIndex toSize;

        [[nodiscard]] BasicIndex fromStart() const {
            return start;
        }

        [[nodiscard]] BasicIndex toStart() const {
            return start + fromSize;
        }

        [[nodiscard]] BasicIndex fromEnd() const {
            return start + fromSize;
        }

        [[nodiscard]] BasicIndex toEnd() const {
            return start + fromSize + toSize;
        }

        [[nodiscard]] BasicIndex totalStart() const {
            return start;
        }

        [[nodiscard]] BasicIndex totalEnd() const {
            return start + fromSize + toSize;
        }
    };

    using VertexMetadata = conditional_t<StructureModifier::is_directed, DirectedVertexMetadata, UndirectedVertexMetadata>;

    struct EdgeMetadata {
        VertexIndex from;
        VertexIndex to;
    };

    struct Empty{};
    template<bool condition, typename T>
    using OptionallyPresent = conditional_t<condition, T, Empty>;

    SharedPtr<VertexMetadata[]> vertexMetadata;
    SharedPtr<EdgeIndex[]> incidenceLists;
    SharedPtr<EdgeMetadata[]> edgeMetadata;

    [[no_unique_address]]
    OptionallyPresent<VertexDecorator::is_colored,
                      SharedPtr<typename VertexDecorator::ColorType[]>> vertexColors;
    [[no_unique_address]]
    OptionallyPresent<VertexDecorator::is_labeled,
                      SharedPtr<ImmutableIndexedHashSet<typename VertexDecorator::LabelType>>> vertexLabels;
    [[no_unique_address]]
    OptionallyPresent<!VertexDecorator::is_labeled,
                      VertexIndex> vertexCount;
    [[no_unique_address]]
    OptionallyPresent<EdgeDecorator::is_weighted,
                      SharedPtr<typename EdgeDecorator::WeightType[]>> edgeWeights;
    [[no_unique_address]]
    OptionallyPresent<EdgeDecorator::is_labeled,
                      SharedPtr<ImmutableIndexedHashSet<typename EdgeDecorator::LabelType>>> edgeLabels;
    [[no_unique_address]]
    OptionallyPresent<!EdgeDecorator::is_labeled,
                          EdgeIndex> edgeCount;

    friend class _GraphBuilder::GraphBuilderImpl<Graph<VertexDecorator, EdgeDecorator, StructureModifier>>;

    // Private default constructor - only GraphBuilder can create Graph instances
    Graph() = default;
public:
    class Vertex {
    private:
        friend class Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        VertexMetadata* graphIdentifier;
        VertexIndex index;
        Vertex(VertexMetadata* gi, VertexIndex ind) : graphIdentifier(gi), index(ind) {}
    public:
        bool operator==(const Vertex & v) const {
            return graphIdentifier == v.graphIdentifier && index == v.index;
        }
    };

    class Edge {
    private:
        friend class Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        EdgeMetadata* graphIdentifier;
        EdgeIndex index;
        [[no_unique_address]]
        OptionallyPresent<StructureModifier::is_undirected,
                          bool> flip;
        Edge(EdgeMetadata* gi, EdgeIndex ind) : graphIdentifier(gi), index(ind)
            {if constexpr (StructureModifier::is_undirected) flip = false;}
        Edge(EdgeMetadata* gi, EdgeIndex ind, bool flip_edge) requires StructureModifier::is_undirected :
            graphIdentifier(gi), index(ind), flip(flip_edge) {}
    public:
        bool operator==(const Edge & e) const {
            return graphIdentifier == e.graphIdentifier && index == e.index;
        }
    };

    class GlobalEdgeIterator {
    private:
        using InternalState = typename _GraphInternal::EdgeIteratorState<EdgeDecorator::is_labeled, Graph>::type;
        InternalState state{};
        friend class Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        const Graph<VertexDecorator, EdgeDecorator, StructureModifier>& g;
        explicit GlobalEdgeIterator(InternalState s, const Graph<VertexDecorator, EdgeDecorator, StructureModifier>& graph) : state(s), g(graph) {}
    public:

        Edge operator*() const {
            if constexpr (EdgeDecorator::is_labeled) {
                return Edge(g.edgeMetadata.get(), *(g.edgeLabels -> indexOf(*state)));
            }
            else {
                return Edge(g.edgeMetadata.get(), state);
            }
        }

        GlobalEdgeIterator& operator++() {
            ++state;
            return *this;
        }

        bool operator!=(const GlobalEdgeIterator& other) const {
            return state != other.state;
        }
    };

    IteratorRange<GlobalEdgeIterator> edges() const {
        if constexpr (EdgeDecorator::is_labeled) {
            return IteratorRange<GlobalEdgeIterator>(GlobalEdgeIterator(edgeLabels -> begin(), *this), GlobalEdgeIterator(edgeLabels -> end(), *this));
        }
        else {
            return IteratorRange<GlobalEdgeIterator>(GlobalEdgeIterator(0, *this), GlobalEdgeIterator(edgeCount, *this));
        }
    }

    class VertexIterator {
    private:
        using InternalState = typename _GraphInternal::VertexIteratorState<VertexDecorator::is_labeled, Graph>::type;
        InternalState state{};
        friend class Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        const Graph<VertexDecorator, EdgeDecorator, StructureModifier>& g;
        explicit VertexIterator(InternalState s, const Graph<VertexDecorator, EdgeDecorator, StructureModifier>& graph) : state(s), g(graph) {}
    public:
        Vertex operator*() const {
            if constexpr (VertexDecorator::is_labeled) {
                return Vertex(g.vertexMetadata.get(), static_cast<VertexIndex>(*(g.vertexLabels -> indexOf(*state))));
            }
            else {
                return Vertex(g.vertexMetadata.get(), state);
            }
        }

        VertexIterator& operator++() {
            ++state;
            return *this;
        }

        bool operator!=(const VertexIterator& other) const {
            return state != other.state;
        }
    };

    IteratorRange<VertexIterator> vertices() const {
        if constexpr (VertexDecorator::is_labeled) {
            return IteratorRange<VertexIterator>(VertexIterator(vertexLabels -> begin(), *this), VertexIterator(vertexLabels -> end(), *this));
        }
        else {
            return IteratorRange<VertexIterator>(VertexIterator(0, *this), VertexIterator(vertexCount, *this));
        }
    }

    enum AdjacentEdgeDirection {
        In,
        Out,
        Both
    };

    template <AdjacentEdgeDirection direction>
    struct AdjacentEdgeIterator {
    private:
        VertexIndex vertex;
        size_t index;
        using G = Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        friend class Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        const G& graph;
        AdjacentEdgeIterator(VertexIndex v, size_t i, const G& g) : vertex(v), index(i), graph(g) {}
    public:
        bool operator!=(const AdjacentEdgeIterator<direction>& other) const {
            return (vertex != other.vertex) || (index != other.index);
        }

        AdjacentEdgeIterator<direction>& operator++() {
            ++index;
            return *this;
        }

        Edge operator*() {
            if constexpr (StructureModifier::is_undirected) {
                if constexpr (direction == In) {
                    EdgeIndex ei = graph.incidenceLists[index];
                    auto metadata = graph.edgeMetadata[ei];
                    if (metadata.from == vertex) {
                        return Edge(graph.edgeMetadata.get(), ei, true);
                    }
                    else {
                        return Edge(graph.edgeMetadata.get(), ei, false);
                    }
                }
                else if constexpr (direction == Out) {
                    EdgeIndex ei = graph.incidenceLists[index];
                    auto metadata = graph.edgeMetadata[ei];
                    if (metadata.to == vertex) {
                        return Edge(graph.edgeMetadata.get(), ei, true);
                    }
                    else {
                        return Edge(graph.edgeMetadata.get(), ei, false);
                    }
                }
                else {
                    return Edge(graph.edgeMetadata.get(), graph.incidenceLists[index]);
                }
            }
            else {
                return Edge(graph.edgeMetadata.get(), graph.incidenceLists[index]);
            }
        }
    };

    IteratorRange<AdjacentEdgeIterator<AdjacentEdgeDirection::In>> incomingEdges(Vertex v) const {
        using It = AdjacentEdgeIterator<AdjacentEdgeDirection::In>;
        auto& metadata = vertexMetadata[v.index];
        assert(v.graphIdentifier == vertexMetadata.get(), "Vertex must be from the same graph as the graph it was retrieved from");
        return IteratorRange<It>(It(v.index, metadata.toStart(), *this),It(v.index, metadata.toEnd(), *this));
    }

    IteratorRange<AdjacentEdgeIterator<AdjacentEdgeDirection::Out>> outgoingEdges(Vertex v) const {
        using It = AdjacentEdgeIterator<AdjacentEdgeDirection::Out>;
        auto& metadata = vertexMetadata[v.index];
        assert(v.graphIdentifier == vertexMetadata.get(), "Vertex must be from the same graph as the graph it was retrieved from");
        return IteratorRange<It>(It(v.index, metadata.fromStart(), *this),It(v.index, metadata.fromEnd(), *this));
    }

    IteratorRange<AdjacentEdgeIterator<AdjacentEdgeDirection::Both>> incidentEdges(Vertex v) const {
        using It = AdjacentEdgeIterator<AdjacentEdgeDirection::Both>;
        auto& metadata = vertexMetadata[v.index];
        assert(v.graphIdentifier == vertexMetadata.get(), "Vertex must be from the same graph as the graph it was retrieved from");
        return IteratorRange<It>(It(v.index, metadata.totalStart(), *this),It(v.index, metadata.totalEnd(), *this));
    }

    template <AdjacentEdgeDirection direction>
    struct AdjacentVertexIterator {
    private:
        AdjacentEdgeIterator<direction> edgeIterator;
        using G = Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        friend class Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        const G& graph;
        VertexIndex sourceVertex;
        AdjacentVertexIterator(AdjacentEdgeIterator<direction> it, const G& g, VertexIndex sv) : edgeIterator(it), graph(g), sourceVertex(sv) {}
    public:
        bool operator!=(const AdjacentVertexIterator<direction>& other) const {
            return edgeIterator != other.edgeIterator;
        }

        AdjacentVertexIterator<direction>& operator++() {
            ++edgeIterator;
            return *this;
        }

        Vertex operator*() {
            Edge e = *edgeIterator;
            auto& metadata = graph.edgeMetadata[e.index];
            
            if constexpr (StructureModifier::is_undirected) {
                if constexpr (direction == In) {
                    if (e.flip) {
                        return Vertex(graph.vertexMetadata.get(), metadata.to);
                    } else {
                        return Vertex(graph.vertexMetadata.get(), metadata.from);
                    }
                } else if constexpr (direction == Out) {
                    if (e.flip) {
                        return Vertex(graph.vertexMetadata.get(), metadata.from);
                    } else {
                        return Vertex(graph.vertexMetadata.get(), metadata.to);
                    }
                } else {
                    return (metadata.from == sourceVertex) ? 
                           Vertex(graph.vertexMetadata.get(), metadata.to) :
                           Vertex(graph.vertexMetadata.get(), metadata.from);
                }
            } else {
                if constexpr (direction == In) {
                    return Vertex(graph.vertexMetadata.get(), metadata.from);
                } else if constexpr (direction == Out) {
                    return Vertex(graph.vertexMetadata.get(), metadata.to);
                } else {
                    return (metadata.from == sourceVertex) ? 
                           Vertex(graph.vertexMetadata.get(), metadata.to) :
                           Vertex(graph.vertexMetadata.get(), metadata.from);
                }
            }
        }
    };

    IteratorRange<AdjacentVertexIterator<AdjacentEdgeDirection::In>> incomingVertices(Vertex v) const {
        using It = AdjacentVertexIterator<AdjacentEdgeDirection::In>;
        auto inEdges = incomingEdges(v);
        return IteratorRange<It>(It(inEdges.begin(), *this, v.index), It(inEdges.end(), *this, v.index));
    }

    IteratorRange<AdjacentVertexIterator<AdjacentEdgeDirection::Out>> outgoingVertices(Vertex v) const {
        using It = AdjacentVertexIterator<AdjacentEdgeDirection::Out>;
        auto outEdges = outgoingEdges(v);
        return IteratorRange<It>(It(outEdges.begin(), *this, v.index), It(outEdges.end(), *this, v.index));
    }

    IteratorRange<AdjacentVertexIterator<AdjacentEdgeDirection::Both>> adjacentVertices(Vertex v) const {
        using It = AdjacentVertexIterator<AdjacentEdgeDirection::Both>;
        auto incEdges = incidentEdges(v);
        return IteratorRange<It>(It(incEdges.begin(), *this, v.index), It(incEdges.end(), *this, v.index));
    }

    [[nodiscard]] size_t getVertexCount() const {
        if constexpr (VertexDecorator::is_labeled) {
            return vertexLabels->size();
        } else {
            return vertexCount;
        }
    }

    [[nodiscard]] size_t getEdgeCount() const {
        if constexpr (EdgeDecorator::is_labeled) {
            return edgeLabels->size();
        } else {
            return edgeCount;
        }
    }

    template<typename T = typename VertexDecorator::LabelType>
    requires VertexDecorator::is_labeled
    Optional<Vertex> getVertexByLabel(const T& label) const {
        auto indexOpt = vertexLabels->indexOf(label);
        if (indexOpt) {
            return Vertex(vertexMetadata.get(), *indexOpt);
        }
        return Optional<Vertex>();
    }

    template<typename T = typename EdgeDecorator::LabelType>
    requires EdgeDecorator::is_labeled
    Optional<Edge> getEdgeByLabel(const T& label) const {
        auto indexOpt = edgeLabels->indexOf(label);
        if (indexOpt) {
            return Edge(edgeMetadata.get(), *indexOpt);
        }
        return Optional<Edge>();
    }

    // Methods to get vertices/edges by index for unlabeled graphs
    Optional<Vertex> getVertex(VertexIndex index) const 
    requires (!VertexDecorator::is_labeled) {
        if (index >= getVertexCount()) {
            return Optional<Vertex>();
        }
        return Vertex(vertexMetadata.get(), index);
    }
    
    Optional<Edge> getEdge(EdgeIndex index) const 
    requires (!EdgeDecorator::is_labeled) {
        if (index >= getEdgeCount()) {
            return Optional<Edge>();
        }
        return Edge(edgeMetadata.get(), index);
    }

    template<typename T = typename VertexDecorator::LabelType>
    requires VertexDecorator::is_labeled
    const T& getVertexLabel(Vertex v) const {
        assert(v.graphIdentifier == vertexMetadata.get(), "Vertex must be from the same graph");
        return *(vertexLabels -> fromIndex(v.index));
    }

    template<typename T = typename VertexDecorator::ColorType>
    requires VertexDecorator::is_colored
    const T& getVertexColor(Vertex v) const {
        assert(v.graphIdentifier == vertexMetadata.get(), "Vertex must be from the same graph");
        return vertexColors[v.index];
    }

    template<typename T = typename EdgeDecorator::LabelType>
    requires EdgeDecorator::is_labeled
    const T& getEdgeLabel(Edge e) const {
        assert(e.graphIdentifier == edgeMetadata.get(), "Edge must be from the same graph");
        return *edgeLabels -> fromIndex(e.index);
    }

    template<typename T = typename EdgeDecorator::WeightType>
    requires EdgeDecorator::is_weighted
    const T& getEdgeWeight(Edge e) const {
        assert(e.graphIdentifier == edgeMetadata.get(), "Edge must be from the same graph");
        return edgeWeights[e.index];
    }

    Vertex getSource(Edge e) const {
        assert(e.graphIdentifier == edgeMetadata.get(), "Edge must be from the same graph");
        auto& metadata = edgeMetadata[e.index];
        if constexpr (StructureModifier::is_undirected) {
            if (e.flip) {
                return Vertex(vertexMetadata.get(), metadata.to);
            }
        }
        return Vertex(vertexMetadata.get(), metadata.from);
    }

    Vertex getTarget(Edge e) const {
        assert(e.graphIdentifier == edgeMetadata.get(), "Edge must be from the same graph");
        auto& metadata = edgeMetadata[e.index];
        if constexpr (StructureModifier::is_undirected) {
            if (e.flip) {
                return Vertex(vertexMetadata.get(), metadata.from);
            }
        }
        return Vertex(vertexMetadata.get(), metadata.to);
    }

    Tuple<Vertex, Vertex> getEndpoints(Edge e) const {
        return Tuple<Vertex, Vertex>(getSource(e), getTarget(e));
    }

    [[nodiscard]] size_t inDegree(Vertex v) const {
        assert(v.graphIdentifier == vertexMetadata.get(), "Vertex must be from the same graph");
        auto& metadata = vertexMetadata[v.index];
        if constexpr (StructureModifier::is_directed) {
            return metadata.toSize;
        } else {
            return metadata.size;
        }
    }

    [[nodiscard]] size_t outDegree(Vertex v) const {
        assert(v.graphIdentifier == vertexMetadata.get(), "Vertex must be from the same graph");
        auto& metadata = vertexMetadata[v.index];
        if constexpr (StructureModifier::is_directed) {
            return metadata.fromSize;
        } else {
            return metadata.size;
        }
    }

    [[nodiscard]] size_t degree(Vertex v) const {
        assert(v.graphIdentifier == vertexMetadata.get(), "Vertex must be from the same graph");
        auto& metadata = vertexMetadata[v.index];
        if constexpr (StructureModifier::is_directed) {
            return metadata.fromSize + metadata.toSize;
        } else {
            return metadata.size;
        }
    }

    [[nodiscard]] bool hasEdge(Vertex from, Vertex to) const {
        assert(from.graphIdentifier == vertexMetadata.get(), "From vertex must be from the same graph");
        assert(to.graphIdentifier == vertexMetadata.get(), "To vertex must be from the same graph");
        
        if constexpr (StructureModifier::is_directed) {
            // For directed graphs, check outgoing edges from 'from' vertex
            for (auto edge : outgoingEdges(from)) {
                if (getTarget(edge).index == to.index) {
                    return true;
                }
            }
            return false;
        } else {
            // For undirected graphs, check incident edges from 'from' vertex
            // An edge exists if there's any edge connecting these two vertices
            for (auto edge : incidentEdges(from)) {
                auto endpoints = getEndpoints(edge);
                if ((endpoints.first.index == from.index && endpoints.second.index == to.index) ||
                    (endpoints.first.index == to.index && endpoints.second.index == from.index)) {
                    return true;
                }
            }
            return false;
        }
    }

    [[nodiscard]] Optional<Edge> findEdge(Vertex from, Vertex to) const {
        assert(from.graphIdentifier == vertexMetadata.get(), "From vertex must be from the same graph");
        assert(to.graphIdentifier == vertexMetadata.get(), "To vertex must be from the same graph");
        
        if constexpr (StructureModifier::is_directed) {
            // For directed graphs, check outgoing edges from 'from' vertex
            for (auto edge : outgoingEdges(from)) {
                if (getTarget(edge).index == to.index) {
                    return edge;
                }
            }
            return Optional<Edge>();
        } else {
            // For undirected graphs, check incident edges from 'from' vertex
            for (auto edge : incidentEdges(from)) {
                auto endpoints = getEndpoints(edge);
                if ((endpoints.first.index == from.index && endpoints.second.index == to.index) ||
                    (endpoints.first.index == to.index && endpoints.second.index == from.index)) {
                    return edge;
                }
            }
            return {};
        }
    }
};

template <typename T>
using EdgeLabeledGraph = Graph<GraphProperties::PlainVertex, GraphProperties::LabelledEdge<T>, GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;

// Simple graph aliases
using DirectedGraph = Graph<GraphProperties::PlainVertex, GraphProperties::PlainEdge, GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;
using UndirectedGraph = Graph<GraphProperties::PlainVertex, GraphProperties::PlainEdge, GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;

// Weighted graph aliases
template <typename WeightType>
using WeightedDirectedGraph = Graph<GraphProperties::PlainVertex, GraphProperties::WeightedEdge<WeightType>, GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;
template <typename WeightType>
using WeightedUndirectedGraph = Graph<GraphProperties::PlainVertex, GraphProperties::WeightedEdge<WeightType>, GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;

// Vertex labeled graph aliases
template <typename VertexLabelType>
using VertexLabeledDirectedGraph = Graph<GraphProperties::LabelledVertex<VertexLabelType>, GraphProperties::PlainEdge, GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;
template <typename VertexLabelType>
using VertexLabeledUndirectedGraph = Graph<GraphProperties::LabelledVertex<VertexLabelType>, GraphProperties::PlainEdge, GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;

// Edge labeled graph aliases (directed version of existing)
template <typename EdgeLabelType>
using EdgeLabeledDirectedGraph = Graph<GraphProperties::PlainVertex, GraphProperties::LabelledEdge<EdgeLabelType>, GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;

// Fully labeled graph aliases
template <typename VertexLabelType, typename EdgeLabelType>
using LabeledDirectedGraph = Graph<GraphProperties::LabelledVertex<VertexLabelType>, GraphProperties::LabelledEdge<EdgeLabelType>, GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;
template <typename VertexLabelType, typename EdgeLabelType>
using LabeledUndirectedGraph = Graph<GraphProperties::LabelledVertex<VertexLabelType>, GraphProperties::LabelledEdge<EdgeLabelType>, GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;

// Colored vertex graph aliases
template <typename ColorType>
using ColoredDirectedGraph = Graph<GraphProperties::ColoredVertex<ColorType>, GraphProperties::PlainEdge, GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;
template <typename ColorType>
using ColoredUndirectedGraph = Graph<GraphProperties::ColoredVertex<ColorType>, GraphProperties::PlainEdge, GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;

// Weighted and labeled combinations
template <typename WeightType, typename EdgeLabelType>
using WeightedLabeledDirectedGraph = Graph<GraphProperties::PlainVertex, GraphProperties::WeightedLabelledEdge<WeightType, EdgeLabelType>, GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph>>;
template <typename WeightType, typename EdgeLabelType>
using WeightedLabeledUndirectedGraph = Graph<GraphProperties::PlainVertex, GraphProperties::WeightedLabelledEdge<WeightType, EdgeLabelType>, GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;
#endif //GRAPH_H
