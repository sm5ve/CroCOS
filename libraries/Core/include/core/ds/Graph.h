//
// Created by Spencer Martin on 7/17/25.
//

#ifndef GRAPH_H
#define GRAPH_H
#include "HashSet.h"
#include "SmartPointer.h"
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

template <typename G>
class GraphBuilder;

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

    SharedPtr<VertexMetadata> vertexMetadata;
    SharedPtr<EdgeIndex> incidenceLists;
    SharedPtr<EdgeMetadata> edgeMetadata;

    [[no_unique_address]]
    OptionallyPresent<VertexDecorator::is_colored,
                      SharedPtr<typename VertexDecorator::ColorType>> vertexColors;
    [[no_unique_address]]
    OptionallyPresent<VertexDecorator::is_labeled,
                      SharedPtr<ImmutableIndexedHashSet<typename VertexDecorator::LabelType>>> vertexLabels;
    [[no_unique_address]]
    OptionallyPresent<!VertexDecorator::is_labeled,
                      VertexIndex> vertexCount;
    [[no_unique_address]]
    OptionallyPresent<EdgeDecorator::is_weighted,
                      SharedPtr<typename EdgeDecorator::WeightType>> edgeWeights;
    [[no_unique_address]]
    OptionallyPresent<EdgeDecorator::is_labeled,
                      SharedPtr<ImmutableIndexedHashSet<typename EdgeDecorator::LabelType>>> edgeLabels;
    [[no_unique_address]]
    OptionallyPresent<!EdgeDecorator::is_labeled,
                          EdgeIndex> edgeCount;

    friend class GraphBuilder<Graph<VertexDecorator, EdgeDecorator, StructureModifier>>;

    //Constructor goes here - all graphs need to be built via a graph builder
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
        using InternalState = _GraphInternal::EdgeIteratorState<EdgeDecorator::is_labeled, Graph>::type;
        InternalState state{};
        friend class Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        const Graph<VertexDecorator, EdgeDecorator, StructureModifier>& g;
        explicit GlobalEdgeIterator(InternalState s, const Graph<VertexDecorator, EdgeDecorator, StructureModifier>& graph) : state(s), g(graph) {}
    public:

        Edge operator*() const {
            if constexpr (EdgeDecorator::is_labeled) {
                return Edge(g.edgeMetadata.get(), g.edgeLabels.indexOf(*state));
            }
            else {
                return Edge(g.edgeMetadata.get(), state);
            }
        }

        GlobalEdgeIterator& operator++() {
            state++;
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
        using InternalState = _GraphInternal::VertexIteratorState<VertexDecorator::is_labeled, Graph>::type;
        InternalState state{};
        friend class Graph<VertexDecorator, EdgeDecorator, StructureModifier>;
        const Graph<VertexDecorator, EdgeDecorator, StructureModifier>& g;
        explicit VertexIterator(InternalState s, const Graph<VertexDecorator, EdgeDecorator, StructureModifier>& graph) : state(s), g(graph) {}
    public:
        Vertex operator*() const {
            if constexpr (VertexDecorator::is_labeled) {
                return Vertex(g.vertexMetadata.get(), g.vertexLabels.indexOf(*state));
            }
            else {
                return Vertex(g.vertexMetadata.get(), state);
            }
        }

        VertexIterator& operator++() {
            state++;
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
            index++;
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
};

template <typename T>
using EdgeLabeledGraph = Graph<GraphProperties::PlainVertex, GraphProperties::LabelledEdge<T>, GraphProperties::StructureModifier<GraphProperties::Undirected, GraphProperties::SimpleGraph>>;

#endif //GRAPH_H
