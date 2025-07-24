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
#include <core/ds/HashMap.h>
#include <core/math.h>
#include <core/Iterator.h>

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
    public:
        // Opaque handles for vertices and edges during construction with validation
        class VertexHandle {
        private:
            size_t index;
            const Vector<PartialVertexInfo<Graph>>* builderVector; // Validates handle belongs to this builder
            friend class GraphBuilderImpl<Graph>;
            explicit VertexHandle(size_t i, const Vector<PartialVertexInfo<Graph>>* vector) 
                : index(i), builderVector(vector) {}
        public:
            bool operator==(const VertexHandle& other) const { 
                return index == other.index && builderVector == other.builderVector; 
            }
            bool operator!=(const VertexHandle& other) const { return !(*this == other); }
        };

        class EdgeHandle {
        private:
            size_t index;
            const Vector<PartialEdgeInfo<Graph>>* builderVector;
            friend class GraphBuilderImpl<Graph>;
            explicit EdgeHandle(size_t i, const Vector<PartialEdgeInfo<Graph>>* vector) 
                : index(i), builderVector(vector) {}
        public:
            bool operator==(const EdgeHandle& other) const { 
                return index == other.index && builderVector == other.builderVector; 
            }
            bool operator!=(const EdgeHandle& other) const { return !(*this == other); }
        };

        class UnpopulatedVertexIterator;
        class UnpopulatedEdgeIterator;

    protected:
        //The index in the PartialVertexInfo must match its index in this vector
        Vector<PartialVertexInfo<Graph>> vertexInfo;
        Vector<PartialEdgeInfo<Graph>> edgeInfo;
        
        // Optional HashMap for fast label lookups (only created if graph has labels)
        using VertexDecorator = typename Graph::VertexDecorator;
        using EdgeDecorator = typename Graph::EdgeDecorator;

        friend UnpopulatedVertexIterator;
        friend UnpopulatedEdgeIterator;

        struct None {};
        
        [[no_unique_address]]
        conditional_t<VertexDecorator::is_labeled,
                      HashMap<typename VertexDecorator::LabelType, size_t>,
                      None> vertexLabelMap;
        [[no_unique_address]]                      
        conditional_t<EdgeDecorator::is_labeled,
                      HashMap<typename EdgeDecorator::LabelType, size_t>,
                      None> edgeLabelMap;

        PartialVertexInfo<Graph>& createVertex() {
            vertexInfo.push(PartialVertexInfo<Graph>(vertexInfo.getSize()));
            PartialVertexInfo<Graph>& vertex = vertexInfo[vertexInfo.getSize() - 1];
            return vertex;
        }

        PartialEdgeInfo<Graph>& createEdge(PartialVertexInfo<Graph>& source, PartialVertexInfo<Graph>& target) {
            using StructureModifier = typename Graph::StructureModifier;
            
            // For simple graphs, check if edge already exists
            if constexpr (StructureModifier::is_simple_graph) {
                for (const auto& existingEdge : edgeInfo) {
                    if constexpr (StructureModifier::is_directed) {
                        // For directed simple graphs, check for exact match
                        if (existingEdge.fromVertexId == source.index && existingEdge.toVertexId == target.index) {
                            assert(false, "Duplicate edge in simple graph not allowed");
                        }
                    } else {
                        // For undirected simple graphs, check both directions
                        if ((existingEdge.fromVertexId == source.index && existingEdge.toVertexId == target.index) ||
                            (existingEdge.fromVertexId == target.index && existingEdge.toVertexId == source.index)) {
                            assert(false, "Duplicate edge in simple graph not allowed");
                        }
                    }
                }
            }

            PartialEdgeInfo<Graph> edge(edgeInfo.getSize(), source.index, target.index);
            edgeInfo.push(move(edge));
            ++source.outgoingEdgeCount;
            ++target.incomingEdgeCount;
            return edgeInfo[edgeInfo.getSize() - 1];
        }
        
        // Helper to validate vertex handle
        void validateVertexHandle(const VertexHandle& handle) const {
            assert(handle.builderVector == &vertexInfo, "Vertex handle must belong to this builder");
            assert(handle.index < vertexInfo.getSize(), "Vertex handle index out of bounds");
        }
        
        // Helper to validate edge handle
        void validateEdgeHandle(const EdgeHandle& handle) const {
            assert(handle.builderVector == &edgeInfo, "Edge handle must belong to this builder");
            assert(handle.index < edgeInfo.getSize(), "Edge handle index out of bounds");
        }

        static size_t getIndexForEdgeHandle(const EdgeHandle& h) {
            return h.index;
        }

        static size_t getIndexForVertexHandle(const VertexHandle& h) {
            return h.index;
        }

        // Private helper methods for setting vertex/edge properties
        
        // Set vertex label - checks for duplicates and updates label map
        template<typename T = typename VertexDecorator::LabelType>
        requires VertexDecorator::is_labeled
        bool _setVertexLabel(const VertexHandle& handle, const T& label) {
            validateVertexHandle(handle);
            if (vertexLabelMap.contains(label)) {
                return false; // Duplicate label
            }

            // Remove old label from map if vertex was already labeled
            auto& vertex = vertexInfo[handle.index];
            if (vertex.label.occupied()) {
                vertexLabelMap.remove(*vertex.label);
            }

            // Set new label and update map
            vertex.label = label;
            vertexLabelMap.insert(label, handle.index);
            return true;
        }
        
        // Set vertex color
        template<typename T = typename VertexDecorator::ColorType>
        requires VertexDecorator::is_colored
        void _setVertexColor(const VertexHandle& handle, const T& color) {
            validateVertexHandle(handle);
            vertexInfo[handle.index].color = color;
        }
        
        // Set edge label - checks for duplicates and updates label map
        template<typename T = typename EdgeDecorator::LabelType>
        requires EdgeDecorator::is_labeled
        bool _setEdgeLabel(const EdgeHandle& handle, const T& label) {
            validateEdgeHandle(handle);
            // Check for duplicate labels
            if (edgeLabelMap.contains(label)) {
                return false; // Duplicate label
            }

            // Remove old label from map if edge was already labeled
            auto& edge = edgeInfo[handle.index];
            if (edge.label.occupied()) {
                edgeLabelMap.remove(*edge.label);
            }

            // Set new label and update map
            edge.label = label;
            edgeLabelMap.insert(label, handle.index);
            return true;
        }
        
        // Set edge weight
        template<typename T = typename EdgeDecorator::WeightType>
        requires EdgeDecorator::is_weighted
        void _setEdgeWeight(const EdgeHandle& handle, const T& weight) {
            validateEdgeHandle(handle);
            edgeInfo[handle.index].weight = weight;
        }
        
        // Helper to check if vertex is fully populated
        bool _isVertexFullyPopulated(const VertexHandle& handle) const {
            validateVertexHandle(handle);
            return vertexInfo[handle.index].fullyPopulated();
        }
        
        // Helper to check if edge is fully populated
        bool _isEdgeFullyPopulated(const EdgeHandle& handle) const {
            validateEdgeHandle(handle);
            return edgeInfo[handle.index].fullyPopulated();
        }
        
        // Helper to clear vertex label (removes from map)
        template<typename T = typename VertexDecorator::LabelType>
        requires VertexDecorator::is_labeled
        void _clearVertexLabel(const VertexHandle& handle) {
            validateVertexHandle(handle);
            auto& vertex = vertexInfo[handle.index];
            if (vertex.label.occupied()) {
                vertexLabelMap.remove(*vertex.label);
                vertex.label = {};
            }
        }
        
        // Helper to clear edge label (removes from map)
        template<typename T = typename EdgeDecorator::LabelType>
        requires EdgeDecorator::is_labeled
        void _clearEdgeLabel(const EdgeHandle& handle) {
            validateEdgeHandle(handle);
            auto& edge = edgeInfo[handle.index];
            if (edge.label.occupied()) {
                edgeLabelMap.remove(*edge.label);
                edge.label = {};
            }
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
                if (labelSet.size() != vertexInfo.getSize()) {
                    return {}; // Duplicate labels
                }
                vertexLabels = make_shared<ImmutableIndexedHashSet<typename VertexDecorator::LabelType>>(move(labelSet));
            }
            if constexpr (EdgeDecorator::is_labeled) {
                HashSet<typename EdgeDecorator::LabelType> labelSet;
                for (const auto& einfo : edgeInfo) {
                    labelSet.insert(*(einfo.label));
                }
                if (labelSet.size() != edgeInfo.getSize()) {
                    return {}; // Duplicate labels
                }
                edgeLabels = make_shared<ImmutableIndexedHashSet<typename EdgeDecorator::LabelType>>(move(labelSet));
            }

            // Create mappings from internal IDs for edge/vertex info to edge/vertex IDs to be used by the graph
            auto vertexIdMap = UniquePtr<VertexIndex[]>::make(vertexInfo.getSize());
            auto edgeIdMap = UniquePtr<EdgeIndex[]>::make(edgeInfo.getSize());

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
            auto outgoingCounts = UniquePtr<size_t[]>::make(vertexMetadataSize);
            auto incomingCounts = UniquePtr<size_t[]>::make(vertexMetadataSize);
            
            // Initialize arrays to zero
            for (size_t i = 0; i < vertexMetadataSize; i++) {
                outgoingCounts[i] = 0;
                incomingCounts[i] = 0;
            }

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

            auto outgoingOffsets = UniquePtr<size_t[]>::make(vertexMetadataSize);
            auto incomingOffsets = UniquePtr<size_t[]>::make(vertexMetadataSize);
            
            // Initialize offset arrays to zero
            for (size_t i = 0; i < vertexMetadataSize; i++) {
                outgoingOffsets[i] = 0;
                incomingOffsets[i] = 0;
            }
            
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

            // UniquePtr arrays are automatically cleaned up here
            
            // Confirm that the Graph passes the predicate check as defined in its StructureModifier type
            if (!StructureModifier::check(graph)) {
                return {};
            }
            return graph;
        }

        VertexHandle getVertexHandle(size_t index) const {
            assert(index < vertexInfo.getSize(), "Vertex index out of bounds");
            return VertexHandle(index, &vertexInfo);
        }

        EdgeHandle getEdgeHandle(size_t index) const {
            assert(index < edgeInfo.getSize(), "Edge index out of bounds");
            return EdgeHandle(index, &edgeInfo);
        }
    public:

        // Basic state queries
        [[nodiscard]] size_t getCurrentVertexCount() const {
            return vertexInfo.getSize();
        }

        [[nodiscard]] size_t getCurrentEdgeCount() const {
            return edgeInfo.getSize();
        }

        // Check if an edge exists between two vertices
        [[nodiscard]] bool hasEdge(const VertexHandle& from, const VertexHandle& to) const {
            validateVertexHandle(from);
            validateVertexHandle(to);
            for (const auto& edge : edgeInfo) {
                if (edge.fromVertexId == from.index && edge.toVertexId == to.index) {
                    return true;
                }
            }
            return false;
        }

        // Get edge counts for specific vertices
        [[nodiscard]] size_t getOutgoingEdgeCount(const VertexHandle& vertex) const {
            validateVertexHandle(vertex);
            return vertexInfo[vertex.index].outgoingEdgeCount;
        }

        [[nodiscard]] size_t getIncomingEdgeCount(const VertexHandle& vertex) const {
            validateVertexHandle(vertex);
            return vertexInfo[vertex.index].incomingEdgeCount;
        }

        // Vertex label/color queries (only available if graph has these properties)
        template<typename T = typename VertexDecorator::LabelType>
        requires VertexDecorator::is_labeled
        Optional<T> getVertexLabel(const VertexHandle& vertex) const {
            validateVertexHandle(vertex);
            return vertexInfo[vertex.index].label;
        }

        template<typename T = typename VertexDecorator::ColorType>
        requires VertexDecorator::is_colored
        Optional<T> getVertexColor(const VertexHandle& vertex) const {
            validateVertexHandle(vertex);
            return vertexInfo[vertex.index].color;
        }

        // Edge label/weight queries (only available if graph has these properties)
        template<typename T = typename EdgeDecorator::LabelType>
        requires EdgeDecorator::is_labeled
        Optional<T> getEdgeLabel(const EdgeHandle& edge) const {
            validateEdgeHandle(edge);
            return edgeInfo[edge.index].label;
        }

        template<typename T = typename EdgeDecorator::WeightType>
        requires EdgeDecorator::is_weighted
        Optional<T> getEdgeWeight(const EdgeHandle& edge) const {
            validateEdgeHandle(edge);
            return edgeInfo[edge.index].weight;
        }

        // Get edge endpoints
        VertexHandle getEdgeSource(const EdgeHandle& edge) const {
            validateEdgeHandle(edge);
            return VertexHandle(edgeInfo[edge.index].fromVertexId, &vertexInfo);
        }

        VertexHandle getEdgeTarget(const EdgeHandle& edge) const {
            validateEdgeHandle(edge);
            return VertexHandle(edgeInfo[edge.index].toVertexId, &vertexInfo);
        }

        // Fast label-to-handle lookup (only available if graph has labels)
        template<typename T = typename VertexDecorator::LabelType>
        requires VertexDecorator::is_labeled
        Optional<VertexHandle> getVertexByLabel(const T& label) const {
            if (vertexLabelMap.contains(label)) {
                size_t index = vertexLabelMap.at(label);
                return VertexHandle(index, &vertexInfo);
            }
            return {};
        }

        template<typename T = typename EdgeDecorator::LabelType>
        requires EdgeDecorator::is_labeled
        Optional<EdgeHandle> getEdgeByLabel(const T& label) const {
            if (edgeLabelMap.contains(label)) {
                size_t index = edgeLabelMap.at(label);
                return EdgeHandle(index, &edgeInfo);
            }
            return {};
        }

        // Iterator access to current partial state
        class VertexIterator {
        private:
            size_t index;
            const Vector<PartialVertexInfo<Graph>>* vertexVector;
            friend GraphBuilderImpl;
            VertexIterator(size_t i, const Vector<PartialVertexInfo<Graph>>* v) : index(i), vertexVector(v) {}
        public:

            VertexHandle operator*() const { return VertexHandle(index, vertexVector); }
            VertexIterator& operator++() { ++index; return *this; }
            bool operator!=(const VertexIterator& other) const { return index != other.index; }
        };

        class EdgeIterator {
        private:
            size_t index;
            const Vector<PartialEdgeInfo<Graph>>* edgeVector;
            friend GraphBuilderImpl;
            EdgeIterator(size_t i, const Vector<PartialEdgeInfo<Graph>>* v) : index(i), edgeVector(v) {}
        public:
            EdgeHandle operator*() const { return EdgeHandle(index, edgeVector); }
            EdgeIterator& operator++() { ++index; return *this; }
            bool operator!=(const EdgeIterator& other) const { return index != other.index; }
        };

        IteratorRange<VertexIterator> currentVertices() const {
            return IteratorRange<VertexIterator>(
                VertexIterator(0, &vertexInfo),
                VertexIterator(vertexInfo.getSize(), &vertexInfo)
            );
        }

        IteratorRange<EdgeIterator> currentEdges() const {
            return IteratorRange<EdgeIterator>(
                EdgeIterator(0, &edgeInfo),
                EdgeIterator(edgeInfo.getSize(), &edgeInfo)
            );
        }

        // Validation iterators - find unpopulated vertices/edges
        class UnpopulatedVertexIterator {
        private:
            VertexIterator iter;
            VertexIterator endIter;
            const GraphBuilderImpl& builder;

            void advanceToUnpopulated() {
                while (iter != endIter) {
                    VertexHandle handle = *iter;
                    if (!builder.vertexInfo[handle.index].fullyPopulated()) {
                        return;
                    }
                    ++iter;
                }
            }

        public:
            UnpopulatedVertexIterator(VertexIterator begin, VertexIterator end, const GraphBuilderImpl& b)
                : iter(begin), endIter(end), builder(b) {
                advanceToUnpopulated();
            }

            VertexHandle operator*() const { return *iter; }
            UnpopulatedVertexIterator& operator++() { ++iter; advanceToUnpopulated(); return *this; }
            bool operator!=(const UnpopulatedVertexIterator& other) const { return iter != other.iter; }
        };

        class UnpopulatedEdgeIterator {
        private:
            EdgeIterator iter;
            EdgeIterator endIter;
            const GraphBuilderImpl& builder;

            void advanceToUnpopulated() {
                while (iter != endIter) {
                    EdgeHandle handle = *iter;
                    if (!builder.edgeInfo[handle.index].fullyPopulated()) {
                        return;
                    }
                    ++iter;
                }
            }



        public:
            UnpopulatedEdgeIterator(EdgeIterator begin, EdgeIterator end, const GraphBuilderImpl& b)
                : iter(begin), endIter(end), builder(b) {
                advanceToUnpopulated();
            }

            EdgeHandle operator*() const { return *iter; }
            UnpopulatedEdgeIterator& operator++() { ++iter; advanceToUnpopulated(); return *this; }
            bool operator!=(const UnpopulatedEdgeIterator& other) const { return iter != other.iter; }
        };

        IteratorRange<UnpopulatedVertexIterator> unpopulatedVertices() const {
            auto vertices = currentVertices();
            return IteratorRange<UnpopulatedVertexIterator>(
                UnpopulatedVertexIterator(vertices.begin(), vertices.end(), *this),
                UnpopulatedVertexIterator(vertices.end(), vertices.end(), *this)
            );
        }

        IteratorRange<UnpopulatedEdgeIterator> unpopulatedEdges() const {
            auto edges = currentEdges();
            return IteratorRange<UnpopulatedEdgeIterator>(
                UnpopulatedEdgeIterator(edges.begin(), edges.end(), *this),
                UnpopulatedEdgeIterator(edges.end(), edges.end(), *this)
            );
        }
    };
}

// Public GraphBuilder class - unrestricted graph construction interface
template<typename Graph>
class GraphBuilder : public _GraphBuilder::GraphBuilderImpl<Graph> {
private:
    using Base = _GraphBuilder::GraphBuilderImpl<Graph>;
    using VertexDecorator = typename Graph::VertexDecorator;
    using EdgeDecorator = typename Graph::EdgeDecorator;
    
public:
    using VertexHandle = typename Base::VertexHandle;
    using EdgeHandle = typename Base::EdgeHandle;
    
    // Vertex creation and management
    VertexHandle addVertex() {
        auto& vertex = this->createVertex();
        return this->getVertexHandle(vertex.index);
    }
    
    // Vertex property setters (only available if graph supports these properties)
    template<typename T = typename VertexDecorator::LabelType>
    requires VertexDecorator::is_labeled
    bool setVertexLabel(const VertexHandle& vertex, const T& label) {
        return this->_setVertexLabel(vertex, label);
    }
    
    template<typename T = typename VertexDecorator::ColorType>
    requires VertexDecorator::is_colored
    void setVertexColor(const VertexHandle& vertex, const T& color) {
        this->_setVertexColor(vertex, color);
    }
    
    void clearVertexLabel(const VertexHandle& vertex) 
    requires VertexDecorator::is_labeled {
        this->_clearVertexLabel(vertex);
    }
    
    // Edge creation and management
    EdgeHandle addEdge(const VertexHandle& from, const VertexHandle& to) {
        this->validateVertexHandle(from);
        this->validateVertexHandle(to);
        
        auto& fromVertex = this->vertexInfo[this -> getIndexForVertexHandle(from)];
        auto& toVertex = this->vertexInfo[this -> getIndexForVertexHandle(to)];
        auto& edge = this->createEdge(fromVertex, toVertex);
        return this->getEdgeHandle(edge.index);
    }
    
    // Edge property setters (only available if graph supports these properties)
    template<typename T = typename EdgeDecorator::LabelType>
    requires EdgeDecorator::is_labeled
    bool setEdgeLabel(const EdgeHandle& edge, const T& label) {
        return this->_setEdgeLabel(edge, label);
    }
    
    template<typename T = typename EdgeDecorator::WeightType>
    requires EdgeDecorator::is_weighted
    void setEdgeWeight(const EdgeHandle& edge, const T& weight) {
        this->_setEdgeWeight(edge, weight);
    }
    
    void clearEdgeLabel(const EdgeHandle& edge) 
    requires EdgeDecorator::is_labeled {
        this->_clearEdgeLabel(edge);
    }
    
    // Convenience methods for creating fully specified vertices/edges
    template<typename T = typename VertexDecorator::LabelType>
    requires VertexDecorator::is_labeled && (!VertexDecorator::is_colored)
    VertexHandle addVertex(const T& label) {
        auto vertex = addVertex();
        if (!setVertexLabel(vertex, label)) {
            // Handle duplicate label - could throw or return invalid handle
            assert(false, "Duplicate vertex label");
        }
        return vertex;
    }
    
    template<typename LabelT = typename VertexDecorator::LabelType, 
             typename ColorT = typename VertexDecorator::ColorType>
    requires VertexDecorator::is_labeled && VertexDecorator::is_colored
    VertexHandle addVertex(const LabelT& label, const ColorT& color) {
        auto vertex = addVertex();
        if (!setVertexLabel(vertex, label)) {
            assert(false, "Duplicate vertex label");
        }
        setVertexColor(vertex, color);
        return vertex;
    }
    
    template<typename ColorT = typename VertexDecorator::ColorType>
    requires (!VertexDecorator::is_labeled) && VertexDecorator::is_colored
    VertexHandle addVertex(const ColorT& color) {
        auto vertex = addVertex();
        setVertexColor(vertex, color);
        return vertex;
    }
    
    template<typename T = typename EdgeDecorator::LabelType>
    requires EdgeDecorator::is_labeled && (!EdgeDecorator::is_weighted)
    EdgeHandle addEdge(const VertexHandle& from, const VertexHandle& to, const T& label) {
        auto edge = addEdge(from, to);
        if (!setEdgeLabel(edge, label)) {
            assert(false, "Duplicate edge label");
        }
        return edge;
    }
    
    template<typename LabelT = typename EdgeDecorator::LabelType,
             typename WeightT = typename EdgeDecorator::WeightType>
    requires EdgeDecorator::is_labeled && EdgeDecorator::is_weighted
    EdgeHandle addEdge(const VertexHandle& from, const VertexHandle& to, 
                      const LabelT& label, const WeightT& weight) {
        auto edge = addEdge(from, to);
        if (!setEdgeLabel(edge, label)) {
            assert(false, "Duplicate edge label");
        }
        setEdgeWeight(edge, weight);
        return edge;
    }
    
    template<typename WeightT = typename EdgeDecorator::WeightType>
    requires (!EdgeDecorator::is_labeled) && EdgeDecorator::is_weighted
    EdgeHandle addEdge(const VertexHandle& from, const VertexHandle& to, const WeightT& weight) {
        auto edge = addEdge(from, to);
        setEdgeWeight(edge, weight);
        return edge;
    }
    
    // Validation and building
    bool isVertexFullyPopulated(const VertexHandle& vertex) const {
        return this->_isVertexFullyPopulated(vertex);
    }
    
    bool isEdgeFullyPopulated(const EdgeHandle& edge) const {
        return this->_isEdgeFullyPopulated(edge);
    }
    
    // Build final graph - consumes the builder
    Optional<Graph> build() {
        return this->buildGraph();
    }
    
    // Reset the builder to its initial state
    void reset() {
        // Clear vertex info and reallocate
        this->vertexInfo.~Vector();
        new(&this->vertexInfo) Vector<_GraphBuilder::PartialVertexInfo<Graph>>();
        
        // Clear edge info and reallocate  
        this->edgeInfo.~Vector();
        new(&this->edgeInfo) Vector<_GraphBuilder::PartialEdgeInfo<Graph>>();
        
        // Clear label maps if they exist
        if constexpr (VertexDecorator::is_labeled) {
            this->vertexLabelMap.~HashMap();
            new(&this->vertexLabelMap) HashMap<typename VertexDecorator::LabelType, size_t>();
        }
        if constexpr (EdgeDecorator::is_labeled) {
            this->edgeLabelMap.~HashMap();
            new(&this->edgeLabelMap) HashMap<typename EdgeDecorator::LabelType, size_t>();
        }
    }
    
    // Populate the builder from an existing graph
    void populateFromGraph(const Graph& graph) {
        // First reset to ensure clean state
        reset();
        
        // Create a mapping from graph vertex objects to builder vertex indices
        HashMap<typename Graph::Vertex, size_t> vertexToBuilderMap;
        
        // Add all vertices from the graph
        for (auto vertex : graph.vertices()) {
            auto& partialVertex = this->createVertex();
            size_t builderIndex = partialVertex.index;
            
            // Store the mapping from graph vertex to builder vertex
            vertexToBuilderMap.insert(vertex, builderIndex);
            
            // Set vertex properties if they exist
            if constexpr (VertexDecorator::is_labeled) {
                const auto& label = graph.getVertexLabel(vertex);
                auto builderHandle = this->getVertexHandle(builderIndex);
                this->_setVertexLabel(builderHandle, label);
            }
            if constexpr (VertexDecorator::is_colored) {
                const auto& color = graph.getVertexColor(vertex);
                auto builderHandle = this->getVertexHandle(builderIndex);
                this->_setVertexColor(builderHandle, color);
            }
        }
        
        // Add all edges from the graph
        for (auto edge : graph.edges()) {
            auto source = graph.getSource(edge);
            auto target = graph.getTarget(edge);
            
            // Find the corresponding builder vertices
            size_t sourceBuilderIndex = vertexToBuilderMap.at(source);
            size_t targetBuilderIndex = vertexToBuilderMap.at(target);
            
            auto& sourceVertex = this->vertexInfo[sourceBuilderIndex];
            auto& targetVertex = this->vertexInfo[targetBuilderIndex];
            
            // Create the edge
            auto& partialEdge = this->createEdge(sourceVertex, targetVertex);
            
            // Set edge properties if they exist
            if constexpr (EdgeDecorator::is_labeled) {
                const auto& label = graph.getEdgeLabel(edge);
                auto builderHandle = this->getEdgeHandle(partialEdge.index);
                this->_setEdgeLabel(builderHandle, label);
            }
            if constexpr (EdgeDecorator::is_weighted) {
                const auto& weight = graph.getEdgeWeight(edge);
                auto builderHandle = this->getEdgeHandle(partialEdge.index);
                this->_setEdgeWeight(builderHandle, weight);
            }
        }
    }
    
    // Inherit all query methods from base class
    using Base::getCurrentVertexCount;
    using Base::getCurrentEdgeCount;
    using Base::hasEdge;
    using Base::getOutgoingEdgeCount;
    using Base::getIncomingEdgeCount;
    using Base::getVertexLabel;
    using Base::getVertexColor;
    using Base::getEdgeLabel;
    using Base::getEdgeWeight;
    using Base::getEdgeSource;
    using Base::getEdgeTarget;
    using Base::getVertexByLabel;
    using Base::getEdgeByLabel;
    using Base::currentVertices;
    using Base::currentEdges;
    using Base::unpopulatedVertices;
    using Base::unpopulatedEdges;
};

// Type aliases to make constraint definitions easier
template<typename Graph>
using GraphBuilderBase = _GraphBuilder::GraphBuilderImpl<Graph>;

template<typename Graph>
using BuilderVertexHandle = typename _GraphBuilder::GraphBuilderImpl<Graph>::VertexHandle;

// Concept for EdgeConstraint - defines methods that a constraint must implement  
template<typename T, typename Graph>
concept EdgeConstraint = requires(const T& constraint, const GraphBuilderBase<Graph>& builder,
                                 BuilderVertexHandle<Graph> from,
                                 BuilderVertexHandle<Graph> to) {
    // Check if an edge is allowed between two vertices
    { constraint.isEdgeAllowed(builder, from, to) } -> convertible_to<bool>;
    
    // Get valid edges originating from a vertex
    { constraint.validEdgesFrom(builder, from) } -> Iterable;
    
    // Get valid edges targeting a vertex
    { constraint.validEdgesTo(builder, to) } -> Iterable;
};

// RestrictedGraphBuilder - adds constraint-based edge validation to graph construction.
// Vertices and the constraint are set in the constructor and immutable through the lifetime of the RestrictedGraphBuilder,
// even between calls to reset()
template<typename Graph, typename Constraint>
requires EdgeConstraint<Constraint, Graph>
class RestrictedGraphBuilder : public _GraphBuilder::GraphBuilderImpl<Graph> {
public:
    using Base = _GraphBuilder::GraphBuilderImpl<Graph>;
    using VertexDecorator = typename Graph::VertexDecorator;
    using EdgeDecorator = typename Graph::EdgeDecorator;
    using VertexHandle = typename Base::VertexHandle;
    using EdgeHandle = typename Base::EdgeHandle;
    
private:
    // Immutable constraint and vertex configuration
    Constraint constraint;
    Vector<VertexHandle> immutableVertices;
    
    // Helper to populate vertices from various sources
    void populateVertices(size_t count) {
        immutableVertices.ensureRoom(count);
        for (size_t i = 0; i < count; i++) {
            auto& vertex = this->createVertex();
            immutableVertices.push(this->getVertexHandle(vertex.index));
        }
    }
    
    template<typename VertexContainer>
    void populateVerticesFromContainer(const VertexContainer& vertices) {
        // Pre-allocate space if we can determine the size
        if constexpr (requires { vertices.getSize(); }) {
            immutableVertices.ensureRoom(vertices.getSize());
        }
        
        for (const auto& vertexSpec : vertices) {
            auto& vertex = this->createVertex();
            auto handle = this->getVertexHandle(vertex.index);
            
            // Set vertex properties if provided
            if constexpr (VertexDecorator::is_labeled) {
                if constexpr (requires { vertexSpec.label; }) {
                    this->_setVertexLabel(handle, vertexSpec.label);
                } else if constexpr (requires { vertexSpec.getLabel(); }) {
                    this->_setVertexLabel(handle, vertexSpec.getLabel());
                }
                else if constexpr (!VertexDecorator::is_colored && convertible_to<decltype(vertexSpec), typename VertexDecorator::LabelType>) {
                    this->_setVertexLabel(handle, vertexSpec);
                }
                else{
                    static_assert(false, "Vertex label not provided");
                }
            }
            if constexpr (VertexDecorator::is_colored) {
                if constexpr (requires { vertexSpec.color; }) {
                    this->_setVertexColor(handle, vertexSpec.color);
                } else if constexpr (requires { vertexSpec.getColor(); }) {
                    this->_setVertexColor(handle, vertexSpec.getColor());
                }
                else if constexpr (!VertexDecorator::is_labeled && convertible_to<decltype(vertexSpec), typename VertexDecorator::ColorType>) {
                    this->_setVertexColor(handle, vertexSpec);
                }
                else{
                    static_assert(false, "Vertex label not provided");
                }
            }
            
            immutableVertices.push(handle);
        }
    }
    
public:
    // Constructor for a fixed number of plain vertices (only for graphs with unlabeled, uncolored vertices)
    explicit RestrictedGraphBuilder(size_t vertexCount, const Constraint& edgeConstraint)
    requires (!VertexDecorator::is_labeled) && (!VertexDecorator::is_colored)
        : constraint(edgeConstraint) {
        populateVertices(vertexCount);
    }
    
    // Constructor taking vertex specifications and constraint
    template<typename VertexContainer>
    explicit RestrictedGraphBuilder(const VertexContainer& vertices, const Constraint& edgeConstraint)
        : constraint(edgeConstraint) {
        populateVerticesFromContainer(vertices);
    }
    
    // Get immutable vertex by index
    VertexHandle getVertex(size_t index) const {
        assert(index < immutableVertices.getSize(), "Vertex index out of bounds");
        return immutableVertices[index];
    }
    
    // Get all vertices as an iterable
    const Vector<VertexHandle>& getVertices() const {
        return immutableVertices;
    }
    
    // Get the constraint
    const Constraint& getConstraint() const {
        return constraint;
    }
    
    // Constrained edge creation - uses stored constraint automatically
    Optional<EdgeHandle> addEdge(const VertexHandle& from, const VertexHandle& to) {
        this->validateVertexHandle(from);
        this->validateVertexHandle(to);
        
        // Check if edge is allowed by the stored constraint
        if (!constraint.isEdgeAllowed(*this, from, to)) {
            return {}; // Edge not allowed
        }
        
        auto& fromVertex = this->vertexInfo[this->getIndexForVertexHandle(from)];
        auto& toVertex = this->vertexInfo[this->getIndexForVertexHandle(to)];
        auto& edge = this->createEdge(fromVertex, toVertex);
        return this->getEdgeHandle(edge.index);
    }
    
    // Convenience methods for creating fully specified edges with automatic constraint checking
    template<typename T = typename EdgeDecorator::LabelType>
    requires EdgeDecorator::is_labeled && (!EdgeDecorator::is_weighted)
    Optional<EdgeHandle> addEdge(const VertexHandle& from, const VertexHandle& to, const T& label) {
        auto edge = addEdge(from, to);
        if (!edge.occupied()) {
            return {}; // Edge was not allowed by constraint
        }
        if (!this->_setEdgeLabel(*edge, label)) {
            assert(false, "Duplicate edge label");
        }
        return edge;
    }
    
    template<typename LabelT = typename EdgeDecorator::LabelType,
             typename WeightT = typename EdgeDecorator::WeightType>
    requires EdgeDecorator::is_labeled && EdgeDecorator::is_weighted
    Optional<EdgeHandle> addEdge(const VertexHandle& from, const VertexHandle& to, 
                                const LabelT& label, const WeightT& weight) {
        auto edge = addEdge(from, to);
        if (!edge.occupied()) {
            return {}; // Edge was not allowed by constraint
        }
        if (!this->_setEdgeLabel(*edge, label)) {
            assert(false, "Duplicate edge label");
        }
        this->_setEdgeWeight(*edge, weight);
        return edge;
    }
    
    template<typename WeightT = typename EdgeDecorator::WeightType>
    requires (!EdgeDecorator::is_labeled) && EdgeDecorator::is_weighted
    Optional<EdgeHandle> addEdge(const VertexHandle& from, const VertexHandle& to, const WeightT& weight) {
        auto edge = addEdge(from, to);
        if (!edge.occupied()) {
            return {}; // Edge was not allowed by constraint
        }
        this->_setEdgeWeight(*edge, weight);
        return edge;
    }
    
    // Edge property setters (only available if graph supports these properties)
    template<typename T = typename EdgeDecorator::LabelType>
    requires EdgeDecorator::is_labeled
    bool setEdgeLabel(const EdgeHandle& edge, const T& label) {
        return this->_setEdgeLabel(edge, label);
    }
    
    template<typename T = typename EdgeDecorator::WeightType>
    requires EdgeDecorator::is_weighted
    void setEdgeWeight(const EdgeHandle& edge, const T& weight) {
        this->_setEdgeWeight(edge, weight);
    }
    
    void clearEdgeLabel(const EdgeHandle& edge) 
    requires EdgeDecorator::is_labeled {
        this->_clearEdgeLabel(edge);
    }
    
    // Constraint-aware validation methods
    bool canAddEdge(const VertexHandle& from, const VertexHandle& to) const {
        this->validateVertexHandle(from);
        this->validateVertexHandle(to);
        return constraint.isEdgeAllowed(*this, from, to);
    }
    
    auto getValidEdgesFrom(const VertexHandle& vertex) const {
        this->validateVertexHandle(vertex);
        return constraint.validEdgesFrom(*this, vertex);
    }
    
    auto getValidEdgesTo(const VertexHandle& vertex) const {
        this->validateVertexHandle(vertex);
        return constraint.validEdgesTo(*this, vertex);
    }
    
    // Validation and building
    bool isEdgeFullyPopulated(const EdgeHandle& edge) const {
        return this->_isEdgeFullyPopulated(edge);
    }
    
    // Build final graph - consumes the builder
    Optional<Graph> build() {
        return this->buildGraph();
    }
    
    // Reset the builder to its initial state - preserves vertices and constraint
    void reset() {
        // Clear edge info and reallocate  
        this->edgeInfo.~Vector();
        new(&this->edgeInfo) Vector<_GraphBuilder::PartialEdgeInfo<Graph>>();
        
        // Clear edge label map if it exists
        if constexpr (EdgeDecorator::is_labeled) {
            this->edgeLabelMap.~HashMap();
            new(&this->edgeLabelMap) HashMap<typename EdgeDecorator::LabelType, size_t>();
        }
        
        // Reset edge counts for all vertices
        for (auto& vertex : this->vertexInfo) {
            vertex.incomingEdgeCount = 0;
            vertex.outgoingEdgeCount = 0;
        }
        
        // Note: vertices, vertex labels, and constraint are preserved
    }

    // Inherit all query methods from base class
    using Base::getCurrentVertexCount;
    using Base::getCurrentEdgeCount;
    using Base::hasEdge;
    using Base::getOutgoingEdgeCount;
    using Base::getIncomingEdgeCount;
    using Base::getVertexLabel;
    using Base::getVertexColor;
    using Base::getEdgeLabel;
    using Base::getEdgeWeight;
    using Base::getEdgeSource;
    using Base::getEdgeTarget;
    using Base::getVertexByLabel;
    using Base::getEdgeByLabel;
    using Base::currentVertices;
    using Base::currentEdges;
    using Base::unpopulatedVertices;
    using Base::unpopulatedEdges;
};

#endif //GRAPHBUILDER_H
