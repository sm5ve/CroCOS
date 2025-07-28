//
// Unit tests for Smart Pointer Infrastructure
// Created by Claude Code on 7/28/25.
//

#include "../test.h"
#include "../harness/TestHarness.h"

#include <core/ds/SmartPointer.h>
#include <core/ds/HashSet.h>
#include <core/ds/HashMap.h>

using namespace CroCOSTest;

// Simple test class for SharedPtr testing
class TestObject {
private:
    int value;
    static int instance_count;
public:
    TestObject(int v) : value(v) { ++instance_count; }
    ~TestObject() { --instance_count; }
    
    int getValue() const { return value; }
    static int getInstanceCount() { return instance_count; }
    static void resetInstanceCount() { instance_count = 0; }
};

int TestObject::instance_count = 0;

// Test basic SharedPtr move semantics
TEST(SharedPtrMoveSemantics) {
    TestObject::resetInstanceCount();
    
    // Create initial SharedPtr
    auto ptr1 = make_shared<TestObject>(42);
    ASSERT_EQ(TestObject::getInstanceCount(), 1);
    ASSERT_TRUE(ptr1);
    ASSERT_EQ(ptr1->getValue(), 42);
    
    // Move construct
    auto ptr2 = move(ptr1);
    ASSERT_EQ(TestObject::getInstanceCount(), 1); // Same object
    ASSERT_FALSE(ptr1); // Source should be null
    ASSERT_TRUE(ptr2);
    ASSERT_EQ(ptr2->getValue(), 42);
    
    // Move assign
    SharedPtr<TestObject> ptr3;
    ptr3 = move(ptr2);
    ASSERT_EQ(TestObject::getInstanceCount(), 1); // Same object
    ASSERT_FALSE(ptr2); // Source should be null
    ASSERT_TRUE(ptr3);
    ASSERT_EQ(ptr3->getValue(), 42);
    
    // Object should be destroyed when ptr3 goes out of scope
}

// Test SharedPtr in HashSet
TEST(SharedPtrInHashSet) {
    TestObject::resetInstanceCount();
    
    HashSet<SharedPtr<TestObject>> ptrSet;
    
    auto ptr1 = make_shared<TestObject>(10);
    auto ptr2 = make_shared<TestObject>(20);
    auto ptr3 = make_shared<TestObject>(30);
    
    ASSERT_EQ(TestObject::getInstanceCount(), 3);
    
    // Insert into HashSet
    ASSERT_FALSE(ptrSet.insert(ptr1)); // Returns false because it wasn't present
    ASSERT_FALSE(ptrSet.insert(ptr2));
    ASSERT_FALSE(ptrSet.insert(ptr3));
    
    ASSERT_EQ(ptrSet.size(), 3u);
    ASSERT_EQ(TestObject::getInstanceCount(), 3); // Objects should still exist
    
    // Test that the same pointer is detected as duplicate
    ASSERT_TRUE(ptrSet.insert(ptr1)); // Returns true because it was already present
    ASSERT_EQ(ptrSet.size(), 3u); // Size unchanged
    
    // Objects should be destroyed when set goes out of scope
}

// Test move semantics with HashSet to ImmutableIndexedHashSet conversion
TEST(HashSetToImmutableIndexedHashSetMove) {
    TestObject::resetInstanceCount();
    
    {
        HashSet<SharedPtr<TestObject>> ptrSet;
        
        auto ptr1 = make_shared<TestObject>(100);
        auto ptr2 = make_shared<TestObject>(200);
        
        ASSERT_EQ(TestObject::getInstanceCount(), 2);
        
        ptrSet.insert(ptr1);
        ptrSet.insert(ptr2);
        
        ASSERT_EQ(ptrSet.size(), 2u);
        ASSERT_EQ(TestObject::getInstanceCount(), 2);
        
        // Move HashSet to ImmutableIndexedHashSet
        auto immutableSet = ImmutableIndexedHashSet<SharedPtr<TestObject>>(move(ptrSet));
        
        // Original set should be empty/moved
        ASSERT_EQ(ptrSet.size(), 0u);
        ASSERT_EQ(immutableSet.size(), 2u);
        ASSERT_EQ(TestObject::getInstanceCount(), 2); // Objects should still exist
        
        // Test lookup in immutable set
        auto index1 = immutableSet.indexOf(ptr1);
        auto index2 = immutableSet.indexOf(ptr2);
        
        ASSERT_TRUE(index1.occupied());
        ASSERT_TRUE(index2.occupied());
        
        auto* retrieved1 = immutableSet.fromIndex(*index1);
        auto* retrieved2 = immutableSet.fromIndex(*index2);
        
        ASSERT_TRUE(retrieved1);
        ASSERT_TRUE(retrieved2);
        ASSERT_EQ((*retrieved1)->getValue(), 100);
        ASSERT_EQ((*retrieved2)->getValue(), 200);
        
        // immutableSet goes out of scope here
    }
    
    // All objects should be destroyed
    ASSERT_EQ(TestObject::getInstanceCount(), 0);
}

// Test multiple move operations to isolate potential issues
TEST(SharedPtrMultipleMoveOperations) {
    TestObject::resetInstanceCount();
    
    {
        // Create several SharedPtrs
        auto ptr1 = make_shared<TestObject>(1);
        auto ptr2 = make_shared<TestObject>(2);
        auto ptr3 = make_shared<TestObject>(3);
        
        ASSERT_EQ(TestObject::getInstanceCount(), 3);
        
        // Create HashSet and populate
        HashSet<SharedPtr<TestObject>> set1;
        set1.insert(ptr1);
        set1.insert(ptr2);
        set1.insert(ptr3);
        
        ASSERT_EQ(set1.size(), 3u);
        ASSERT_EQ(TestObject::getInstanceCount(), 3);
        
        // Move to another HashSet
        HashSet<SharedPtr<TestObject>> set2 = move(set1);
        ASSERT_EQ(set1.size(), 0u);
        ASSERT_EQ(set2.size(), 3u);
        ASSERT_EQ(TestObject::getInstanceCount(), 3);
        
        // Move to ImmutableIndexedHashSet
        auto immutableSet1 = ImmutableIndexedHashSet<SharedPtr<TestObject>>(move(set2));
        ASSERT_EQ(set2.size(), 0u);
        ASSERT_EQ(immutableSet1.size(), 3u);
        ASSERT_EQ(TestObject::getInstanceCount(), 3);
        
        // Test that we can still access the objects
        auto index = immutableSet1.indexOf(ptr1);
        ASSERT_TRUE(index.occupied());
        auto* retrieved = immutableSet1.fromIndex(*index);
        ASSERT_TRUE(retrieved);
        ASSERT_EQ((*retrieved)->getValue(), 1);
        
        // All objects should still be alive
        ASSERT_EQ(TestObject::getInstanceCount(), 3);
    }
    
    // All objects should be destroyed
    ASSERT_EQ(TestObject::getInstanceCount(), 0);
}

// Test edge case: empty HashSet move
TEST(EmptyHashSetMove) {
    TestObject::resetInstanceCount();
    
    {
        HashSet<SharedPtr<TestObject>> emptySet;
        ASSERT_EQ(emptySet.size(), 0u);
        
        // Move empty set to ImmutableIndexedHashSet
        auto immutableSet = ImmutableIndexedHashSet<SharedPtr<TestObject>>(move(emptySet));
        ASSERT_EQ(emptySet.size(), 0u);
        ASSERT_EQ(immutableSet.size(), 0u);
        ASSERT_EQ(TestObject::getInstanceCount(), 0);
    }
    
    ASSERT_EQ(TestObject::getInstanceCount(), 0);
}

// Test exact scenario from interrupt graphs: make_shared<ImmutableIndexedHashSet<SharedPtr<T>>>
TEST(SharedPtrMakeSharedScenario) {
    TestObject::resetInstanceCount();
    
    {
        // Replicate the exact pattern from GraphBuilder.h:397
        HashSet<SharedPtr<TestObject>> labelSet;
        
        // This exactly matches: edgeLabels = make_shared<ImmutableIndexedHashSet<typename EdgeDecorator::LabelType>>(move(labelSet));
        auto edgeLabels = make_shared<ImmutableIndexedHashSet<SharedPtr<TestObject>>>(move(labelSet));
        
        ASSERT_TRUE(edgeLabels);
        ASSERT_EQ(edgeLabels->size(), 0u);
        ASSERT_EQ(TestObject::getInstanceCount(), 0);
        
        // edgeLabels SharedPtr goes out of scope here - this should trigger the issue if it exists
    }
    
    ASSERT_EQ(TestObject::getInstanceCount(), 0);
}

// Test SharedPtr as keys in HashMap - this matches GraphBuilder vertex label mapping
TEST(SharedPtrAsHashMapKeys) {
    TestObject::resetInstanceCount();
    
    {
        // This replicates the pattern in GraphBuilder: HashMap<SharedPtr<T>, size_t>
        HashMap<SharedPtr<TestObject>, size_t> vertexLabelMap;
        
        auto obj1 = make_shared<TestObject>(1);
        auto obj2 = make_shared<TestObject>(2);
        auto obj3 = make_shared<TestObject>(3);
        
        ASSERT_EQ(TestObject::getInstanceCount(), 3);
        
        // Insert SharedPtr objects as keys
        vertexLabelMap.insert(obj1, 0);
        vertexLabelMap.insert(obj2, 1);
        vertexLabelMap.insert(obj3, 2);
        
        ASSERT_EQ(vertexLabelMap.size(), 3u);
        ASSERT_EQ(TestObject::getInstanceCount(), 3); // Objects should still exist
        
        // Test lookup
        ASSERT_TRUE(vertexLabelMap.contains(obj1));
        ASSERT_TRUE(vertexLabelMap.contains(obj2));
        ASSERT_TRUE(vertexLabelMap.contains(obj3));
        
        ASSERT_EQ(vertexLabelMap.at(obj1), 0u);
        ASSERT_EQ(vertexLabelMap.at(obj2), 1u);
        ASSERT_EQ(vertexLabelMap.at(obj3), 2u);
        
        // Test removal (this is what happens in GraphBuilder when labels are cleared)
        vertexLabelMap.remove(obj2);
        ASSERT_EQ(vertexLabelMap.size(), 2u);
        ASSERT_FALSE(vertexLabelMap.contains(obj2));
        ASSERT_EQ(TestObject::getInstanceCount(), 3); // Objects should still exist
        
        // vertexLabelMap goes out of scope here - this should trigger the issue if it exists
    }
    
    // All objects should be destroyed
    ASSERT_EQ(TestObject::getInstanceCount(), 0);
}

// Test the specific double-destruction scenario
TEST(SharedPtrHashMapDestruction) {
    TestObject::resetInstanceCount();
    
    {
        HashMap<SharedPtr<TestObject>, size_t> map1;
        
        auto obj = make_shared<TestObject>(42);
        ASSERT_EQ(TestObject::getInstanceCount(), 1);
        
        map1.insert(obj, 100);
        ASSERT_EQ(TestObject::getInstanceCount(), 1);
        
        // Create another map and copy the mapping
        HashMap<SharedPtr<TestObject>, size_t> map2;
        map2.insert(obj, 200);  // Same SharedPtr in two different maps
        
        ASSERT_EQ(TestObject::getInstanceCount(), 1); // Still just one object
        ASSERT_EQ(map1.size(), 1u);
        ASSERT_EQ(map2.size(), 1u);
        
        // Both maps should contain the same SharedPtr
        ASSERT_TRUE(map1.contains(obj));
        ASSERT_TRUE(map2.contains(obj));
        
        // obj goes out of scope here, but maps should keep the object alive
    }
    
    // Object should only be destroyed once when both maps are destroyed
    ASSERT_EQ(TestObject::getInstanceCount(), 0);
}

// Test the exact pattern used in GraphBuilder
TEST(SharedPtrGraphBuilderPattern) {
    TestObject::resetInstanceCount();
    
    {
        // Simulate GraphBuilder's vertex label map
        HashMap<SharedPtr<TestObject>, size_t> vertexLabelMap;
        
        // Create SharedPtr objects and store them like GraphBuilder.addVertex() does
        {
            auto obj1 = make_shared<TestObject>(1);
            auto obj2 = make_shared<TestObject>(2);
            auto obj3 = make_shared<TestObject>(3);
            
            ASSERT_EQ(TestObject::getInstanceCount(), 3);
            
            // Insert into map like _setVertexLabel does
            vertexLabelMap.insert(obj1, 0);
            vertexLabelMap.insert(obj2, 1); 
            vertexLabelMap.insert(obj3, 2);
            
            ASSERT_EQ(TestObject::getInstanceCount(), 3);
            
            // obj1, obj2, obj3 go out of scope here
            // HashMap should still hold references, keeping objects alive
        }
        
        // Objects should still exist because HashMap holds references
        ASSERT_EQ(TestObject::getInstanceCount(), 3);
        
        // Simulate what GraphBuilder.buildGraph() does - create a HashSet from the map values
        HashSet<SharedPtr<TestObject>> labelSet;
        // This simulates: for (const auto& vinfo : vertexInfo) { labelSet.insert(*(vinfo.label)); }
        for (size_t i = 0; i < 3; i++) {
            // Find the SharedPtr key corresponding to index i
            for (auto it = vertexLabelMap.begin(); it != vertexLabelMap.end(); ++it) {
                if ((*it).second() == i) {
                    labelSet.insert((*it).first());
                    break;
                }
            }
        }
        
        ASSERT_EQ(TestObject::getInstanceCount(), 3);
        
        // Create ImmutableIndexedHashSet like buildGraph() does
        auto immutableLabels = make_shared<ImmutableIndexedHashSet<SharedPtr<TestObject>>>(move(labelSet));
        
        ASSERT_EQ(TestObject::getInstanceCount(), 3);
        
        // vertexLabelMap and immutableLabels go out of scope here
        // This should trigger the heap-use-after-free if the bug exists
    }
    
    // All objects should be destroyed
    ASSERT_EQ(TestObject::getInstanceCount(), 0);
}