//
// Unit tests for Optional - focused on move/copy semantics and memory safety
// Created by Spencer Martin on 7/28/25.
//

#include "../test.h"
#include "../harness/TestHarness.h"

#include <core/ds/Optional.h>

using namespace CroCOSTest;

// Tracking object for detailed move/copy semantics testing
class TrackingObject {
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
    // Default constructor
    TrackingObject() : value(0) { 
        ++construction_count;
        ++alive_count;
    }
    
    // Value constructor
    TrackingObject(int v) : value(v) { 
        ++construction_count;
        ++alive_count;
    }
    
    // Copy constructor
    TrackingObject(const TrackingObject& other) : value(other.value) {
        ++copy_construction_count;
        ++alive_count;
    }
    
    // Move constructor
    TrackingObject(TrackingObject&& other) noexcept : value(other.value) {
        ++move_construction_count;
        ++alive_count;
        other.value = -1; // Mark as moved
    }
    
    // Copy assignment
    TrackingObject& operator=(const TrackingObject& other) {
        if (this != &other) {
            value = other.value;
            ++copy_assignment_count;
        }
        return *this;
    }
    
    // Move assignment
    TrackingObject& operator=(TrackingObject&& other) noexcept {
        if (this != &other) {
            value = other.value;
            other.value = -1; // Mark as moved
            ++move_assignment_count;
        }
        return *this;
    }
    
    ~TrackingObject() { 
        ++destruction_count;
        --alive_count;
    }
    
    int getValue() const { return value; }
    
    // Static counters
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
int TrackingObject::construction_count = 0;
int TrackingObject::copy_construction_count = 0;
int TrackingObject::move_construction_count = 0;
int TrackingObject::copy_assignment_count = 0;
int TrackingObject::move_assignment_count = 0;
int TrackingObject::destruction_count = 0;
int TrackingObject::alive_count = 0;

// Test basic Optional construction and destruction
TEST(OptionalBasicLifecycle) {
    TrackingObject::resetCounters();
    
    {
        Optional<TrackingObject> opt;
        ASSERT_FALSE(opt.occupied());
        ASSERT_EQ(TrackingObject::getAliveCount(), 0);
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
    ASSERT_EQ(TrackingObject::getConstructionCount(), 0);
    ASSERT_EQ(TrackingObject::getDestructionCount(), 0);
}

// Test Optional with value construction
TEST(OptionalValueConstruction) {
    TrackingObject::resetCounters();
    
    {
        TrackingObject obj(42);
        ASSERT_EQ(TrackingObject::getConstructionCount(), 1);
        ASSERT_EQ(TrackingObject::getAliveCount(), 1);
        
        Optional<TrackingObject> opt(obj);
        ASSERT_TRUE(opt.occupied());
        ASSERT_EQ(opt->getValue(), 42);
        ASSERT_EQ(TrackingObject::getCopyConstructionCount(), 1);
        ASSERT_EQ(TrackingObject::getAliveCount(), 2);
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
    ASSERT_EQ(TrackingObject::getDestructionCount(), 2);
}

// Test Optional move construction
TEST(OptionalMoveConstruction) {
    TrackingObject::resetCounters();
    
    {
        TrackingObject obj(100);
        ASSERT_EQ(TrackingObject::getConstructionCount(), 1);
        
        Optional<TrackingObject> opt(move(obj));
        ASSERT_TRUE(opt.occupied());
        ASSERT_EQ(opt->getValue(), 100);
        ASSERT_EQ(TrackingObject::getMoveConstructionCount(), 1);
        ASSERT_EQ(TrackingObject::getAliveCount(), 2); // Original + moved copy
        ASSERT_EQ(obj.getValue(), -1); // Original should be moved from
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
}

// Test Optional copy constructor
TEST(OptionalCopyConstructor) {
    TrackingObject::resetCounters();
    
    {
        Optional<TrackingObject> opt1;
        opt1.emplace(200);
        ASSERT_TRUE(opt1.occupied());
        ASSERT_EQ(opt1->getValue(), 200);
        ASSERT_EQ(TrackingObject::getConstructionCount(), 1);
        ASSERT_EQ(TrackingObject::getAliveCount(), 1);
        
        Optional<TrackingObject> opt2(opt1);
        ASSERT_TRUE(opt2.occupied());
        ASSERT_EQ(opt2->getValue(), 200);
        ASSERT_EQ(TrackingObject::getCopyConstructionCount(), 1);
        ASSERT_EQ(TrackingObject::getAliveCount(), 2);
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
}

// Test Optional move constructor
TEST(OptionalMoveConstructorFromOptional) {
    TrackingObject::resetCounters();
    
    {
        Optional<TrackingObject> opt1;
        opt1.emplace(300);
        ASSERT_TRUE(opt1.occupied());
        ASSERT_EQ(opt1->getValue(), 300);
        ASSERT_EQ(TrackingObject::getConstructionCount(), 1);
        
        Optional<TrackingObject> opt2(move(opt1));
        ASSERT_TRUE(opt2.occupied());
        ASSERT_EQ(opt2->getValue(), 300);
        // Note: Optional uses Variant internally, so the exact move semantics 
        // depend on Variant's implementation
        ASSERT_EQ(TrackingObject::getAliveCount(), 1); // Only one should be alive after move
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
}

// Test emplace with multiple constructions/destructions
TEST(OptionalEmplaceStress) {
    TrackingObject::resetCounters();
    
    {
        Optional<TrackingObject> opt;
        
        // Emplace multiple times
        opt.emplace(1);
        ASSERT_TRUE(opt.occupied());
        ASSERT_EQ(opt->getValue(), 1);
        ASSERT_EQ(TrackingObject::getConstructionCount(), 1);
        ASSERT_EQ(TrackingObject::getAliveCount(), 1);
        
        opt.emplace(2);
        ASSERT_TRUE(opt.occupied());
        ASSERT_EQ(opt->getValue(), 2);
        ASSERT_EQ(TrackingObject::getConstructionCount(), 2);
        ASSERT_EQ(TrackingObject::getDestructionCount(), 1); // Previous value destroyed
        ASSERT_EQ(TrackingObject::getAliveCount(), 1);
        
        opt.emplace(3);
        ASSERT_TRUE(opt.occupied());
        ASSERT_EQ(opt->getValue(), 3);
        ASSERT_EQ(TrackingObject::getConstructionCount(), 3);
        ASSERT_EQ(TrackingObject::getDestructionCount(), 2);
        ASSERT_EQ(TrackingObject::getAliveCount(), 1);
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
    ASSERT_EQ(TrackingObject::getDestructionCount(), 3);
}

// Test Optional with complex copy/move scenarios
TEST(OptionalComplexMoveScenarios) {
    TrackingObject::resetCounters();
    
    {
        // Create a chain of optionals with different construction patterns
        Optional<TrackingObject> opt1;
        opt1.emplace(10);
        
        Optional<TrackingObject> opt2(opt1); // Copy construct
        Optional<TrackingObject> opt3(move(opt1)); // Move construct
        
        ASSERT_FALSE(opt1.occupied()); // Should be empty after move
        ASSERT_TRUE(opt2.occupied());
        ASSERT_TRUE(opt3.occupied());
        ASSERT_EQ(opt2->getValue(), 10);
        ASSERT_EQ(opt3->getValue(), 10);
        
        // Test assignment
        Optional<TrackingObject> opt4;
        opt4 = opt2; // Copy assignment
        ASSERT_TRUE(opt4.occupied());
        ASSERT_EQ(opt4->getValue(), 10);
        
        Optional<TrackingObject> opt5;
        opt5 = move(opt3); // Move assignment
        ASSERT_FALSE(opt3.occupied()); // Should be empty after move
        ASSERT_TRUE(opt5.occupied());
        ASSERT_EQ(opt5->getValue(), 10);
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
}

// Test Optional transform functionality with move semantics
TEST(OptionalTransformMoveSemantics) {
    TrackingObject::resetCounters();
    
    {
        Optional<TrackingObject> opt;
        opt.emplace(42);
        
        auto transformed = opt.transform([](const TrackingObject& obj) {
            return obj.getValue() * 2;
        });
        
        ASSERT_TRUE(transformed.occupied());
        ASSERT_EQ(*transformed, 84);
        ASSERT_TRUE(opt.occupied()); // Original should still be occupied
        ASSERT_EQ(opt->getValue(), 42);
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
}

// Test Optional and_then functionality 
TEST(OptionalAndThenMoveSemantics) {
    TrackingObject::resetCounters();
    
    {
        Optional<TrackingObject> opt;
        opt.emplace(10);
        
        auto chained = opt.and_then([](const TrackingObject& obj) -> Optional<int> {
            if (obj.getValue() > 5) {
                return Optional<int>(obj.getValue() + 100);
            }
            return Optional<int>();
        });
        
        ASSERT_TRUE(chained.occupied());
        ASSERT_EQ(*chained, 110);
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
}

// Test multiple Optional operations that could cause use-after-free
TEST(OptionalUseAfterFreeStress) {
    TrackingObject::resetCounters();
    
    {
        // Create optionals and perform operations that previously caused issues
        Optional<TrackingObject> optionals[10];
        
        // Fill with values
        for (int i = 0; i < 10; ++i) {
            optionals[i].emplace(i);
        }
        ASSERT_EQ(TrackingObject::getAliveCount(), 10);
        
        // Copy some to others (this can reveal double-free bugs)
        for (int i = 0; i < 5; ++i) {
            optionals[i + 5] = optionals[i];
        }
        ASSERT_EQ(TrackingObject::getAliveCount(), 10); // Still 10 distinct objects
        
        // Move some around (this can cause use-after-free)
        for (int i = 0; i < 3; ++i) {
            Optional<TrackingObject> temp = move(optionals[i]);
            optionals[i] = move(temp);
        }
        ASSERT_EQ(TrackingObject::getAliveCount(), 10);
        
        // Verify all values are still correct
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(optionals[i].occupied());
            ASSERT_EQ(optionals[i]->getValue(), i);
            ASSERT_TRUE(optionals[i + 5].occupied());
            ASSERT_EQ(optionals[i + 5]->getValue(), i);
        }
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
}

// Test edge case: Optional with self-assignment
TEST(OptionalSelfAssignment) {
    TrackingObject::resetCounters();
    
    {
        Optional<TrackingObject> opt;
        opt.emplace(999);
        ASSERT_TRUE(opt.occupied());
        ASSERT_EQ(opt->getValue(), 999);
        
        // Self copy assignment
        opt = opt;
        ASSERT_TRUE(opt.occupied());
        ASSERT_EQ(opt->getValue(), 999);
        ASSERT_EQ(TrackingObject::getAliveCount(), 1);
        
        // Self move assignment
        opt = move(opt);
        ASSERT_TRUE(opt.occupied());
        ASSERT_EQ(opt->getValue(), 999);
        ASSERT_EQ(TrackingObject::getAliveCount(), 1);
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
}

// Test Optional with objects that have expensive copy/move operations
TEST(OptionalExpensiveOperations) {
    TrackingObject::resetCounters();
    
    {
        Optional<TrackingObject> opt1;
        Optional<TrackingObject> opt2;
        Optional<TrackingObject> opt3;
        
        // Create a value
        opt1.emplace(12345);
        ASSERT_EQ(TrackingObject::getConstructionCount(), 1);
        
        // Copy to opt2 (should trigger copy construction)
        opt2 = opt1;
        ASSERT_EQ(TrackingObject::getCopyConstructionCount(), 1);
        ASSERT_EQ(TrackingObject::getAliveCount(), 2);
        
        // Move to opt3 (should trigger move construction)
        opt3 = move(opt1);
        ASSERT_EQ(TrackingObject::getMoveConstructionCount(), 1);
        ASSERT_FALSE(opt1.occupied());
        ASSERT_TRUE(opt2.occupied());
        ASSERT_TRUE(opt3.occupied());
        ASSERT_EQ(TrackingObject::getAliveCount(), 2);
    }
    
    ASSERT_EQ(TrackingObject::getAliveCount(), 0);
}