//
// Unit tests for Core DArray and SArray classes
// Created by Spencer Martin on 3/28/26.
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <core/ds/Array.h>

using namespace CroCOSTest;

// -----------------------------------------------------------------------------------------
// DArray tests
// -----------------------------------------------------------------------------------------

TEST(DArray1DDefaultConstruct) {
    DArray<int, 1> arr(5u);
    ASSERT_EQ(1u, arr.rank());
    ASSERT_EQ(5u, arr.extent(0));
}

TEST(DArray2DDefaultConstruct) {
    DArray<int, 2> arr(3u, 4u);
    ASSERT_EQ(2u, arr.rank());
    ASSERT_EQ(3u, arr.extent(0));
    ASSERT_EQ(4u, arr.extent(1));
}

TEST(DArray1DReadWrite) {
    DArray<int, 1> arr(4u);
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    arr[3] = 40;
    ASSERT_EQ(10, arr[0]);
    ASSERT_EQ(20, arr[1]);
    ASSERT_EQ(30, arr[2]);
    ASSERT_EQ(40, arr[3]);
}

TEST(DArray2DReadWrite) {
    DArray<int, 2> arr(3u, 4u);
    for (size_t i = 0; i < 3; i++)
        for (size_t j = 0; j < 4; j++)
            arr[i, j] = (int)(i * 4 + j);

    for (size_t i = 0; i < 3; i++)
        for (size_t j = 0; j < 4; j++)
            ASSERT_EQ((int)(i * 4 + j), arr[i, j]);
}

TEST(DArray3DReadWrite) {
    DArray<int, 3> arr(2u, 3u, 4u);
    for (size_t i = 0; i < 2; i++)
        for (size_t j = 0; j < 3; j++)
            for (size_t k = 0; k < 4; k++)
                arr[i, j, k] = (int)(i * 12 + j * 4 + k);

    for (size_t i = 0; i < 2; i++)
        for (size_t j = 0; j < 3; j++)
            for (size_t k = 0; k < 4; k++)
                ASSERT_EQ((int)(i * 12 + j * 4 + k), arr[i, j, k]);
}

TEST(DArray1DInitializer) {
    DArray<int, 1> arr([](size_t i) -> int { return (int)i * 2; }, 5u);
    for (size_t i = 0; i < 5; i++)
        ASSERT_EQ((int)i * 2, arr[i]);
}

TEST(DArray2DInitializer) {
    DArray<int, 2> arr([](size_t i, size_t j) -> int { return (int)(i * 10 + j); }, 3u, 4u);
    for (size_t i = 0; i < 3; i++)
        for (size_t j = 0; j < 4; j++)
            ASSERT_EQ((int)(i * 10 + j), arr[i, j]);
}

TEST(DArray3DInitializer) {
    DArray<int, 3> arr([](size_t i, size_t j, size_t k) -> int { return (int)(i * 100 + j * 10 + k); }, 2u, 3u, 4u);
    for (size_t i = 0; i < 2; i++)
        for (size_t j = 0; j < 3; j++)
            for (size_t k = 0; k < 4; k++)
                ASSERT_EQ((int)(i * 100 + j * 10 + k), arr[i, j, k]);
}

TEST(DArrayCopyConstructor) {
    DArray<int, 2> original([](size_t i, size_t j) -> int { return (int)(i * 4 + j); }, 3u, 4u);
    DArray<int, 2> copy(original);

    for (size_t i = 0; i < 3; i++)
        for (size_t j = 0; j < 4; j++)
            ASSERT_EQ(original[i, j], copy[i, j]);

    // Verify independence: mutating the copy does not affect the original
    copy[1, 2] = 999;
    ASSERT_NE(original[1, 2], copy[1, 2]);
}

TEST(DArrayMoveConstructor) {
    DArray<int, 1> original([](size_t i) -> int { return (int)i; }, 4u);
    DArray<int, 1> moved(static_cast<DArray<int, 1>&&>(original));

    ASSERT_EQ(4u, moved.extent(0));
    for (size_t i = 0; i < 4; i++)
        ASSERT_EQ((int)i, moved[i]);
}

TEST(DArrayCopyAssignment) {
    DArray<int, 1> a([](size_t i) -> int { return (int)i; }, 4u);
    DArray<int, 1> b(2u);
    b = a;
    for (size_t i = 0; i < 4; i++)
        ASSERT_EQ(a[i], b[i]);
    b[0] = 999;
    ASSERT_NE(a[0], b[0]);
}

TEST(DArrayMoveAssignment) {
    DArray<int, 1> a([](size_t i) -> int { return (int)i * 3; }, 4u);
    DArray<int, 1> b(2u);
    b = static_cast<DArray<int, 1>&&>(a);
    ASSERT_EQ(4u, b.extent(0));
    for (size_t i = 0; i < 4; i++)
        ASSERT_EQ((int)i * 3, b[i]);
}

TEST(DArrayRankAndExtent) {
    DArray<double, 3> arr(2u, 5u, 7u);
    ASSERT_EQ(3u, arr.rank());
    ASSERT_EQ(2u, arr.extent(0));
    ASSERT_EQ(5u, arr.extent(1));
    ASSERT_EQ(7u, arr.extent(2));
}

TEST(DArrayOOBThrows) {
    DArray<int, 2> arr(3u, 4u);
    bool caught = false;
    try {
        int val = arr[3, 0];  // extent(0) == 3, so index 3 is out of bounds
        (void)val;
    } catch (const AssertionFailure& e) {
        caught = true;
        std::string msg = e.what();
        ASSERT_TRUE(msg.find("out of bounds") != std::string::npos);
    }
    ASSERT_TRUE(caught);
}

TEST(DArrayExtentOOBThrows) {
    DArray<int, 2> arr(3u, 4u);
    bool caught = false;
    try {
        arr.extent(2);  // only indices 0 and 1 are valid for Rank == 2
    } catch (const AssertionFailure& e) {
        caught = true;
    }
    ASSERT_TRUE(caught);
}

// -----------------------------------------------------------------------------------------
// SArray tests
// -----------------------------------------------------------------------------------------

TEST(SArray1DDefaultConstruct) {
    SArray<int, 5> arr;
    ASSERT_EQ(1u, arr.rank());
    ASSERT_EQ(5u, arr.extent(0));
}

TEST(SArray2DDefaultConstruct) {
    SArray<int, 3, 4> arr;
    ASSERT_EQ(2u, arr.rank());
    ASSERT_EQ(3u, arr.extent(0));
    ASSERT_EQ(4u, arr.extent(1));
}

TEST(SArray1DReadWrite) {
    SArray<int, 4> arr;
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    arr[3] = 40;
    ASSERT_EQ(10, arr[0]);
    ASSERT_EQ(20, arr[1]);
    ASSERT_EQ(30, arr[2]);
    ASSERT_EQ(40, arr[3]);
}

TEST(SArray2DReadWrite) {
    SArray<int, 3, 4> arr;
    for (size_t i = 0; i < 3; i++)
        for (size_t j = 0; j < 4; j++)
            arr[i, j] = (int)(i * 4 + j);

    for (size_t i = 0; i < 3; i++)
        for (size_t j = 0; j < 4; j++)
            ASSERT_EQ((int)(i * 4 + j), arr[i, j]);
}

TEST(SArray1DInitializer) {
    SArray<int, 6> arr([](size_t i) -> int { return (int)i * 3; });
    for (size_t i = 0; i < 6; i++)
        ASSERT_EQ((int)i * 3, arr[i]);
}

TEST(SArray2DInitializer) {
    SArray<int, 3, 4> arr([](size_t i, size_t j) -> int { return (int)(i * 10 + j); });
    for (size_t i = 0; i < 3; i++)
        for (size_t j = 0; j < 4; j++)
            ASSERT_EQ((int)(i * 10 + j), arr[i, j]);
}

TEST(SArray3DInitializer) {
    SArray<int, 2, 3, 4> arr([](size_t i, size_t j, size_t k) -> int { return (int)(i * 100 + j * 10 + k); });
    for (size_t i = 0; i < 2; i++)
        for (size_t j = 0; j < 3; j++)
            for (size_t k = 0; k < 4; k++)
                ASSERT_EQ((int)(i * 100 + j * 10 + k), arr[i, j, k]);
}

TEST(SArrayCopyConstructor) {
    SArray<int, 3, 4> original([](size_t i, size_t j) -> int { return (int)(i * 4 + j); });
    SArray<int, 3, 4> copy(original);

    for (size_t i = 0; i < 3; i++)
        for (size_t j = 0; j < 4; j++)
            ASSERT_EQ(original[i, j], copy[i, j]);

    // Verify independence
    copy[1, 2] = 999;
    ASSERT_NE(original[1, 2], copy[1, 2]);
}

TEST(SArrayMoveConstructor) {
    SArray<int, 4> original([](size_t i) -> int { return (int)i * 5; });
    SArray<int, 4> moved(static_cast<SArray<int, 4>&&>(original));

    for (size_t i = 0; i < 4; i++)
        ASSERT_EQ((int)i * 5, moved[i]);
}

TEST(SArrayRankAndExtent) {
    SArray<double, 2, 5, 7> arr;
    ASSERT_EQ(3u, arr.rank());
    ASSERT_EQ(2u, arr.extent(0));
    ASSERT_EQ(5u, arr.extent(1));
    ASSERT_EQ(7u, arr.extent(2));
}

TEST(SArrayOOBThrows) {
    SArray<int, 3, 4> arr;
    bool caught = false;
    try {
        int val = arr[3, 0];  // extent(0) == 3, so index 3 is out of bounds
        (void)val;
    } catch (const AssertionFailure& e) {
        caught = true;
        std::string msg = e.what();
        ASSERT_TRUE(msg.find("out of bounds") != std::string::npos);
    }
    ASSERT_TRUE(caught);
}

TEST(SArrayExtentOOBThrows) {
    SArray<int, 3, 4> arr;
    bool caught = false;
    try {
        arr.extent(2);  // only indices 0 and 1 are valid for Rank == 2
    } catch (const AssertionFailure& e) {
        caught = true;
    }
    ASSERT_TRUE(caught);
}