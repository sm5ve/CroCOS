//
// Unit tests for Core Object system (CRClass)
// Created by Spencer Martin on 7/27/25.
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <core/Object.h>
#include <core/utility.h>

using namespace CroCOSTest;

// Test classes for CRClass functionality
CRClass(BaseClass) {
public:
    int baseValue = 10;
    virtual ~BaseClass() = default;
};

CRClass(DerivedClass, public BaseClass) {
public:
    int derivedValue = 20;
    virtual ~DerivedClass() = default;
};

CRClass(MultipleInheritanceBase1) {
public:
    int base1Value = 30;
    virtual ~MultipleInheritanceBase1() = default;
};

CRClass(MultipleInheritanceBase2) {
public:
    int base2Value = 40;
    virtual ~MultipleInheritanceBase2() = default;
};

CRClass(MultipleInheritanceDerived, public MultipleInheritanceBase1, public MultipleInheritanceBase2) {
public:
    int derivedValue = 50;
    virtual ~MultipleInheritanceDerived() = default;
};

TEST(CRClassBasicTypeID) {
    BaseClass base;
    DerivedClass derived;
    
    // Each class should have a unique TypeID
    ASSERT_NE(base.type_id(), derived.type_id());
    
    // Instance should return its class TypeID
    ASSERT_EQ(base.type_id(), TypeID_v<BaseClass>);
    ASSERT_EQ(derived.type_id(), TypeID_v<DerivedClass>);
}

TEST(CRClassInheritanceCheck) {
    BaseClass base;
    DerivedClass derived;
    
    // Base class should not be derived from DerivedClass
    ASSERT_FALSE(base.instanceof(TypeID_v<DerivedClass>));
    
    // Derived class should be of type BaseClass and DerivedClass
    ASSERT_TRUE(derived.instanceof(TypeID_v<BaseClass>));
    ASSERT_TRUE(derived.instanceof(TypeID_v<DerivedClass>));
    
    // Self-type check should always be true
    ASSERT_TRUE(base.instanceof(TypeID_v<BaseClass>));
}

TEST(CRClassDynamicCast) {
    DerivedClass derived;
    BaseClass& baseRef = derived;
    
    // Cast from base reference to derived should work
    DerivedClass* derivedPtr = crocos_dynamic_cast<DerivedClass*>(&baseRef);
    ASSERT_NE(derivedPtr, nullptr);
    ASSERT_EQ(derivedPtr->derivedValue, 20);
    
    // Cast to unrelated type should fail
    MultipleInheritanceBase1* unrelatedPtr = crocos_dynamic_cast<MultipleInheritanceBase1*>(&baseRef);
    ASSERT_EQ(unrelatedPtr, nullptr);
}

TEST(CRClassDynamicCastWithBaseClass) {
    BaseClass base;
    
    // Cast from BaseClass to DerivedClass should fail
    DerivedClass* derivedPtr = crocos_dynamic_cast<DerivedClass*>(&base);
    ASSERT_EQ(derivedPtr, nullptr);
    
    // Cast to same type should work
    BaseClass* basePtr = crocos_dynamic_cast<BaseClass*>(&base);
    ASSERT_NE(basePtr, nullptr);
    ASSERT_EQ(basePtr->baseValue, 10);
}

TEST(CRClassMultipleInheritance) {
    MultipleInheritanceDerived derived;
    
    // Should be of all parent types
    ASSERT_TRUE(derived.instanceof(TypeID_v<MultipleInheritanceBase1>));
    ASSERT_TRUE(derived.instanceof(TypeID_v<MultipleInheritanceBase2>));
    ASSERT_TRUE(derived.instanceof(TypeID_v<MultipleInheritanceDerived>));
    
    // Casts to both base classes should work
    MultipleInheritanceBase1* base1Ptr = crocos_dynamic_cast<MultipleInheritanceBase1*>(&derived);
    ASSERT_NE(base1Ptr, nullptr);
    ASSERT_EQ(base1Ptr->base1Value, 30);
    
    MultipleInheritanceBase2* base2Ptr = crocos_dynamic_cast<MultipleInheritanceBase2*>(&derived);
    ASSERT_NE(base2Ptr, nullptr);
    ASSERT_EQ(base2Ptr->base2Value, 40);
}

TEST(CRClassMultipleInheritanceCrossCast) {
    MultipleInheritanceDerived derived;
    MultipleInheritanceBase1& base1Ref = derived;
    
    // Cast from one base to another through common derived class
    MultipleInheritanceBase2* base2Ptr = crocos_dynamic_cast<MultipleInheritanceBase2*>(&base1Ref);
    ASSERT_NE(base2Ptr, nullptr);
    ASSERT_EQ(base2Ptr->base2Value, 40);
    
    // Cast back to derived class should work
    MultipleInheritanceDerived* derivedPtr = crocos_dynamic_cast<MultipleInheritanceDerived*>(&base1Ref);
    ASSERT_NE(derivedPtr, nullptr);
    ASSERT_EQ(derivedPtr->derivedValue, 50);
}

TEST(CRClassTypeIDConsistency) {
    // TypeID should be consistent across multiple calls
    BaseClass obj1, obj2;
    auto id1 = obj1.type_id();
    auto id2 = obj1.type_id();
    ASSERT_EQ(id1, id2);
    
    // Different instances should have same TypeID
    ASSERT_EQ(obj1.type_id(), obj2.type_id());
    ASSERT_EQ(obj1.type_id(), TypeID_v<BaseClass>);
}

TEST(CRClassPolymorphism) {
    DerivedClass derived;
    BaseClass* basePtr = &derived;
    
    // Polymorphic type check should work through base pointer
    ASSERT_TRUE(basePtr->instanceof(TypeID_v<DerivedClass>));
    ASSERT_TRUE(basePtr->instanceof(TypeID_v<BaseClass>));
    
    // type_id should return derived class TypeID
    ASSERT_EQ(basePtr->type_id(), TypeID_v<DerivedClass>);
    ASSERT_NE(basePtr->type_id(), TypeID_v<BaseClass>);
}

TEST(CRClassInvalidCast) {
    BaseClass base;
    
    // Cast to completely unrelated type should return nullptr
    MultipleInheritanceDerived* invalidPtr = crocos_dynamic_cast<MultipleInheritanceDerived*>(&base);
    ASSERT_EQ(invalidPtr, nullptr);
    
    // Test with null pointer
    BaseClass* nullPtr = nullptr;
    DerivedClass* resultPtr = crocos_dynamic_cast<DerivedClass*>(nullPtr);
    ASSERT_EQ(resultPtr, nullptr);
}