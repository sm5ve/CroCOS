//
// Comprehensive unit tests for Interrupt Graph Infrastructure
// Tests topology graphs, routing graphs, and all domain types
// Created by Spencer Martin on 8/26/25.
//

#define CROCOS_TESTING
#include "../test.h"
#include <TestHarness.h>
#include <cstdlib>

#include <arch/hal/InterruptGraphs.h>
#include <core/ds/Vector.h>
#include <core/ds/SmartPointer.h>

using namespace kernel::hal::interrupts;
using namespace CroCOSTest;

// ============================================================================
// Mock Domains for Testing
// ============================================================================

CRClass(MockEmitterDomain, public platform::InterruptDomain, public platform::InterruptEmitter) {
private:
    size_t emitterCount;
public:
    MockEmitterDomain(size_t count) : emitterCount(count) {}
    size_t getEmitterCount() override { return emitterCount; }
};

CRClass(MockReceiverDomain, public platform::InterruptDomain, public platform::InterruptReceiver) {
private:
    size_t receiverCount;
public:
    MockReceiverDomain(size_t count) : receiverCount(count) {}
    size_t getReceiverCount() override { return receiverCount; }
};

CRClass(MockFreeRoutableDomain, public platform::InterruptDomain, public platform::FreeRoutableDomain) {
private:
    size_t receiverCount;
    size_t emitterCount;
public:
    MockFreeRoutableDomain(size_t receivers, size_t emitters) 
        : receiverCount(receivers), emitterCount(emitters) {}
    
    size_t getReceiverCount() override { return receiverCount; }
    size_t getEmitterCount() override { return emitterCount; }
    
    bool routeInterrupt(size_t, size_t) override {
        return true; // Simple stub
    }
};

CRClass(MockContextIndependentRoutableDomain, public platform::InterruptDomain, public platform::ContextIndependentRoutableDomain) {
private:
    size_t receiverCount;
    size_t emitterCount;
    
public:
    MockContextIndependentRoutableDomain(size_t receivers, size_t emitters) 
        : receiverCount(receivers), emitterCount(emitters) {}
    
    size_t getReceiverCount() override { return receiverCount; }
    size_t getEmitterCount() override { return emitterCount; }
    
    bool routeInterrupt(size_t fromReceiver, size_t toEmitter) override {
        return isRoutingAllowed(fromReceiver, toEmitter);
    }
    
    bool isRoutingAllowed(size_t fromReceiver, size_t toEmitter) const override {
        if (fromReceiver >= receiverCount || toEmitter >= emitterCount) {
            return false;
        }
        // Simple rule: allow routing only when receiver index equals emitter index
        return fromReceiver == toEmitter;
    }
};

CRClass(MockContextDependentRoutableDomain, public platform::InterruptDomain, public platform::ContextDependentRoutableDomain) {
private:
    size_t receiverCount;
    size_t emitterCount;
    mutable bool allowAllForTesting;
    
public:
    MockContextDependentRoutableDomain(size_t receivers, size_t emitters) 
        : receiverCount(receivers), emitterCount(emitters), allowAllForTesting(false) {}
    
    void setAllowAllForTesting(bool allow) { allowAllForTesting = allow; }
    
    size_t getReceiverCount() override { return receiverCount; }
    size_t getEmitterCount() override { return emitterCount; }
    
    bool routeInterrupt(size_t, size_t) override {
        return true; // Simple stub
    }
    
    bool isRoutingAllowed(size_t fromReceiver, size_t toEmitter, const GraphBuilderBase<managed::RoutingGraph>& builder) const override {
        // Simple test implementation - could inspect builder for complex logic
        (void)builder; // Suppress unused parameter warning
        if (fromReceiver >= receiverCount || toEmitter >= emitterCount) {
            return false;
        }
        return allowAllForTesting;
    }
};

CRClass(MockFixedRoutingDomain, public platform::InterruptDomain, public platform::FixedRoutingDomain) {
private:
    size_t receiverCount;
    size_t emitterCount;
    Vector<size_t> fixedRouting; // [receiver] -> emitter
    
public:
    MockFixedRoutingDomain(size_t receivers, size_t emitters) 
        : receiverCount(receivers), emitterCount(emitters) {
        // Default: receiver i routes to emitter i % emitterCount
        for (size_t i = 0; i < receivers; i++) {
            fixedRouting.push(i % emitters);
        }
    }
    
    void setFixedRoute(size_t receiver, size_t emitter) {
        if (receiver < receiverCount && emitter < emitterCount) {
            fixedRouting[receiver] = emitter;
        }
    }
    
    size_t getReceiverCount() override { return receiverCount; }
    size_t getEmitterCount() override { return emitterCount; }
    
    size_t getEmitterFor(size_t receiver) const override {
        if (receiver >= receiverCount) {
            return 0; // Should not happen in normal operation
        }
        return fixedRouting[receiver];
    }
};

// Simple connector that maps outputs 1:1 to inputs
class MockSimpleConnector : public platform::DomainConnector {
public:
    MockSimpleConnector(SharedPtr<platform::InterruptDomain> src, SharedPtr<platform::InterruptDomain> tgt)
        : DomainConnector(src, tgt) {}
    
    Optional<platform::DomainInputIndex> fromOutput(platform::DomainOutputIndex output) const override {
        return Optional<platform::DomainInputIndex>(output);
    }
    
    Optional<platform::DomainOutputIndex> fromInput(platform::DomainInputIndex input) const override {
        return Optional<platform::DomainOutputIndex>(input);
    }
};

// Connector with custom mapping
class MockCustomConnector : public platform::DomainConnector {
private:
    Vector<Optional<size_t>> outputToInput;
    Vector<Optional<size_t>> inputToOutput;
    
public:
    MockCustomConnector(SharedPtr<platform::InterruptDomain> src, SharedPtr<platform::InterruptDomain> tgt)
        : DomainConnector(src, tgt) {}
    
    void addMapping(size_t output, size_t input) {
        // Resize vectors if needed
        while (outputToInput.getSize() <= output) {
            outputToInput.push(Optional<size_t>());
        }
        while (inputToOutput.getSize() <= input) {
            inputToOutput.push(Optional<size_t>());
        }
        
        outputToInput[output] = input;
        inputToOutput[input] = output;
    }
    
    Optional<platform::DomainInputIndex> fromOutput(platform::DomainOutputIndex output) const override {
        if (output < outputToInput.getSize()) {
            return outputToInput[output];
        }
        return Optional<platform::DomainInputIndex>();
    }
    
    Optional<platform::DomainOutputIndex> fromInput(platform::DomainInputIndex input) const override {
        if (input < inputToOutput.getSize()) {
            return inputToOutput[input];
        }
        return Optional<platform::DomainOutputIndex>();
    }
};

// ============================================================================
// Test Setup/Teardown
// ============================================================================

class InterruptGraphTestSetup {
public:
    InterruptGraphTestSetup() {
        topology::resetTopologyState();
    }
    
    ~InterruptGraphTestSetup() {
        topology::resetTopologyState();
    }
};

// ============================================================================
// Basic Topology Graph Tests
// ============================================================================

TEST(TopologyGraphDomainRegistration) {
    InterruptGraphTestSetup setup;
    
    auto emitter = make_shared<MockEmitterDomain>(3);
    auto receiver = make_shared<MockReceiverDomain>(2);
    
    topology::registerDomain(emitter);
    topology::registerDomain(receiver);
    
    auto& topologyGraph = topology::getTopologyGraph();
    ASSERT_TRUE(topologyGraph.occupied());
    
    // Should have 2 vertices
    size_t vertexCount = 0;
    for (auto vertex : topologyGraph->vertices()) {
        (void)vertex; // Suppress unused variable warning
        vertexCount++;
    }
    ASSERT_EQ(2u, vertexCount);
}

TEST(TopologyGraphConnectorRegistration) {
    InterruptGraphTestSetup setup;
    
    auto emitter = make_shared<MockEmitterDomain>(2);
    auto receiver = make_shared<MockReceiverDomain>(2);
    auto connector = make_shared<MockSimpleConnector>(emitter, receiver);
    
    topology::registerDomain(emitter);
    topology::registerDomain(receiver);
    topology::registerConnector(connector);
    
    auto& topologyGraph = topology::getTopologyGraph();
    ASSERT_TRUE(topologyGraph.occupied());
    
    // Should have 1 edge
    size_t edgeCount = 0;
    for (auto vertex : topologyGraph->vertices()) {
        for (auto edge : topologyGraph->outgoingEdges(vertex)) {
            (void)edge; // Suppress unused variable warning
            edgeCount++;
        }
    }
    ASSERT_EQ(1u, edgeCount);
}

// ============================================================================
// Routing Graph Builder Tests
// ============================================================================

TEST(RoutingGraphBuilderBasicVertexCreation) {
    InterruptGraphTestSetup setup;
    
    auto emitter = make_shared<MockEmitterDomain>(2);
    auto receiver = make_shared<MockReceiverDomain>(3);
    
    topology::registerDomain(emitter);
    topology::registerDomain(receiver);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Should have 5 vertices total (2 device + 3 input)
    ASSERT_EQ(5u, routingBuilder->getCurrentVertexCount());
    ASSERT_EQ(0u, routingBuilder->getCurrentEdgeCount());
}

TEST(RoutingGraphBuilderVertexLabels) {
    InterruptGraphTestSetup setup;
    
    auto emitter = make_shared<MockEmitterDomain>(1);
    auto receiver = make_shared<MockReceiverDomain>(1);
    
    topology::registerDomain(emitter);
    topology::registerDomain(receiver);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Check that we can find vertices by label
    auto deviceLabel = managed::RoutingNodeLabel(emitter, 0);
    auto inputLabel = managed::RoutingNodeLabel(receiver, 0);
    
    auto deviceVertex = routingBuilder->getVertexByLabel(deviceLabel);
    auto inputVertex = routingBuilder->getVertexByLabel(inputLabel);
    
    ASSERT_TRUE(deviceVertex.occupied());
    ASSERT_TRUE(inputVertex.occupied());
}

// ============================================================================
// Fixed Routing Domain Tests
// ============================================================================

TEST(FixedRoutingDomainPrebuiltEdges) {
    InterruptGraphTestSetup setup;
    
    auto fixedDomain = make_shared<MockFixedRoutingDomain>(2, 2);
    auto receiver = make_shared<MockReceiverDomain>(2);
    auto connector = make_shared<MockSimpleConnector>(fixedDomain, receiver);
    
    // Set up fixed routing: receiver 0 -> emitter 1, receiver 1 -> emitter 0
    fixedDomain->setFixedRoute(0, 1);
    fixedDomain->setFixedRoute(1, 0);
    
    topology::registerDomain(fixedDomain);
    topology::registerDomain(receiver);
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Fixed domain should have edges prebuilt
    auto sourceLabel0 = managed::RoutingNodeLabel(fixedDomain, 0);
    auto sourceLabel1 = managed::RoutingNodeLabel(fixedDomain, 1);
    
    auto sourceVertex0 = routingBuilder->getVertexByLabel(sourceLabel0);
    auto sourceVertex1 = routingBuilder->getVertexByLabel(sourceLabel1);
    
    ASSERT_TRUE(sourceVertex0.occupied());
    ASSERT_TRUE(sourceVertex1.occupied());
    
    // Should have exactly 2 prebuilt edges
    ASSERT_EQ(2u, routingBuilder->getCurrentEdgeCount());
    
    // Check edge counts for each source
    ASSERT_EQ(1u, routingBuilder->getOutgoingEdgeCount(*sourceVertex0));
    ASSERT_EQ(1u, routingBuilder->getOutgoingEdgeCount(*sourceVertex1));
}

TEST(FixedRoutingDomainConstraintBehavior) {
    InterruptGraphTestSetup setup;
    
    auto fixedDomain = make_shared<MockFixedRoutingDomain>(1, 2);
    auto receiver = make_shared<MockReceiverDomain>(2);
    auto connector = make_shared<MockSimpleConnector>(fixedDomain, receiver);
    
    // Fixed routing: receiver 0 -> emitter 1
    fixedDomain->setFixedRoute(0, 1);
    
    topology::registerDomain(fixedDomain);
    topology::registerDomain(receiver);
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    auto sourceLabel = managed::RoutingNodeLabel(fixedDomain, 0);
    auto targetLabel0 = managed::RoutingNodeLabel(receiver, 0);
    auto targetLabel1 = managed::RoutingNodeLabel(receiver, 1);
    
    auto sourceVertex = routingBuilder->getVertexByLabel(sourceLabel);
    auto targetVertex0 = routingBuilder->getVertexByLabel(targetLabel0);
    auto targetVertex1 = routingBuilder->getVertexByLabel(targetLabel1);
    
    ASSERT_TRUE(sourceVertex.occupied());
    ASSERT_TRUE(targetVertex0.occupied());
    ASSERT_TRUE(targetVertex1.occupied());
    
    // Should have prebuilt edge to target1 but not target0
    ASSERT_FALSE(routingBuilder->hasEdge(*sourceVertex, *targetVertex0));
    ASSERT_TRUE(routingBuilder->hasEdge(*sourceVertex, *targetVertex1));
}

// ============================================================================
// Free Routable Domain Tests
// ============================================================================

TEST(FreeRoutableDomainConstraints) {
    InterruptGraphTestSetup setup;
    
    auto freeDomain = make_shared<MockFreeRoutableDomain>(2, 2);
    auto receiver = make_shared<MockReceiverDomain>(2);
    auto connector = make_shared<MockSimpleConnector>(freeDomain, receiver);
    
    topology::registerDomain(freeDomain);
    topology::registerDomain(receiver);
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    auto sourceLabel0 = managed::RoutingNodeLabel(freeDomain, 0);
    auto sourceLabel1 = managed::RoutingNodeLabel(freeDomain, 1);
    auto targetLabel0 = managed::RoutingNodeLabel(receiver, 0);
    auto targetLabel1 = managed::RoutingNodeLabel(receiver, 1);
    
    auto sourceVertex0 = routingBuilder->getVertexByLabel(sourceLabel0);
    auto sourceVertex1 = routingBuilder->getVertexByLabel(sourceLabel1);
    auto targetVertex0 = routingBuilder->getVertexByLabel(targetLabel0);
    auto targetVertex1 = routingBuilder->getVertexByLabel(targetLabel1);
    
    // Free domain should allow all routing
    ASSERT_TRUE(routingBuilder->canAddEdge(*sourceVertex0, *targetVertex0));
    ASSERT_TRUE(routingBuilder->canAddEdge(*sourceVertex0, *targetVertex1));
    ASSERT_TRUE(routingBuilder->canAddEdge(*sourceVertex1, *targetVertex0));
    ASSERT_TRUE(routingBuilder->canAddEdge(*sourceVertex1, *targetVertex1));
}

// ============================================================================
// Context Independent Routable Domain Tests
// ============================================================================

TEST(ContextIndependentRoutableDomainConstraints) {
    InterruptGraphTestSetup setup;
    
    auto routableDomain = make_shared<MockContextIndependentRoutableDomain>(2, 2);
    auto receiver = make_shared<MockReceiverDomain>(2);
    auto connector = make_shared<MockSimpleConnector>(routableDomain, receiver);
    
    // Built-in logic: receiver0->emitter0, receiver1->emitter1 (receiver index == emitter index)
    
    topology::registerDomain(routableDomain);
    topology::registerDomain(receiver);
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    auto sourceLabel0 = managed::RoutingNodeLabel(routableDomain, 0);
    auto sourceLabel1 = managed::RoutingNodeLabel(routableDomain, 1);
    auto targetLabel0 = managed::RoutingNodeLabel(receiver, 0);
    auto targetLabel1 = managed::RoutingNodeLabel(receiver, 1);
    
    auto sourceVertex0 = routingBuilder->getVertexByLabel(sourceLabel0);
    auto sourceVertex1 = routingBuilder->getVertexByLabel(sourceLabel1);
    auto targetVertex0 = routingBuilder->getVertexByLabel(targetLabel0);
    auto targetVertex1 = routingBuilder->getVertexByLabel(targetLabel1);
    
    // Should allow only the specified routing
    ASSERT_TRUE(routingBuilder->canAddEdge(*sourceVertex0, *targetVertex0));
    ASSERT_FALSE(routingBuilder->canAddEdge(*sourceVertex0, *targetVertex1));
    ASSERT_FALSE(routingBuilder->canAddEdge(*sourceVertex1, *targetVertex0));
    ASSERT_TRUE(routingBuilder->canAddEdge(*sourceVertex1, *targetVertex1));
}

// ============================================================================
// Context Dependent Routable Domain Tests
// ============================================================================

TEST(ContextDependentRoutableDomainConstraints) {
    InterruptGraphTestSetup setup;
    
    auto routableDomain = make_shared<MockContextDependentRoutableDomain>(2, 2);
    auto receiver = make_shared<MockReceiverDomain>(2);
    auto connector = make_shared<MockSimpleConnector>(routableDomain, receiver);
    
    topology::registerDomain(routableDomain);
    topology::registerDomain(receiver);
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    auto sourceLabel0 = managed::RoutingNodeLabel(routableDomain, 0);
    auto targetLabel0 = managed::RoutingNodeLabel(receiver, 0);
    
    auto sourceVertex0 = routingBuilder->getVertexByLabel(sourceLabel0);
    auto targetVertex0 = routingBuilder->getVertexByLabel(targetLabel0);
    
    // Initially disallow
    routableDomain->setAllowAllForTesting(false);
    ASSERT_FALSE(routingBuilder->canAddEdge(*sourceVertex0, *targetVertex0));
    
    // Then allow
    routableDomain->setAllowAllForTesting(true);
    ASSERT_TRUE(routingBuilder->canAddEdge(*sourceVertex0, *targetVertex0));
}

// ============================================================================
// Device Domain Constraint Tests
// ============================================================================

TEST(DeviceDomainConstraints) {
    InterruptGraphTestSetup setup;
    
    auto device = make_shared<MockEmitterDomain>(2);
    auto receiver = make_shared<MockReceiverDomain>(3);
    auto connector = make_shared<MockCustomConnector>(device, receiver);
    
    // Map device output 0 -> receiver input 1, device output 1 -> receiver input 2
    connector->addMapping(0, 1);
    connector->addMapping(1, 2);
    
    topology::registerDomain(device);
    topology::registerDomain(receiver);
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    auto deviceLabel0 = managed::RoutingNodeLabel(device, 0);
    auto deviceLabel1 = managed::RoutingNodeLabel(device, 1);
    auto targetLabel0 = managed::RoutingNodeLabel(receiver, 0);
    auto targetLabel1 = managed::RoutingNodeLabel(receiver, 1);
    auto targetLabel2 = managed::RoutingNodeLabel(receiver, 2);
    
    auto deviceVertex0 = routingBuilder->getVertexByLabel(deviceLabel0);
    auto deviceVertex1 = routingBuilder->getVertexByLabel(deviceLabel1);
    auto targetVertex0 = routingBuilder->getVertexByLabel(targetLabel0);
    auto targetVertex1 = routingBuilder->getVertexByLabel(targetLabel1);
    auto targetVertex2 = routingBuilder->getVertexByLabel(targetLabel2);
    
    // Should only allow connections as mapped by connector
    ASSERT_FALSE(routingBuilder->canAddEdge(*deviceVertex0, *targetVertex0));
    ASSERT_FALSE(routingBuilder->canAddEdge(*deviceVertex0, *targetVertex2));
    
    ASSERT_FALSE(routingBuilder->canAddEdge(*deviceVertex1, *targetVertex0));
    ASSERT_FALSE(routingBuilder->canAddEdge(*deviceVertex1, *targetVertex1));
}

// ============================================================================
// Complex Multi-Domain Integration Tests
// ============================================================================

TEST(ComplexMultiDomainTopology) {
    InterruptGraphTestSetup setup;
    
    // Create a complex topology: Device -> Fixed -> Free -> Receiver
    auto device = make_shared<MockEmitterDomain>(2);
    auto fixedDomain = make_shared<MockFixedRoutingDomain>(2, 2);
    auto freeDomain = make_shared<MockFreeRoutableDomain>(2, 3);
    auto receiver = make_shared<MockReceiverDomain>(3);
    
    auto connector1 = make_shared<MockSimpleConnector>(device, fixedDomain);
    auto connector2 = make_shared<MockSimpleConnector>(fixedDomain, freeDomain);
    auto connector3 = make_shared<MockSimpleConnector>(freeDomain, receiver);
    
    topology::registerDomain(device);
    topology::registerDomain(fixedDomain);
    topology::registerDomain(freeDomain);
    topology::registerDomain(receiver);
    topology::registerConnector(connector1);
    topology::registerConnector(connector2);
    topology::registerConnector(connector3);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Should have vertices for all domains
    // Device: 2 emitter nodes
    // Fixed: 2 input nodes (receivers from device)
    // Free: 2 input nodes
    // Receiver: 3 input nodes
    // Total: 9 vertices
    ASSERT_EQ(9u, routingBuilder->getCurrentVertexCount());
    
    // Fixed domain should have 4 prebuilt edges
    ASSERT_EQ(4u, routingBuilder->getCurrentEdgeCount());
}

TEST(EdgeIterationValidEdgesFrom) {
    InterruptGraphTestSetup setup;
    
    auto device = make_shared<MockEmitterDomain>(1);
    auto freeDomain = make_shared<MockFreeRoutableDomain>(2, 2);
    auto receiver = make_shared<MockReceiverDomain>(2);
    
    auto connector1 = make_shared<MockSimpleConnector>(device, freeDomain);
    auto connector2 = make_shared<MockSimpleConnector>(freeDomain, receiver);
    
    topology::registerDomain(device);
    topology::registerDomain(freeDomain);
    topology::registerDomain(receiver);
    topology::registerConnector(connector1);
    topology::registerConnector(connector2);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Test iteration from free domain input
    auto sourceLabel = managed::RoutingNodeLabel(freeDomain, 0);
    auto sourceVertex = routingBuilder->getVertexByLabel(sourceLabel);
    ASSERT_TRUE(sourceVertex.occupied());
    
    auto validEdges = routingBuilder->getValidEdgesFrom(*sourceVertex);
    
    // Count valid edges
    size_t edgeCount = 0;
    for (auto targetVertex : validEdges) {
        (void)targetVertex; // Suppress unused variable warning
        edgeCount++;
    }
    
    // Free domain should be able to connect to both receiver inputs
    ASSERT_EQ(2u, edgeCount);
}

// ============================================================================
// Edge Addition and Routing Tests
// ============================================================================

TEST(ActualEdgeAddition) {
    InterruptGraphTestSetup setup;
    
    auto freeDomain = make_shared<MockFreeRoutableDomain>(1, 1);
    auto receiver = make_shared<MockReceiverDomain>(1);
    auto connector = make_shared<MockSimpleConnector>(freeDomain, receiver);
    
    topology::registerDomain(freeDomain);
    topology::registerDomain(receiver);
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    auto sourceLabel = managed::RoutingNodeLabel(freeDomain, 0);
    auto targetLabel = managed::RoutingNodeLabel(receiver, 0);
    
    auto sourceVertex = routingBuilder->getVertexByLabel(sourceLabel);
    auto targetVertex = routingBuilder->getVertexByLabel(targetLabel);
    
    ASSERT_TRUE(sourceVertex.occupied());
    ASSERT_TRUE(targetVertex.occupied());
    
    // Should be able to add edge
    ASSERT_TRUE(routingBuilder->canAddEdge(*sourceVertex, *targetVertex));
    
    routingBuilder->addEdge(*sourceVertex, *targetVertex);
    
    // Should now have the edge
    ASSERT_TRUE(routingBuilder->hasEdge(*sourceVertex, *targetVertex));
    ASSERT_EQ(1u, routingBuilder->getCurrentEdgeCount());
}

TEST(MultipleConcurrentEdges) {
    InterruptGraphTestSetup setup;
    
    auto freeDomain = make_shared<MockFreeRoutableDomain>(2, 2);
    auto receiver = make_shared<MockReceiverDomain>(2);
    auto connector = make_shared<MockSimpleConnector>(freeDomain, receiver);
    
    topology::registerDomain(freeDomain);
    topology::registerDomain(receiver);
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Add multiple edges
    auto source0 = *routingBuilder->getVertexByLabel(managed::RoutingNodeLabel(freeDomain, 0));
    auto source1 = *routingBuilder->getVertexByLabel(managed::RoutingNodeLabel(freeDomain, 1));
    auto target0 = *routingBuilder->getVertexByLabel(managed::RoutingNodeLabel(receiver, 0));
    auto target1 = *routingBuilder->getVertexByLabel(managed::RoutingNodeLabel(receiver, 1));
    
    routingBuilder->addEdge(source0, target0);
    routingBuilder->addEdge(source1, target1);
    
    ASSERT_EQ(2u, routingBuilder->getCurrentEdgeCount());
    ASSERT_TRUE(routingBuilder->hasEdge(source0, target0));
    ASSERT_TRUE(routingBuilder->hasEdge(source1, target1));
}

// ============================================================================
// Error Condition Tests
// ============================================================================

TEST(InvalidDomainConnection) {
    InterruptGraphTestSetup setup;
    
    auto emitter = make_shared<MockEmitterDomain>(1);
    auto receiver = make_shared<MockReceiverDomain>(1);
    
    topology::registerDomain(emitter);
    topology::registerDomain(receiver);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    auto deviceLabel = managed::RoutingNodeLabel(emitter, 0);
    auto inputLabel = managed::RoutingNodeLabel(receiver, 0);
    
    auto deviceVertex = routingBuilder->getVertexByLabel(deviceLabel);
    auto inputVertex = routingBuilder->getVertexByLabel(inputLabel);
    
    ASSERT_TRUE(deviceVertex.occupied());
    ASSERT_TRUE(inputVertex.occupied());
    
    // Should not be able to add edge without topology connection
    ASSERT_FALSE(routingBuilder->canAddEdge(*deviceVertex, *inputVertex));
}