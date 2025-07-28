//
// Unit tests for Interrupt Graph Infrastructure
// Created by Spencer Martin on 7/27/25.
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

// Mock domains for testing
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

CRClass(MockRoutableDomain, public platform::InterruptDomain, public platform::RoutableDomain) {
private:
    size_t receiverCount;
    size_t emitterCount;
public:
    MockRoutableDomain(size_t receivers, size_t emitters) 
        : receiverCount(receivers), emitterCount(emitters) {
    }
    
    size_t getReceiverCount() override { return receiverCount; }
    size_t getEmitterCount() override { return emitterCount; }
    
    bool routeInterrupt(size_t, size_t) override {
        return true; // Simple stub
    }
};

class MockDomainConnector : public platform::DomainConnector {
private:
    Vector<Optional<platform::DomainInputIndex>> outputToInputMap;
    Vector<Optional<platform::DomainOutputIndex>> inputToOutputMap;
    
public:
    MockDomainConnector(SharedPtr<platform::InterruptDomain> src, 
                       SharedPtr<platform::InterruptDomain> tgt,
                       size_t sourceOutputs, size_t targetInputs)
        : platform::DomainConnector(src, tgt) {
        outputToInputMap.ensureRoom(sourceOutputs);
        inputToOutputMap.ensureRoom(targetInputs);
        for (size_t i = 0; i < sourceOutputs; i++) {
            outputToInputMap.push({});
        }
        for (size_t i = 0; i < targetInputs; i++) {
            inputToOutputMap.push({});
        }
    }
    
    void setMapping(platform::DomainOutputIndex output, platform::DomainInputIndex input) {
        if (output < outputToInputMap.getSize() && input < inputToOutputMap.getSize()) {
            outputToInputMap[output] = input;
            inputToOutputMap[input] = output;
        }
    }
    
    Optional<platform::DomainInputIndex> fromOutput(platform::DomainOutputIndex output) override {
        if (output >= outputToInputMap.getSize()) {
            return {};
        }
        return outputToInputMap[output];
    }
    
    Optional<platform::DomainOutputIndex> fromInput(platform::DomainInputIndex input) override {
        if (input >= inputToOutputMap.getSize()) {
            return {};
        }
        return inputToOutputMap[input];
    }
};

TEST(TopologyConnectorRegistration) {
    topology::resetTopologyState();
    
    auto emitter = make_shared<MockEmitterDomain>(2);
    auto receiver = make_shared<MockReceiverDomain>(3);
    
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(emitter));
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(receiver));
    
    // Comment out the connector part to isolate the vertex-only case
    auto connector = make_shared<MockDomainConnector>(emitter, receiver, 2, 3);
    connector->setMapping(0, 1); // Output 0 -> Input 1
    connector->setMapping(1, 2); // Output 1 -> Input 2
    
    topology::registerConnector(connector);
    
    auto graph = topology::getTopologyGraph();
    ASSERT_TRUE(graph.occupied());
    
    // Should have one edge
    size_t edgeCount = 0;
    for (auto vertex : graph->vertices()) {
        for (auto edge : graph->outgoingEdges(vertex)) {
            edgeCount++;
        }
    }
    ASSERT_EQ(edgeCount, 1u);
}

TEST(TopologyComplexGraph) {
    topology::resetTopologyState();
    
    // Create a more complex topology: Device -> Controller -> CPU
    auto device = make_shared<MockEmitterDomain>(4);
    auto controller = make_shared<MockRoutableDomain>(4, 2);
    auto cpu = make_shared<MockReceiverDomain>(2);
    
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(device));
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(controller));
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(cpu));
    
    // Device -> Controller
    auto connector1 = make_shared<MockDomainConnector>(device, controller, 4, 4);
    for (size_t i = 0; i < 4; i++) {
        connector1->setMapping(i, i); // 1:1 mapping
    }
    
    // Controller -> CPU
    auto connector2 = make_shared<MockDomainConnector>(controller, cpu, 2, 2);
    connector2->setMapping(0, 0);
    connector2->setMapping(1, 1);
    
    topology::registerConnector(connector1);
    topology::registerConnector(connector2);
    
    auto graph = topology::getTopologyGraph();
    ASSERT_TRUE(graph.occupied());
    
    // Verify graph structure
    size_t vertexCount = 0;
    size_t edgeCount = 0;
    for (auto vertex : graph->vertices()) {
        vertexCount++;
        for (auto edge : graph->outgoingEdges(vertex)) {
            edgeCount++;
        }
    }
    ASSERT_EQ(vertexCount, 3u);
    ASSERT_EQ(edgeCount, 2u);
}

// Test routing graph creation
TEST(RoutingGraphBasicCreation) {
    topology::resetTopologyState();
    
    auto emitter = make_shared<MockEmitterDomain>(2);
    auto receiver = make_shared<MockReceiverDomain>(3);
    
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(emitter));
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(receiver));
    
    auto connector = make_shared<MockDomainConnector>(emitter, receiver, 2, 3);
    connector->setMapping(0, 1);
    connector->setMapping(1, 2);
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Should create vertices for all device outputs and receiver inputs
    // Device has 2 outputs (NodeType::Device)
    // Receiver has 3 inputs (NodeType::Input)
    // Total: 5 vertices
    
    auto graph = routingBuilder->build();
    ASSERT_TRUE(graph.occupied());
    
    size_t vertexCount = 0;
    for (auto vertex : graph->vertices()) {
        vertexCount++;
    }
    ASSERT_EQ(vertexCount, 5u);
}

TEST(RoutingGraphConstraintValidation) {
    topology::resetTopologyState();
    
    auto emitter = make_shared<MockEmitterDomain>(2);
    auto receiver = make_shared<MockReceiverDomain>(3);
    
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(emitter));
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(receiver));
    
    auto connector = make_shared<MockDomainConnector>(emitter, receiver, 2, 3);
    connector->setMapping(0, 1); // Output 0 -> Input 1
    connector->setMapping(1, 2); // Output 1 -> Input 2
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Find device output vertices and receiver input vertices
    managed::RoutingNodeLabel deviceOutput0(emitter, 0, managed::NodeType::Device);
    managed::RoutingNodeLabel deviceOutput1(emitter, 1, managed::NodeType::Device);
    managed::RoutingNodeLabel receiverInput1(receiver, 1, managed::NodeType::Input);
    managed::RoutingNodeLabel receiverInput2(receiver, 2, managed::NodeType::Input);
    managed::RoutingNodeLabel receiverInput0(receiver, 0, managed::NodeType::Input);
    
    auto output0 = routingBuilder->getVertexByLabel(deviceOutput0);
    auto output1 = routingBuilder->getVertexByLabel(deviceOutput1);
    auto input0 = routingBuilder->getVertexByLabel(receiverInput0);
    auto input1 = routingBuilder->getVertexByLabel(receiverInput1);
    auto input2 = routingBuilder->getVertexByLabel(receiverInput2);
    
    ASSERT_TRUE(output0.occupied());
    ASSERT_TRUE(output1.occupied());
    ASSERT_TRUE(input0.occupied());
    ASSERT_TRUE(input1.occupied());
    ASSERT_TRUE(input2.occupied());
    
    // Test valid connections based on connector mapping
    ASSERT_TRUE(routingBuilder->canAddEdge(*output0, *input1)); // 0->1 mapped
    ASSERT_TRUE(routingBuilder->canAddEdge(*output1, *input2)); // 1->2 mapped
    
    // Test invalid connections
    ASSERT_FALSE(routingBuilder->canAddEdge(*output0, *input0)); // 0->0 not mapped
    ASSERT_FALSE(routingBuilder->canAddEdge(*output0, *input2)); // 0->2 not mapped
    ASSERT_FALSE(routingBuilder->canAddEdge(*output1, *input0)); // 1->0 not mapped
    ASSERT_FALSE(routingBuilder->canAddEdge(*output1, *input1)); // 1->1 not mapped
}

TEST(RoutingGraphMultiDomainRouting) {
    topology::resetTopologyState();
    
    // Test with a controller that can route between inputs and outputs
    auto device = make_shared<MockEmitterDomain>(2);
    auto controller = make_shared<MockRoutableDomain>(2, 2);
    auto cpu = make_shared<MockReceiverDomain>(2);
    
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(device));
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(controller));
    topology::registerDomain(static_pointer_cast<platform::InterruptDomain>(cpu));
    
    // Device -> Controller (1:1 mapping)
    auto connector1 = make_shared<MockDomainConnector>(device, controller, 2, 2);
    connector1->setMapping(0, 0);
    connector1->setMapping(1, 1);
    
    // Controller -> CPU (1:1 mapping)
    auto connector2 = make_shared<MockDomainConnector>(controller, cpu, 2, 2);
    connector2->setMapping(0, 0);
    connector2->setMapping(1, 1);
    
    topology::registerConnector(connector1);
    topology::registerConnector(connector2);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Should create vertices for:
    // - Device outputs: 2 (NodeType::Device)
    // - Controller inputs: 2 (NodeType::Input) 
    // - CPU inputs: 2 (NodeType::Input)
    // Total: 6 vertices
    
    auto graph = routingBuilder->build();
    ASSERT_TRUE(graph.occupied());
    
    size_t vertexCount = 0;
    for (auto vertex : graph->vertices()) {
        vertexCount++;
    }
    ASSERT_EQ(vertexCount, 6u);
}

TEST(PotentialEdgeIteratorForward) {
    topology::resetTopologyState();
    
    auto device = make_shared<MockEmitterDomain>(2);
    auto controller = make_shared<MockRoutableDomain>(3, 2);
    
    topology::registerDomain(device);
    topology::registerDomain(controller);
    
    auto connector = make_shared<MockDomainConnector>(device, controller, 2, 3);
    connector->setMapping(0, 1); // Device output 0 -> Controller input 1
    connector->setMapping(1, 2); // Device output 1 -> Controller input 2
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Test forward iteration from device output
    managed::RoutingNodeLabel deviceOutput0(device, 0, managed::NodeType::Device);
    auto output0 = routingBuilder->getVertexByLabel(deviceOutput0);
    ASSERT_TRUE(output0.occupied());
    
    // Get valid edges from this device output
    auto validEdges = managed::RoutingConstraint::validEdgesFrom(*routingBuilder, *output0);
    
    size_t edgeCount = 0;
    for (auto target : validEdges) {
        edgeCount++;
        auto targetLabel = routingBuilder->getVertexLabel(target);
        
        // Should connect to controller input 1 (due to mapping)
        //ASSERT_EQ(targetLabel->getDomain(), controller);
        ASSERT_EQ(targetLabel->getIndex(), 1u);
        ASSERT_EQ(targetLabel->getType(), managed::NodeType::Input);
    }
    ASSERT_EQ(edgeCount, 1u); // Should find exactly one valid target
}

TEST(PotentialEdgeIteratorBackward) {
    topology::resetTopologyState();
    
    auto device = make_shared<MockEmitterDomain>(2);
    auto controller = make_shared<MockRoutableDomain>(3, 2);
    
    topology::registerDomain(device);
    topology::registerDomain(controller);
    
    auto connector = make_shared<MockDomainConnector>(device, controller, 2, 3);
    connector->setMapping(0, 1); // Device output 0 -> Controller input 1
    connector->setMapping(1, 2); // Device output 1 -> Controller input 2
    topology::registerConnector(connector);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Test backward iteration to controller input
    managed::RoutingNodeLabel controllerInput1(controller, 1, managed::NodeType::Input);
    auto input1 = routingBuilder->getVertexByLabel(controllerInput1);
    ASSERT_TRUE(input1.occupied());
    
    // Get valid edges to this controller input
    auto validEdges = managed::RoutingConstraint::validEdgesTo(*routingBuilder, *input1);
    
    size_t edgeCount = 0;
    for (auto source : validEdges) {
        edgeCount++;
        auto sourceLabel = routingBuilder->getVertexLabel(source);
        
        // Should connect from device output 0 (due to mapping)
        //ASSERT_EQ(sourceLabel->getDomain(), device);
        ASSERT_EQ(sourceLabel->getIndex(), 0u);
        ASSERT_EQ(sourceLabel->getType(), managed::NodeType::Device);
    }
    ASSERT_EQ(edgeCount, 1u); // Should find exactly one valid source
}

TEST(RoutingNodeLabelHashAndEquality) {
    topology::resetTopologyState();
    
    auto domain1 = make_shared<MockEmitterDomain>(2);
    auto domain2 = make_shared<MockReceiverDomain>(3);
    
    managed::RoutingNodeLabel label1(domain1, 0, managed::NodeType::Device);
    managed::RoutingNodeLabel label2(domain1, 0, managed::NodeType::Device);
    managed::RoutingNodeLabel label3(domain1, 1, managed::NodeType::Device);
    managed::RoutingNodeLabel label4(domain2, 0, managed::NodeType::Input);
    managed::RoutingNodeLabel label5(domain1, 0, managed::NodeType::Input);
    
    // Test equality
    ASSERT_TRUE(label1 == label2);  // Same domain, index, type
    ASSERT_FALSE(label1 == label3); // Different index
    ASSERT_FALSE(label1 == label4); // Different domain
    ASSERT_FALSE(label1 == label5); // Different type
    
    // Test hash consistency
    ASSERT_EQ(label1.hash(), label2.hash()); // Equal objects have equal hashes
    
    // Test that different objects likely have different hashes
    ASSERT_NE(label1.hash(), label3.hash());
    ASSERT_NE(label1.hash(), label4.hash());
    ASSERT_NE(label1.hash(), label5.hash());
}

TEST(ComplexRoutingScenario) {
    topology::resetTopologyState();
    
    // Create a complex interrupt routing scenario:
    // Device1 (2 outputs) -> Controller (4 inputs, 2 outputs) -> CPU (2 inputs)
    // Device2 (2 outputs) -> Controller
    
    auto device1 = make_shared<MockEmitterDomain>(2);
    auto device2 = make_shared<MockEmitterDomain>(2);
    auto controller = make_shared<MockRoutableDomain>(4, 2);
    auto cpu = make_shared<MockReceiverDomain>(2);
    
    topology::registerDomain(device1);
    topology::registerDomain(device2);
    topology::registerDomain(controller);
    topology::registerDomain(cpu);
    
    // Device1 -> Controller (outputs 0,1 -> inputs 0,1)
    auto connector1 = make_shared<MockDomainConnector>(device1, controller, 2, 4);
    connector1->setMapping(0, 0);
    connector1->setMapping(1, 1);
    
    // Device2 -> Controller (outputs 0,1 -> inputs 2,3)
    auto connector2 = make_shared<MockDomainConnector>(device2, controller, 2, 4);
    connector2->setMapping(0, 2);
    connector2->setMapping(1, 3);
    
    // Controller -> CPU (outputs 0,1 -> inputs 0,1)
    auto connector3 = make_shared<MockDomainConnector>(controller, cpu, 2, 2);
    connector3->setMapping(0, 0);
    connector3->setMapping(1, 1);
    
    topology::registerConnector(connector1);
    topology::registerConnector(connector2);
    topology::registerConnector(connector3);
    
    auto routingBuilder = managed::createRoutingGraphBuilder();
    ASSERT_TRUE(routingBuilder);
    
    // Expected vertices:
    // Device1: 2 device outputs
    // Device2: 2 device outputs  
    // Controller: 4 inputs
    // CPU: 2 inputs
    // Total: 10 vertices
    
    auto graph = routingBuilder->build();
    ASSERT_TRUE(graph.occupied());
    
    size_t vertexCount = 0;
    for (auto vertex : graph->vertices()) {
        vertexCount++;
    }
    ASSERT_EQ(vertexCount, 10u);
    
    // Test specific routing constraints
    managed::RoutingNodeLabel device1_out0(device1, 0, managed::NodeType::Device);
    managed::RoutingNodeLabel controller_in0(controller, 0, managed::NodeType::Input);
    managed::RoutingNodeLabel device2_out1(device2, 1, managed::NodeType::Device);
    managed::RoutingNodeLabel controller_in3(controller, 3, managed::NodeType::Input);
    
    auto d1_o0 = routingBuilder->getVertexByLabel(device1_out0);
    auto c_i0 = routingBuilder->getVertexByLabel(controller_in0);
    auto d2_o1 = routingBuilder->getVertexByLabel(device2_out1);
    auto c_i3 = routingBuilder->getVertexByLabel(controller_in3);
    
    ASSERT_TRUE(d1_o0.occupied());
    ASSERT_TRUE(c_i0.occupied());
    ASSERT_TRUE(d2_o1.occupied());
    ASSERT_TRUE(c_i3.occupied());
    
    // Test valid connections
    ASSERT_TRUE(routingBuilder->canAddEdge(*d1_o0, *c_i0)); // Device1 out 0 -> Controller in 0
    ASSERT_TRUE(routingBuilder->canAddEdge(*d2_o1, *c_i3)); // Device2 out 1 -> Controller in 3
    
    // Test invalid connections
    ASSERT_FALSE(routingBuilder->canAddEdge(*d1_o0, *c_i3)); // Wrong mapping
    ASSERT_FALSE(routingBuilder->canAddEdge(*d2_o1, *c_i0)); // Wrong mapping
}