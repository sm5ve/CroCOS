//
// Created by Spencer Martin on 7/21/25.
//

#ifndef GRAPHBUILDER_H
#define GRAPHBUILDER_H

#include <core/ds/Vector.h>
#include <core/ds/Optional.h>
#include <core/ds/Graph.h>
#include <core/ds/SmartPointer.h>
#include <core/ds/HashSet.h>
#include <core/math.h>

namespace _GraphBuilder {
    template <bool HasLabel, bool HasColor, typename G>
    struct PartialVertexInfoImpl{};

    template <typename G>
    struct PartialVertexInfoImpl<true, true, G> {
        using Dec = typename G::VertexDecorator;
        Optional<typename Dec::LabelType> label;
        Optional<typename Dec::ColorType> color;
        size_t index;
        size_t incomingEdgeCount;
        size_t outgoingEdgeCount;
        
        explicit PartialVertexInfoImpl(const size_t i) : index(i), incomingEdgeCount(0), outgoingEdgeCount(0) {}
        
        [[nodiscard]] bool fullyPopulated() const {
            return label.occupied() && color.occupied();
        }
    };

    template <typename G>
    struct PartialVertexInfoImpl<true, false, G> {
        using Dec = typename G::VertexDecorator;
        Optional<typename Dec::LabelType> label;
        size_t index;
        size_t incomingEdgeCount;
        size_t outgoingEdgeCount;
        
        explicit PartialVertexInfoImpl(const size_t i) : index(i), incomingEdgeCount(0), outgoingEdgeCount(0) {}
        
        [[nodiscard]] bool fullyPopulated() const {
            return label.occupied();
        }
    };

    template <typename G>
    struct PartialVertexInfoImpl<false, true, G> {
        using Dec = typename G::VertexDecorator;
        Optional<typename Dec::ColorType> color;
        size_t index;
        size_t incomingEdgeCount;
        size_t outgoingEdgeCount;
        
        explicit PartialVertexInfoImpl(const size_t i) : index(i), incomingEdgeCount(0), outgoingEdgeCount(0) {}
        
        [[nodiscard]] bool fullyPopulated() const {
            return color.occupied();
        }
    };

    template <typename G>
    struct PartialVertexInfoImpl<false, false, G> {
        size_t index;
        size_t incomingEdgeCount;
        size_t outgoingEdgeCount;
        
        explicit PartialVertexInfoImpl(const size_t i) : index(i), incomingEdgeCount(0), outgoingEdgeCount(0) {}
        
        [[nodiscard]] bool fullyPopulated() const {
            return true;
        }
    };

    template<typename Graph>
    using PartialVertexInfo = PartialVertexInfoImpl<Graph::VertexDecorator::is_labeled, Graph::VertexDecorator::is_colored, Graph>;

    template <bool HasLabel, bool HasWeight, typename G>
    struct PartialEdgeInfoImpl{};

    template <typename G>
    struct PartialEdgeInfoImpl<true, true, G> {
        using Dec = typename G::EdgeDecorator;
        Optional<typename Dec::LabelType> label;
        Optional<typename Dec::WeightType> weight;
        size_t fromVertexId;
        size_t toVertexId;
        size_t index;
        
        explicit PartialEdgeInfoImpl(const size_t i, const size_t from, const size_t to) : fromVertexId(from), toVertexId(to), index(i) {}
        
        [[nodiscard]] bool fullyPopulated() const {
            return label.occupied() && weight.occupied();
        }
    };

    template <typename G>
    struct PartialEdgeInfoImpl<true, false, G> {
        using Dec = typename G::EdgeDecorator;
        Optional<typename Dec::LabelType> label;
        size_t fromVertexId;
        size_t toVertexId;
        size_t index;
        
        explicit PartialEdgeInfoImpl(const size_t i, const size_t from, const size_t to) : fromVertexId(from), toVertexId(to), index(i) {}
        
        [[nodiscard]] bool fullyPopulated() const {
            return label.occupied();
        }
    };

    template <typename G>
    struct PartialEdgeInfoImpl<false, true, G> {
        using Dec = typename G::EdgeDecorator;
        Optional<typename Dec::WeightType> weight;
        size_t fromVertexId;
        size_t toVertexId;
        size_t index;
        
        explicit PartialEdgeInfoImpl(const size_t i, const size_t from, const size_t to) : fromVertexId(from), toVertexId(to), index(i) {}
        
        [[nodiscard]] bool fullyPopulated() const {
            return weight.occupied();
        }
    };

    template <typename G>
    struct PartialEdgeInfoImpl<false, false, G> {
        size_t fromVertexId;
        size_t toVertexId;
        size_t index;
        
        explicit PartialEdgeInfoImpl(const size_t i, const size_t from, const size_t to) : fromVertexId(from), toVertexId(to), index(i) {}
        
        [[nodiscard]] bool fullyPopulated() const {
            return true;
        }
    };

    template<typename Graph>
    using PartialEdgeInfo = PartialEdgeInfoImpl<Graph::EdgeDecorator::is_labeled, Graph::EdgeDecorator::is_weighted, Graph>;

    template <typename Graph>
    class GraphBuilderImpl {
    protected:
        //The index in the PartialVertexInfo must match its index in this vector
        Vector<PartialVertexInfo<Graph>> vertexInfo;
        Vector<PartialEdgeInfo<Graph>> edgeInfo;

        PartialVertexInfo<Graph>& createVertex() {
            vertexInfo.push(PartialVertexInfo<Graph>(vertexInfo.getSize()));
            return vertexInfo[vertexInfo.getSize() - 1];
        }

        PartialEdgeInfo<Graph>& createEdge(PartialVertexInfo<Graph>& source, PartialVertexInfo<Graph>& target) {
            PartialEdgeInfo<Graph> edge(edgeInfo.getSize(), source.index, target.index);
            edgeInfo.push(move(edge));
            ++source.outgoingEdgeCount;
            ++target.incomingEdgeCount;
            return edgeInfo[edgeInfo.getSize() - 1];
        }

        //TODO create ErrorOr abstraction a la SerenityOS to give more useful failure info
        Optional<Graph> buildGraph() {
            //Confirm all information to build the graph is complete
            for (auto vinfo : vertexInfo) {
                if (!vinfo.fullyPopulated()) {
                    return {};
                }
            }
            for (auto einfo : edgeInfo) {
                if (!einfo.fullyPopulated()) {
                    return {};
                }
            }

            // Build up the buffers/sets of vertex labels/colors, edge labels/weights as needed
            using VertexDecorator = typename Graph::VertexDecorator;
            using EdgeDecorator = typename Graph::EdgeDecorator;
            using StructureModifier = typename Graph::StructureModifier;
            using VertexIndex = typename Graph::VertexIndex;
            using EdgeIndex = typename Graph::EdgeIndex;

            SharedPtr<ImmutableIndexedHashSet<typename VertexDecorator::LabelType>> vertexLabels;
            SharedPtr<ImmutableIndexedHashSet<typename EdgeDecorator::LabelType>> edgeLabels;

            if constexpr (VertexDecorator::is_labeled) {
                HashSet<typename VertexDecorator::LabelType> labelSet;
                for (const auto& vinfo : vertexInfo) {
                    labelSet.insert(*(vinfo.label));
                }
                vertexLabels = make_shared<ImmutableIndexedHashSet<typename VertexDecorator::LabelType>>(move(labelSet));
            }
            if constexpr (EdgeDecorator::is_labeled) {
                HashSet<typename EdgeDecorator::LabelType> labelSet;
                for (const auto& einfo : edgeInfo) {
                    labelSet.insert(*(einfo.label));
                }
                edgeLabels = make_shared<ImmutableIndexedHashSet<typename EdgeDecorator::LabelType>>(move(labelSet));
            }

            // Create mappings from internal IDs for edge/vertex info to edge/vertex IDs to be used by the graph
            VertexIndex* vertexIdMap = new VertexIndex[vertexInfo.getSize()];
            EdgeIndex* edgeIdMap = new EdgeIndex[edgeInfo.getSize()];

            size_t vertexMetadataSize = 0;
            size_t edgeMetadataSize = 0;

            if constexpr (VertexDecorator::is_labeled) {
                for (size_t i = 0; i < vertexInfo.getSize(); i++) {
                    auto index = *vertexLabels->indexOf(*vertexInfo[i].label);
                    vertexIdMap[i] = index;
                    vertexMetadataSize = max(vertexMetadataSize, vertexIdMap[i] + 1);
                }
            } else {
                for (size_t i = 0; i < vertexInfo.getSize(); i++) {
                    vertexIdMap[i] = i;  // Identity mapping
                }
                vertexMetadataSize = vertexInfo.getSize();
            }

            if constexpr (EdgeDecorator::is_labeled) {
                for (size_t i = 0; i < edgeInfo.getSize(); i++) {
                    edgeIdMap[i] = *edgeLabels->indexOf(*edgeInfo[i].label);
                    edgeMetadataSize = max(edgeMetadataSize, edgeIdMap[i] + 1);
                }
            } else {
                for (size_t i = 0; i < edgeInfo.getSize(); i++) {
                    edgeIdMap[i] = i;  // Identity mapping
                }
                edgeMetadataSize = edgeInfo.getSize();
            }


            SharedPtr<typename VertexDecorator::ColorType[]> vertexColors;
            SharedPtr<typename EdgeDecorator::WeightType[]> edgeWeights;
            //The indices that we populate here need to be based on the edge/vertex IDs as given in the ID maps above
            if constexpr (EdgeDecorator::is_weighted) {
                edgeWeights = SharedPtr<typename EdgeDecorator::WeightType[]>::make(edgeMetadataSize);
                for (size_t i = 0; i < edgeInfo.getSize(); i++) {
                    EdgeIndex graphEdgeId = edgeIdMap[i];
                    edgeWeights[graphEdgeId] = *edgeInfo[i].weight;
                }
            }
            if constexpr (VertexDecorator::is_colored) {
                vertexColors = SharedPtr<typename VertexDecorator::ColorType[]>::make(vertexMetadataSize);
                for (size_t i = 0; i < vertexInfo.getSize(); i++) {
                    VertexIndex graphVertexId = vertexIdMap[i];
                    vertexColors[graphVertexId] = *vertexInfo[i].color;
                }
            }

            // Create the metadata buffers for vertices/edges
            using VertexMetadata = typename Graph::VertexMetadata;
            using EdgeMetadata = typename Graph::EdgeMetadata;

            // Create metadata arrays using SharedPtr array specialization
            SharedPtr<VertexMetadata[]> vertexMetadata = SharedPtr<VertexMetadata[]>::make(vertexMetadataSize);
            SharedPtr<EdgeMetadata[]> edgeMetadata = SharedPtr<EdgeMetadata[]>::make(edgeMetadataSize);

            // Populate edge metadata using graph IDs
            for (size_t i = 0; i < edgeInfo.getSize(); i++) {
                EdgeIndex graphEdgeId = edgeIdMap[i];
                edgeMetadata[graphEdgeId].from = vertexIdMap[edgeInfo[i].fromVertexId];
                edgeMetadata[graphEdgeId].to = vertexIdMap[edgeInfo[i].toVertexId];
            }

            // Calculate vertex metadata (incidence list spans) using graph IDs
            // First, count edges per vertex using graph vertex IDs
            size_t* outgoingCounts = new size_t[vertexMetadataSize]();
            size_t* incomingCounts = new size_t[vertexMetadataSize]();

            for (size_t i = 0; i < vertexInfo.getSize(); i++) {
                VertexIndex graphVertexId = vertexIdMap[i];
                outgoingCounts[graphVertexId] = vertexInfo[i].outgoingEdgeCount;
                incomingCounts[graphVertexId] = vertexInfo[i].incomingEdgeCount;
            }

            // Calculate incidence list offsets
            size_t currentIncidenceOffset = 0;
            // Iterate through actual vertices that exist, using their graph IDs
            for (size_t i = 0; i < vertexInfo.getSize(); i++) {
                VertexIndex vertexId = vertexIdMap[i];  // Get the actual graph vertex ID
                if constexpr (StructureModifier::is_directed) {
                    vertexMetadata[vertexId].start = currentIncidenceOffset;
                    vertexMetadata[vertexId].fromSize = outgoingCounts[vertexId];
                    vertexMetadata[vertexId].toSize = incomingCounts[vertexId];
                    currentIncidenceOffset += outgoingCounts[vertexId] + incomingCounts[vertexId];
                } else {
                    vertexMetadata[vertexId].start = currentIncidenceOffset;
                    vertexMetadata[vertexId].size = outgoingCounts[vertexId] + incomingCounts[vertexId];
                    currentIncidenceOffset += outgoingCounts[vertexId] + incomingCounts[vertexId];
                }
            }

            // Create the incidence list
            SharedPtr<EdgeIndex[]> incidenceLists = SharedPtr<EdgeIndex[]>::make(currentIncidenceOffset);

            size_t* outgoingOffsets = new size_t[vertexMetadataSize]();
            size_t* incomingOffsets = new size_t[vertexMetadataSize]();
            
            for (size_t i = 0; i < edgeInfo.getSize(); i++) {
                const auto& edge = edgeInfo[i];
                VertexIndex fromVertex = vertexIdMap[edge.fromVertexId];
                VertexIndex toVertex = vertexIdMap[edge.toVertexId];
                EdgeIndex graphEdgeId = edgeIdMap[i];
                
                // Fill outgoing edge for source vertex
                size_t outgoingOffset = vertexMetadata[fromVertex].fromStart() + outgoingOffsets[fromVertex]++;
                incidenceLists[outgoingOffset] = graphEdgeId;
                
                // Fill incoming edge for target vertex  
                if constexpr (StructureModifier::is_directed) {
                    size_t incomingOffset = vertexMetadata[toVertex].toStart() + incomingOffsets[toVertex]++;
                    incidenceLists[incomingOffset] = graphEdgeId;
                } else {
                    // For undirected graphs, also add to target vertex's incident list
                    // (since incoming and outgoing are the same for undirected)
                    size_t incidentOffset = vertexMetadata[toVertex].fromStart() + outgoingOffsets[toVertex]++;
                    incidenceLists[incidentOffset] = graphEdgeId;
                }
            }

            // Construct the Graph object using direct field assignment
            Graph graph;
            graph.vertexMetadata = move(vertexMetadata);
            graph.incidenceLists = move(incidenceLists);  
            graph.edgeMetadata = move(edgeMetadata);
            
            // Assign conditional fields
            if constexpr (VertexDecorator::is_colored) {
                graph.vertexColors = move(vertexColors);
            }
            if constexpr (VertexDecorator::is_labeled) {
                graph.vertexLabels = move(vertexLabels);
            } else {
                graph.vertexCount = vertexMetadataSize;
            }
            if constexpr (EdgeDecorator::is_weighted) {
                graph.edgeWeights = move(edgeWeights);
            }
            if constexpr (EdgeDecorator::is_labeled) {
                graph.edgeLabels = move(edgeLabels);
            } else {
                graph.edgeCount = edgeMetadataSize;
            }

            delete[] vertexIdMap;
            delete[] edgeIdMap;
            delete[] outgoingCounts;
            delete[] incomingCounts;
            delete[] outgoingOffsets;
            delete[] incomingOffsets;

            // Confirm that the Graph passes the predicate check as defined in its StructureModifier type
            if (!StructureModifier::check(graph)) {
                return {};
            }
            return graph;
        }
    public:

    };
}

#endif //GRAPHBUILDER_H
