//
// Unit tests for Variant - focused on move/copy semantics and memory safety
// Created by Spencer Martin on 7/28/25.
//

#include "../test.h"
#include "../harness/TestHarness.h"

#include <core/ds/Variant.h>

using namespace CroCOSTest;

// Reuse TrackingObject from OptionalTest for consistency
class VariantTrackingObject {
private:
    int value;
    static int construction_count;
    static int copy_construction_count;
    static int move_construction_count;
    static int copy_assignment_count;
    static int move_assignment_count;
    static int destruction_count;
    static int alive_count;

public:
    VariantTrackingObject() : value(0) { 
        ++construction_count;
        ++alive_count;
    }
    
    VariantTrackingObject(int v) : value(v) { 
        ++construction_count;
        ++alive_count;
    }
    
    VariantTrackingObject(const VariantTrackingObject& other) : value(other.value) {
        ++copy_construction_count;
        ++alive_count;
    }
    
    VariantTrackingObject(VariantTrackingObject&& other) noexcept : value(other.value) {
        ++move_construction_count;
        ++alive_count;
        other.value = -1;
    }
    
    VariantTrackingObject& operator=(const VariantTrackingObject& other) {
        if (this != &other) {
            value = other.value;
            ++copy_assignment_count;
        }
        return *this;
    }
    
    VariantTrackingObject& operator=(VariantTrackingObject&& other) noexcept {
        if (this != &other) {
            value = other.value;
            other.value = -1;
            ++move_assignment_count;
        }
        return *this;
    }
    
    ~VariantTrackingObject() { 
        ++destruction_count;
        --alive_count;
    }
    
    int getValue() const { return value; }
    
    static int getConstructionCount() { return construction_count; }
    static int getCopyConstructionCount() { return copy_construction_count; }
    static int getMoveConstructionCount() { return move_construction_count; }
    static int getCopyAssignmentCount() { return copy_assignment_count; }
    static int getMoveAssignmentCount() { return move_assignment_count; }
    static int getDestructionCount() { return destruction_count; }
    static int getAliveCount() { return alive_count; }
    
    static void resetCounters() {
        construction_count = 0;
        copy_construction_count = 0;
        move_construction_count = 0;
        copy_assignment_count = 0;
        move_assignment_count = 0;
        destruction_count = 0;
        alive_count = 0;
    }
};

// Static member definitions
int VariantTrackingObject::construction_count = 0;
int VariantTrackingObject::copy_construction_count = 0;
int VariantTrackingObject::move_construction_count = 0;
int VariantTrackingObject::copy_assignment_count = 0;
int VariantTrackingObject::move_assignment_count = 0;
int VariantTrackingObject::destruction_count = 0;
int VariantTrackingObject::alive_count = 0;

// Additional tracking object for multi-type variant tests
class AlternativeTrackingObject {
private:
    double value;
    static int alive_count;

public:
    AlternativeTrackingObject(double v) : value(v) { ++alive_count; }
    AlternativeTrackingObject(const AlternativeTrackingObject& other) : value(other.value) { ++alive_count; }
    AlternativeTrackingObject(AlternativeTrackingObject&& other) noexcept : value(other.value) { ++alive_count; }
    ~AlternativeTrackingObject() { --alive_count; }
    
    double getValue() const { return value; }
    static int getAliveCount() { return alive_count; }
    static void resetCounters() { alive_count = 0; }
};

int AlternativeTrackingObject::alive_count = 0;

// Test basic Variant construction and destruction
TEST(VariantBasicLifecycle) {
    VariantTrackingObject::resetCounters();
    
    {
        Variant<VariantTrackingObject, monostate> var;
        ASSERT_TRUE(var.holds<monostate>());
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
    ASSERT_EQ(VariantTrackingObject::getConstructionCount(), 0);
}

// Test Variant with value construction
TEST(VariantValueConstruction) {
    VariantTrackingObject::resetCounters();
    
    {
        VariantTrackingObject obj(42);
        ASSERT_EQ(VariantTrackingObject::getConstructionCount(), 1);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
        
        Variant<VariantTrackingObject, monostate> var(obj);
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 42);
        ASSERT_EQ(VariantTrackingObject::getCopyConstructionCount(), 1);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 2);
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
    ASSERT_EQ(VariantTrackingObject::getDestructionCount(), 2);
}

// Test Variant move construction
TEST(VariantMoveConstruction) {
    VariantTrackingObject::resetCounters();
    
    {
        VariantTrackingObject obj(100);
        ASSERT_EQ(VariantTrackingObject::getConstructionCount(), 1);
        
        Variant<VariantTrackingObject, monostate> var(move(obj));
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 100);
        ASSERT_EQ(VariantTrackingObject::getMoveConstructionCount(), 1);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 2); // Original + moved copy
        ASSERT_EQ(obj.getValue(), -1); // Original should be moved from
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
}

// Test Variant copy constructor
TEST(VariantCopyConstructor) {
    VariantTrackingObject::resetCounters();
    
    {
        Variant<VariantTrackingObject, monostate> var1;
        var1.emplace<VariantTrackingObject>(200);
        ASSERT_TRUE(var1.holds<VariantTrackingObject>());
        ASSERT_EQ(var1.get<VariantTrackingObject>().getValue(), 200);
        ASSERT_EQ(VariantTrackingObject::getConstructionCount(), 1);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
        
        Variant<VariantTrackingObject, monostate> var2(var1);
        ASSERT_TRUE(var2.holds<VariantTrackingObject>());
        ASSERT_EQ(var2.get<VariantTrackingObject>().getValue(), 200);
        ASSERT_EQ(VariantTrackingObject::getCopyConstructionCount(), 1);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 2);
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
}

// Test Variant move constructor
TEST(VariantMoveConstructorFromVariant) {
    VariantTrackingObject::resetCounters();
    
    {
        Variant<VariantTrackingObject, monostate> var1;
        var1.emplace<VariantTrackingObject>(300);
        ASSERT_TRUE(var1.holds<VariantTrackingObject>());
        ASSERT_EQ(var1.get<VariantTrackingObject>().getValue(), 300);
        ASSERT_EQ(VariantTrackingObject::getConstructionCount(), 1);
        
        Variant<VariantTrackingObject, monostate> var2(move(var1));
        ASSERT_TRUE(var2.holds<VariantTrackingObject>());
        ASSERT_EQ(var2.get<VariantTrackingObject>().getValue(), 300);
        ASSERT_EQ(VariantTrackingObject::getMoveConstructionCount(), 1);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1); // Only one should be alive after move
        ASSERT_EQ(var1.which(), static_cast<size_t>(-1)); // Source should be destroyed
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
}

// Test emplace with multiple constructions/destructions
TEST(VariantEmplaceStress) {
    VariantTrackingObject::resetCounters();
    
    {
        Variant<VariantTrackingObject, monostate> var;
        
        // Emplace multiple times
        var.emplace<VariantTrackingObject>(1);
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 1);
        ASSERT_EQ(VariantTrackingObject::getConstructionCount(), 1);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
        
        var.emplace<VariantTrackingObject>(2);
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 2);
        ASSERT_EQ(VariantTrackingObject::getConstructionCount(), 2);
        ASSERT_EQ(VariantTrackingObject::getDestructionCount(), 1); // Previous value destroyed
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
        
        var.emplace<VariantTrackingObject>(3);
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 3);
        ASSERT_EQ(VariantTrackingObject::getConstructionCount(), 3);
        ASSERT_EQ(VariantTrackingObject::getDestructionCount(), 2);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
    ASSERT_EQ(VariantTrackingObject::getDestructionCount(), 3);
}

// Test Variant with type switching
TEST(VariantTypeSwitching) {
    VariantTrackingObject::resetCounters();
    AlternativeTrackingObject::resetCounters();
    
    {
        Variant<VariantTrackingObject, AlternativeTrackingObject, monostate> var;
        
        // Start with VariantTrackingObject
        var.emplace<VariantTrackingObject>(10);
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
        ASSERT_EQ(AlternativeTrackingObject::getAliveCount(), 0);
        
        // Switch to AlternativeTrackingObject
        var.emplace<AlternativeTrackingObject>(20.5);
        ASSERT_TRUE(var.holds<AlternativeTrackingObject>());
        ASSERT_EQ(var.get<AlternativeTrackingObject>().getValue(), 20.5);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0); // Should be destroyed
        ASSERT_EQ(AlternativeTrackingObject::getAliveCount(), 1);
        ASSERT_EQ(VariantTrackingObject::getDestructionCount(), 1);
        
        // Switch back to VariantTrackingObject
        var.emplace<VariantTrackingObject>(30);
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 30);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
        ASSERT_EQ(AlternativeTrackingObject::getAliveCount(), 0); // Should be destroyed
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
    ASSERT_EQ(AlternativeTrackingObject::getAliveCount(), 0);
}

// Test complex copy/move scenarios
TEST(VariantComplexMoveScenarios) {
    VariantTrackingObject::resetCounters();
    
    {
        // Create a chain of variants with different construction patterns
        Variant<VariantTrackingObject, monostate> var1;
        var1.emplace<VariantTrackingObject>(10);
        
        Variant<VariantTrackingObject, monostate> var2(var1); // Copy construct
        Variant<VariantTrackingObject, monostate> var3(move(var1)); // Move construct
        
        ASSERT_EQ(var1.which(), static_cast<size_t>(-1)); // Should be destroyed after move
        ASSERT_TRUE(var2.holds<VariantTrackingObject>());
        ASSERT_TRUE(var3.holds<VariantTrackingObject>());
        ASSERT_EQ(var2.get<VariantTrackingObject>().getValue(), 10);
        ASSERT_EQ(var3.get<VariantTrackingObject>().getValue(), 10);
        
        // Test assignment
        Variant<VariantTrackingObject, monostate> var4;
        var4 = var2; // Copy assignment
        ASSERT_TRUE(var4.holds<VariantTrackingObject>());
        ASSERT_EQ(var4.get<VariantTrackingObject>().getValue(), 10);
        
        Variant<VariantTrackingObject, monostate> var5;
        var5 = move(var3); // Move assignment
        ASSERT_EQ(var3.which(), static_cast<size_t>(-1)); // Should be destroyed after move
        ASSERT_TRUE(var5.holds<VariantTrackingObject>());
        ASSERT_EQ(var5.get<VariantTrackingObject>().getValue(), 10);
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
}

// Test multiple Variant operations that could cause use-after-free
TEST(VariantUseAfterFreeStress) {
    VariantTrackingObject::resetCounters();
    
    {
        // Create variants and perform operations that previously caused issues
        Variant<VariantTrackingObject, monostate> variants[10];
        
        // Fill with values
        for (int i = 0; i < 10; ++i) {
            variants[i].emplace<VariantTrackingObject>(i);
        }
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 10);
        
        // Copy some to others (this can reveal double-free bugs)
        for (int i = 0; i < 5; ++i) {
            variants[i + 5] = variants[i];
        }
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 10); // Should have 5 old + 5 new copies
        
        // Move some around (this can cause use-after-free)
        for (int i = 0; i < 3; ++i) {
            Variant<VariantTrackingObject, monostate> temp = move(variants[i]);
            variants[i] = move(temp);
        }
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 10);
        
        // Verify all values are still correct
        for (int i = 0; i < 10; ++i) {
            ASSERT_TRUE(variants[i].holds<VariantTrackingObject>());
            if (i < 5) {
                ASSERT_EQ(variants[i].get<VariantTrackingObject>().getValue(), i);
                ASSERT_EQ(variants[i + 5].get<VariantTrackingObject>().getValue(), i);
            }
        }
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
}

// Test edge case: Variant with self-assignment
TEST(VariantSelfAssignment) {
    VariantTrackingObject::resetCounters();
    
    {
        Variant<VariantTrackingObject, monostate> var;
        var.emplace<VariantTrackingObject>(999);
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 999);
        
        // Self copy assignment
        var = var;
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 999);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
        
        // Self move assignment
        var = move(var);
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 999);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
}

// Test Variant assignment between different types
TEST(VariantTypeAssignment) {
    VariantTrackingObject::resetCounters();
    
    {
        Variant<VariantTrackingObject, monostate> var;
        
        // Assign a tracking object
        VariantTrackingObject obj(42);
        var = obj;
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 42);
        ASSERT_EQ(VariantTrackingObject::getCopyConstructionCount(), 1);
        
        // Assign another tracking object (should use assignment, not construction)
        VariantTrackingObject obj2(84);
        var = obj2;
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 84);
        ASSERT_EQ(VariantTrackingObject::getCopyAssignmentCount(), 1);
        
        // Move assign
        VariantTrackingObject obj3(168);
        var = move(obj3);
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 168);
        ASSERT_EQ(VariantTrackingObject::getMoveAssignmentCount(), 1);
        ASSERT_EQ(obj3.getValue(), -1); // Should be moved from
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
}

// Test basic Variant operations without visit/transform which have compilation issues
TEST(VariantBasicOperations) {
    VariantTrackingObject::resetCounters();
    
    {
        Variant<VariantTrackingObject, monostate> var;
        var.emplace<VariantTrackingObject>(42);
        
        ASSERT_TRUE(var.holds<VariantTrackingObject>());
        ASSERT_EQ(var.get<VariantTrackingObject>().getValue(), 42);
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
        
        // Test get_if
        auto* ptr = var.get_if<VariantTrackingObject>();
        ASSERT_TRUE(ptr != nullptr);
        ASSERT_EQ(ptr->getValue(), 42);
        
        auto* null_ptr = var.get_if<monostate>();
        ASSERT_TRUE(null_ptr == nullptr);
    }
    
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
}

// Test Variant destruction ordering in complex scenarios
TEST(VariantDestructionOrdering) {
    VariantTrackingObject::resetCounters();
    AlternativeTrackingObject::resetCounters();
    
    {
        // Create variants in different scopes to test destruction ordering
        {
            Variant<VariantTrackingObject, AlternativeTrackingObject, monostate> var1;
            var1.emplace<VariantTrackingObject>(1);
            
            {
                Variant<VariantTrackingObject, AlternativeTrackingObject, monostate> var2;
                var2.emplace<AlternativeTrackingObject>(2.0);
                
                // Copy var1 to var2 (should destroy AlternativeTrackingObject in var2)
                var2 = var1;
                ASSERT_EQ(VariantTrackingObject::getAliveCount(), 2);
                ASSERT_EQ(AlternativeTrackingObject::getAliveCount(), 0);
                
            } // var2 goes out of scope here
            
            ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1);
            ASSERT_EQ(AlternativeTrackingObject::getAliveCount(), 0);
            
        } // var1 goes out of scope here
        
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
        ASSERT_EQ(AlternativeTrackingObject::getAliveCount(), 0);
    }
}

// Test the specific double-destruction scenario that was mentioned
TEST(VariantDoubleDestructionPrevention) {
    VariantTrackingObject::resetCounters();
    
    {
        // Simulate the scenario that previously caused double-free issues
        Variant<VariantTrackingObject, monostate> var1;
        var1.emplace<VariantTrackingObject>(100);
        
        Variant<VariantTrackingObject, monostate> var2;
        var2.emplace<VariantTrackingObject>(200);
        
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 2);
        
        // Move var1 into var2 - this destroys both var2's old value AND var1's value
        // This Variant implementation uses "destructive move" semantics
        var2 = move(var1);
        
        ASSERT_EQ(VariantTrackingObject::getAliveCount(), 1); // Only one should remain
        ASSERT_TRUE(var2.holds<VariantTrackingObject>());
        ASSERT_EQ(var2.get<VariantTrackingObject>().getValue(), 100);
        ASSERT_EQ(var1.which(), static_cast<size_t>(-1)); // var1 should be empty
        
        // Verify destruction count - Variant's move assignment destroys both:
        // 1. var2's original value (200) and 2. var1's moved value (100)
        ASSERT_EQ(VariantTrackingObject::getDestructionCount(), 2);
    }
    
    // Final destruction should happen cleanly
    ASSERT_EQ(VariantTrackingObject::getAliveCount(), 0);
    ASSERT_EQ(VariantTrackingObject::getDestructionCount(), 3); // var2's original + var1's moved + var2's final
}