//
// Unit tests for Core Permutation and PermutationWindow classes
// Created by Claude on 2/3/26.
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <core/ds/Permutation.h>

using namespace CroCOSTest;

// Helper to create a dense permutation with stack-allocated buffers
template <size_t N>
struct DensePermutationFixture {
    using PermType = Permutation<8, DENSE>;
    using IndexType = PermType::IndexType;
    using PositionIndex = PermType::PositionIndex;
    using ElementIndex = PermType::ElementIndex;

    IndexType forwardBuffer[N];
    IndexType backwardBuffer[N];
    PermType perm;

    DensePermutationFixture() : perm(forwardBuffer, backwardBuffer, N) {
        perm.reset();
    }
};

// ==================== Permutation Tests ====================

TEST(PermutationConstruction) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;

    ASSERT_EQ(5u, perm.elementCount());
    ASSERT_EQ(5u, perm.positionCount());
}

TEST(PermutationResetIdentity) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // After reset, each element should be at its own position (identity permutation)
    for (uint8_t i = 0; i < 5; i++) {
        ASSERT_EQ(i, perm.elementAt(PositionIndex{i}));
        ASSERT_EQ(i, perm[PositionIndex{i}]);
    }
}

TEST(PermutationPositionOfAfterReset) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using ElementIndex = DensePermutationFixture<5>::ElementIndex;

    // After reset, each element should be at its corresponding position
    for (uint8_t i = 0; i < 5; i++) {
        ASSERT_EQ(i, perm.positionOf(ElementIndex{i}));
    }
}

TEST(PermutationSwapPositions) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;
    using ElementIndex = DensePermutationFixture<5>::ElementIndex;

    // Initial: [0, 1, 2, 3, 4]
    // Swap positions 1 and 3
    perm.swap(PositionIndex{1}, PositionIndex{3});

    // After swap: [0, 3, 2, 1, 4]
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(3u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{2}));
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{3}));
    ASSERT_EQ(4u, perm.elementAt(PositionIndex{4}));

    // Check reverse mapping
    ASSERT_EQ(3u, perm.positionOf(ElementIndex{1}));
    ASSERT_EQ(1u, perm.positionOf(ElementIndex{3}));
}

TEST(PermutationSwapElements) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;
    using ElementIndex = DensePermutationFixture<5>::ElementIndex;

    // Initial: [0, 1, 2, 3, 4]
    // Swap elements 1 and 3 (they're at positions 1 and 3 initially)
    perm.swap(ElementIndex{1}, ElementIndex{3});

    // After swap: [0, 3, 2, 1, 4] (same result as swapping positions in identity)
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(3u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{2}));
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{3}));
    ASSERT_EQ(4u, perm.elementAt(PositionIndex{4}));
}

TEST(PermutationSwapSelf) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Swapping an element with itself should be a no-op
    perm.swap(PositionIndex{2}, PositionIndex{2});

    // Should remain identity
    for (uint8_t i = 0; i < 5; i++) {
        ASSERT_EQ(i, perm.elementAt(PositionIndex{i}));
    }
}

TEST(PermutationRotateRightVariadic) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Initial: [0, 1, 2, 3, 4]
    // Rotate right positions 0, 1, 2: element at pos 2 -> pos 0, pos 0 -> pos 1, pos 1 -> pos 2
    perm.rotateRight(PositionIndex{0}, PositionIndex{1}, PositionIndex{2});

    // After: [2, 0, 1, 3, 4]
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{2}));
    ASSERT_EQ(3u, perm.elementAt(PositionIndex{3}));
    ASSERT_EQ(4u, perm.elementAt(PositionIndex{4}));
}

TEST(PermutationRotateLeftVariadic) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Initial: [0, 1, 2, 3, 4]
    // Rotate left positions 0, 1, 2: element at pos 0 -> pos 2, pos 1 -> pos 0, pos 2 -> pos 1
    perm.rotateLeft(PositionIndex{0}, PositionIndex{1}, PositionIndex{2});

    // After: [1, 2, 0, 3, 4]
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{2}));
    ASSERT_EQ(3u, perm.elementAt(PositionIndex{3}));
    ASSERT_EQ(4u, perm.elementAt(PositionIndex{4}));
}

TEST(PermutationRotateRightThenLeftRestores) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Rotate right then left should restore original
    perm.rotateRight(PositionIndex{0}, PositionIndex{1}, PositionIndex{2});
    perm.rotateLeft(PositionIndex{0}, PositionIndex{1}, PositionIndex{2});

    // Should be back to identity
    for (uint8_t i = 0; i < 5; i++) {
        ASSERT_EQ(i, perm.elementAt(PositionIndex{i}));
    }
}

TEST(PermutationRotateRightRuntime) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Runtime array-based rotation
    PositionIndex indices[] = {PositionIndex{0}, PositionIndex{1}, PositionIndex{2}};
    perm.rotateRight(indices, (size_t)3);

    // After: [2, 0, 1, 3, 4]
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{2}));
    ASSERT_EQ(3u, perm.elementAt(PositionIndex{3}));
    ASSERT_EQ(4u, perm.elementAt(PositionIndex{4}));
}

TEST(PermutationRotateLeftRuntime) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Runtime array-based rotation
    PositionIndex indices[] = {PositionIndex{0}, PositionIndex{1}, PositionIndex{2}};
    perm.rotateLeft(indices, (size_t)3);

    // After: [1, 2, 0, 3, 4]
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{2}));
    ASSERT_EQ(3u, perm.elementAt(PositionIndex{3}));
    ASSERT_EQ(4u, perm.elementAt(PositionIndex{4}));
}

TEST(PermutationRotateRuntimeWithPrefix) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Rotate with prefix: equivalent to rotateRight(pos0, pos1, pos2)
    PositionIndex indices[] = {PositionIndex{1}, PositionIndex{2}};
    perm.rotateRight(PositionIndex{0}, indices, (size_t)2);

    // After: [2, 0, 1, 3, 4]
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{2}));
}

TEST(PermutationRotateRuntimeWithSuffix) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Rotate with suffix: equivalent to rotateRight(pos0, pos1, pos2)
    PositionIndex indices[] = {PositionIndex{0}, PositionIndex{1}};
    perm.rotateRight(indices, (size_t)2, PositionIndex{2});

    // After: [2, 0, 1, 3, 4]
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{2}));
}

TEST(PermutationRotateRuntimeWithPrefixAndSuffix) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Rotate with prefix and suffix: equivalent to rotateRight(pos0, pos1, pos2)
    PositionIndex indices[] = {PositionIndex{1}};
    perm.rotateRight(PositionIndex{0}, indices, (size_t)1, PositionIndex{2});

    // After: [2, 0, 1, 3, 4]
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{2}));
}

TEST(PermutationRuntimeRotatePreservesArrayIndices) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Verify that the array indices are restored after rotation
    PositionIndex indices[] = {PositionIndex{0}, PositionIndex{1}, PositionIndex{2}};
    perm.rotateRight(indices, (size_t)3);

    // The indices array should be unchanged
    ASSERT_EQ(0u, indices[0].index);
    ASSERT_EQ(1u, indices[1].index);
    ASSERT_EQ(2u, indices[2].index);
}

TEST(PermutationMultipleOperations) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;
    using ElementIndex = DensePermutationFixture<5>::ElementIndex;

    // Perform a series of operations
    // Initial: [0, 1, 2, 3, 4]
    perm.swap(PositionIndex{0}, PositionIndex{4});  // [4, 1, 2, 3, 0]
    perm.rotateRight(PositionIndex{1}, PositionIndex{2}, PositionIndex{3});  // [4, 3, 1, 2, 0]

    ASSERT_EQ(4u, perm.elementAt(PositionIndex{0}));
    ASSERT_EQ(3u, perm.elementAt(PositionIndex{1}));
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{2}));
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{3}));
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{4}));

    // Verify positionOf is consistent
    ASSERT_EQ(4u, perm.positionOf(ElementIndex{0}));
    ASSERT_EQ(2u, perm.positionOf(ElementIndex{1}));
    ASSERT_EQ(3u, perm.positionOf(ElementIndex{2}));
    ASSERT_EQ(1u, perm.positionOf(ElementIndex{3}));
    ASSERT_EQ(0u, perm.positionOf(ElementIndex{4}));
}

// ==================== PermutationWindow Tests ====================

TEST(PermutationWindowConstruction) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    // Create a window over positions 2-5 (offset=2, width=4)
    PermutationWindow<8, DENSE> window(perm, 2, 4);

    // Window should see elements at positions 2, 3, 4, 5
    // In identity permutation, those are elements 2, 3, 4, 5
    ASSERT_EQ(2u, window.elementAt(PositionIndex{0}));  // Window position 0 = global position 2
    ASSERT_EQ(3u, window.elementAt(PositionIndex{1}));
    ASSERT_EQ(4u, window.elementAt(PositionIndex{2}));
    ASSERT_EQ(5u, window.elementAt(PositionIndex{3}));
}

TEST(PermutationWindowSwap) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    PermutationWindow<8, DENSE> window(perm, 2, 4);

    // Swap window positions 0 and 2 (global positions 2 and 4)
    window.swap(PositionIndex{0}, PositionIndex{2});

    // Window should now show: [4, 3, 2, 5]
    ASSERT_EQ(4u, window.elementAt(PositionIndex{0}));
    ASSERT_EQ(3u, window.elementAt(PositionIndex{1}));
    ASSERT_EQ(2u, window.elementAt(PositionIndex{2}));
    ASSERT_EQ(5u, window.elementAt(PositionIndex{3}));

    // Verify underlying permutation
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{0}));  // Unchanged
    ASSERT_EQ(1u, perm.elementAt(PositionIndex{1}));  // Unchanged
    ASSERT_EQ(4u, perm.elementAt(PositionIndex{2}));  // Swapped
    ASSERT_EQ(3u, perm.elementAt(PositionIndex{3}));  // Unchanged
    ASSERT_EQ(2u, perm.elementAt(PositionIndex{4}));  // Swapped
    ASSERT_EQ(5u, perm.elementAt(PositionIndex{5}));  // Unchanged
}

TEST(PermutationWindowRotateRightVariadic) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    PermutationWindow<8, DENSE> window(perm, 2, 4);

    // Rotate right window positions 0, 1, 2 (global positions 2, 3, 4)
    window.rotateRight(PositionIndex{0}, PositionIndex{1}, PositionIndex{2});

    // Window should now show: [4, 2, 3, 5]
    ASSERT_EQ(4u, window.elementAt(PositionIndex{0}));
    ASSERT_EQ(2u, window.elementAt(PositionIndex{1}));
    ASSERT_EQ(3u, window.elementAt(PositionIndex{2}));
    ASSERT_EQ(5u, window.elementAt(PositionIndex{3}));
}

TEST(PermutationWindowRotateRightRuntime) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    PermutationWindow<8, DENSE> window(perm, 2, 4);

    // Runtime rotation within window
    PositionIndex indices[] = {PositionIndex{0}, PositionIndex{1}, PositionIndex{2}};
    window.rotateRight(indices, 3);

    // Window should now show: [4, 2, 3, 5]
    ASSERT_EQ(4u, window.elementAt(PositionIndex{0}));
    ASSERT_EQ(2u, window.elementAt(PositionIndex{1}));
    ASSERT_EQ(3u, window.elementAt(PositionIndex{2}));
    ASSERT_EQ(5u, window.elementAt(PositionIndex{3}));

    // Verify array indices are preserved
    ASSERT_EQ(0u, indices[0].index);
    ASSERT_EQ(1u, indices[1].index);
    ASSERT_EQ(2u, indices[2].index);
}

TEST(PermutationWindowInWindow) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using ElementIndex = DensePermutationFixture<8>::ElementIndex;

    PermutationWindow<8, DENSE> window(perm, 2, 4);

    // In identity permutation, elements 2-5 are in the window
    ASSERT_FALSE(window.inWindow(ElementIndex{0}));
    ASSERT_FALSE(window.inWindow(ElementIndex{1}));
    ASSERT_TRUE(window.inWindow(ElementIndex{2}));
    ASSERT_TRUE(window.inWindow(ElementIndex{3}));
    ASSERT_TRUE(window.inWindow(ElementIndex{4}));
    ASSERT_TRUE(window.inWindow(ElementIndex{5}));
    ASSERT_FALSE(window.inWindow(ElementIndex{6}));
    ASSERT_FALSE(window.inWindow(ElementIndex{7}));
}

TEST(PermutationWindowPositionOf) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using ElementIndex = DensePermutationFixture<8>::ElementIndex;

    PermutationWindow<8, DENSE> window(perm, 2, 4);

    // positionOf should return window-relative position
    ASSERT_EQ(0u, window.positionOf(ElementIndex{2}));  // Element 2 is at global pos 2, window pos 0
    ASSERT_EQ(1u, window.positionOf(ElementIndex{3}));
    ASSERT_EQ(2u, window.positionOf(ElementIndex{4}));
    ASSERT_EQ(3u, window.positionOf(ElementIndex{5}));
}

TEST(PermutationWindowFullWidth) {
    DensePermutationFixture<5> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<5>::PositionIndex;

    // Create a window covering the entire permutation
    PermutationWindow<8, DENSE> window(perm);

    // Should behave identically to the underlying permutation
    window.rotateRight(PositionIndex{0}, PositionIndex{1}, PositionIndex{2});

    ASSERT_EQ(2u, window.elementAt(PositionIndex{0}));
    ASSERT_EQ(0u, window.elementAt(PositionIndex{1}));
    ASSERT_EQ(1u, window.elementAt(PositionIndex{2}));
}

// ==================== Sparse Permutation Tests ====================

TEST(SparsePermutationConstruction) {
    using PermType = Permutation<8, SPARSE>;
    using IndexType = PermType::IndexType;
    using PositionIndex = PermType::PositionIndex;

    IndexType forwardBuffer[8];   // 8 positions
    IndexType backwardBuffer[5];  // 5 elements

    PermType perm(forwardBuffer, 8, backwardBuffer, 5);
    perm.reset();

    ASSERT_EQ(5u, perm.elementCount());
    ASSERT_EQ(8u, perm.positionCount());

    // First 5 positions should have elements 0-4
    for (uint8_t i = 0; i < 5; i++) {
        ASSERT_EQ(i, perm.elementAt(PositionIndex{i}));
    }

    // Positions 5-7 should be invalid
    ASSERT_EQ(PermType::INVALID, perm.elementAt(PositionIndex{5}));
    ASSERT_EQ(PermType::INVALID, perm.elementAt(PositionIndex{6}));
    ASSERT_EQ(PermType::INVALID, perm.elementAt(PositionIndex{7}));
}

TEST(SparsePermutationSwap) {
    using PermType = Permutation<8, SPARSE>;
    using IndexType = PermType::IndexType;
    using PositionIndex = PermType::PositionIndex;

    IndexType forwardBuffer[8];
    IndexType backwardBuffer[5];

    PermType perm(forwardBuffer, 8, backwardBuffer, 5);
    perm.reset();

    // Move element from position 0 to position 5 (previously invalid)
    perm.swap(PositionIndex{0}, PositionIndex{5});

    ASSERT_EQ(PermType::INVALID, perm.elementAt(PositionIndex{0}));  // Now invalid
    ASSERT_EQ(0u, perm.elementAt(PositionIndex{5}));  // Element 0 moved here
}

// ==================== Validation Tests ====================

TEST(PermutationValidateAfterReset) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;

    ASSERT_TRUE(perm.validate());
}

TEST(PermutationValidateAfterSwap) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    perm.swap(PositionIndex{0}, PositionIndex{7});
    ASSERT_TRUE(perm.validate());

    perm.swap(PositionIndex{1}, PositionIndex{6});
    ASSERT_TRUE(perm.validate());

    perm.swap(PositionIndex{2}, PositionIndex{5});
    ASSERT_TRUE(perm.validate());
}

TEST(PermutationValidateAfterRotations) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    perm.rotateRight(PositionIndex{0}, PositionIndex{1}, PositionIndex{2}, PositionIndex{3});
    ASSERT_TRUE(perm.validate());

    perm.rotateLeft(PositionIndex{4}, PositionIndex{5}, PositionIndex{6}, PositionIndex{7});
    ASSERT_TRUE(perm.validate());

    // Mix rotations
    perm.rotateRight(PositionIndex{0}, PositionIndex{4});
    ASSERT_TRUE(perm.validate());
}

// ==================== Stress Tests ====================

TEST(PermutationStressRandomSwaps) {
    DensePermutationFixture<32> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<32>::PositionIndex;

    // Simple LCG for deterministic "random" numbers
    uint32_t seed = 12345;
    auto nextRand = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return seed;
    };

    // Perform many random swaps
    for (int i = 0; i < 1000; i++) {
        uint8_t a = nextRand() % 32;
        uint8_t b = nextRand() % 32;
        perm.swap(PositionIndex{a}, PositionIndex{b});

        // Validate periodically
        if (i % 100 == 0) {
            ASSERT_TRUE(perm.validate());
        }
    }

    ASSERT_TRUE(perm.validate());
}

TEST(PermutationStressRandomRotations) {
    DensePermutationFixture<32> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<32>::PositionIndex;

    uint32_t seed = 67890;
    auto nextRand = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return seed;
    };

    // Perform many random rotations (variadic)
    for (int i = 0; i < 500; i++) {
        uint8_t a = nextRand() % 32;
        uint8_t b = nextRand() % 32;
        uint8_t c = nextRand() % 32;

        // Ensure distinct indices
        while (b == a) b = nextRand() % 32;
        while (c == a || c == b) c = nextRand() % 32;

        if (nextRand() % 2 == 0) {
            perm.rotateRight(PositionIndex{a}, PositionIndex{b}, PositionIndex{c});
        } else {
            perm.rotateLeft(PositionIndex{a}, PositionIndex{b}, PositionIndex{c});
        }

        if (i % 50 == 0) {
            ASSERT_TRUE(perm.validate());
        }
    }

    ASSERT_TRUE(perm.validate());
}

TEST(PermutationStressRuntimeRotations) {
    DensePermutationFixture<32> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<32>::PositionIndex;

    uint32_t seed = 11111;
    auto nextRand = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return seed;
    };

    // Perform runtime-length rotations
    for (int i = 0; i < 300; i++) {
        // Generate 3-6 distinct indices
        size_t len = 3 + (nextRand() % 4);
        PositionIndex indices[6];
        uint8_t used[32] = {0};

        for (size_t j = 0; j < len; j++) {
            uint8_t idx;
            do {
                idx = nextRand() % 32;
            } while (used[idx]);
            used[idx] = 1;
            indices[j] = PositionIndex{idx};
        }

        if (nextRand() % 2 == 0) {
            perm.rotateRight(indices, len);
        } else {
            perm.rotateLeft(indices, len);
        }

        if (i % 30 == 0) {
            ASSERT_TRUE(perm.validate());
        }
    }

    ASSERT_TRUE(perm.validate());
}

TEST(PermutationStressMixedOperations) {
    DensePermutationFixture<16> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<16>::PositionIndex;
    using ElementIndex = DensePermutationFixture<16>::ElementIndex;

    uint32_t seed = 99999;
    auto nextRand = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return seed;
    };

    // Mix of all operations
    for (int i = 0; i < 500; i++) {
        int op = nextRand() % 6;

        switch (op) {
            case 0: {  // Swap by position
                uint8_t a = nextRand() % 16;
                uint8_t b = nextRand() % 16;
                perm.swap(PositionIndex{a}, PositionIndex{b});
                break;
            }
            case 1: {  // Swap by element
                uint8_t a = nextRand() % 16;
                uint8_t b = nextRand() % 16;
                perm.swap(ElementIndex{a}, ElementIndex{b});
                break;
            }
            case 2: {  // Variadic rotateRight
                uint8_t a = nextRand() % 16;
                uint8_t b = nextRand() % 16;
                while (b == a) b = nextRand() % 16;
                perm.rotateRight(PositionIndex{a}, PositionIndex{b});
                break;
            }
            case 3: {  // Variadic rotateLeft
                uint8_t a = nextRand() % 16;
                uint8_t b = nextRand() % 16;
                uint8_t c = nextRand() % 16;
                while (b == a) b = nextRand() % 16;
                while (c == a || c == b) c = nextRand() % 16;
                perm.rotateLeft(PositionIndex{a}, PositionIndex{b}, PositionIndex{c});
                break;
            }
            case 4: {  // Runtime rotateRight with prefix
                uint8_t prefix = nextRand() % 16;
                uint8_t idx1 = nextRand() % 16;
                while (idx1 == prefix) idx1 = nextRand() % 16;
                PositionIndex arr[] = {PositionIndex{idx1}};
                perm.rotateRight(PositionIndex{prefix}, arr, (size_t)1);
                break;
            }
            case 5: {  // Runtime rotateLeft with suffix
                uint8_t idx1 = nextRand() % 16;
                uint8_t suffix = nextRand() % 16;
                while (suffix == idx1) suffix = nextRand() % 16;
                PositionIndex arr[] = {PositionIndex{idx1}};
                perm.rotateLeft(arr, (size_t)1, PositionIndex{suffix});
                break;
            }
        }

        // Validate after every operation
        ASSERT_TRUE(perm.validate());
    }
}

TEST(PermutationWindowStressMixedOperations) {
    DensePermutationFixture<32> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<32>::PositionIndex;

    // Create a window over positions 8-23 (offset=8, width=16)
    PermutationWindow<8, DENSE> window(perm, 8, 16);

    uint32_t seed = 77777;
    auto nextRand = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return seed;
    };

    for (int i = 0; i < 300; i++) {
        int op = nextRand() % 4;

        switch (op) {
            case 0: {  // Swap within window
                uint8_t a = nextRand() % 16;
                uint8_t b = nextRand() % 16;
                window.swap(PositionIndex{a}, PositionIndex{b});
                break;
            }
            case 1: {  // Variadic rotateRight within window
                uint8_t a = nextRand() % 16;
                uint8_t b = nextRand() % 16;
                while (b == a) b = nextRand() % 16;
                window.rotateRight(PositionIndex{a}, PositionIndex{b});
                break;
            }
            case 2: {  // Variadic rotateLeft within window
                uint8_t a = nextRand() % 16;
                uint8_t b = nextRand() % 16;
                uint8_t c = nextRand() % 16;
                while (b == a) b = nextRand() % 16;
                while (c == a || c == b) c = nextRand() % 16;
                window.rotateLeft(PositionIndex{a}, PositionIndex{b}, PositionIndex{c});
                break;
            }
            case 3: {  // Runtime rotation within window
                uint8_t a = nextRand() % 16;
                uint8_t b = nextRand() % 16;
                while (b == a) b = nextRand() % 16;
                PositionIndex arr[] = {PositionIndex{a}, PositionIndex{b}};
                if (nextRand() % 2 == 0) {
                    window.rotateRight(arr, (size_t)2);
                } else {
                    window.rotateLeft(arr, (size_t)2);
                }
                break;
            }
        }

        ASSERT_TRUE(window.validate());
    }
}

TEST(PermutationStressRotateRightThenLeftRestores) {
    DensePermutationFixture<16> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<16>::PositionIndex;
    using IndexType = DensePermutationFixture<16>::IndexType;

    // Save initial state
    IndexType initialForward[16];
    for (int i = 0; i < 16; i++) {
        initialForward[i] = perm.elementAt(PositionIndex{static_cast<uint8_t>(i)});
    }

    uint32_t seed = 55555;
    auto nextRand = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return seed;
    };

    // Do random rotations and their inverses
    for (int i = 0; i < 100; i++) {
        uint8_t a = nextRand() % 16;
        uint8_t b = nextRand() % 16;
        uint8_t c = nextRand() % 16;
        uint8_t d = nextRand() % 16;
        while (b == a) b = nextRand() % 16;
        while (c == a || c == b) c = nextRand() % 16;
        while (d == a || d == b || d == c) d = nextRand() % 16;

        // Rotate right then left should restore
        perm.rotateRight(PositionIndex{a}, PositionIndex{b}, PositionIndex{c}, PositionIndex{d});
        perm.rotateLeft(PositionIndex{a}, PositionIndex{b}, PositionIndex{c}, PositionIndex{d});

        // Verify state is unchanged
        for (int j = 0; j < 16; j++) {
            ASSERT_EQ(initialForward[j], perm.elementAt(PositionIndex{static_cast<uint8_t>(j)}));
        }
    }
}

// ==================== Duplicate Detection Tests ====================

// Note: In test builds, assert() throws an exception, so we can catch it.
// The PERM_SKIP_EXPENSIVE_ASSERT macro skips duplicate checking for
// rotations with more than 5 elements.

TEST(PermutationRotateRightVariadicDuplicateAsserts) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    // Duplicate position should trigger assertion
    bool assertFired = false;
    try {
        perm.rotateRight(PositionIndex{1}, PositionIndex{2}, PositionIndex{1});  // 1 appears twice
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);

    // Permutation should still be valid (operation was aborted)
    ASSERT_TRUE(perm.validate());
}

TEST(PermutationRotateLeftVariadicDuplicateAsserts) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    bool assertFired = false;
    try {
        perm.rotateLeft(PositionIndex{3}, PositionIndex{3}, PositionIndex{4});  // 3 appears twice
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
    ASSERT_TRUE(perm.validate());
}

TEST(PermutationRotateRightRuntimeDuplicateAsserts) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    // Runtime rotation with duplicate in array
    PositionIndex indices[] = {PositionIndex{0}, PositionIndex{1}, PositionIndex{0}};  // 0 appears twice
    bool assertFired = false;
    try {
        perm.rotateRight(indices, (size_t)3);
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
    ASSERT_TRUE(perm.validate());
}

TEST(PermutationRotateLeftRuntimeDuplicateAsserts) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    PositionIndex indices[] = {PositionIndex{5}, PositionIndex{6}, PositionIndex{5}};  // 5 appears twice
    bool assertFired = false;
    try {
        perm.rotateLeft(indices, (size_t)3);
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
    ASSERT_TRUE(perm.validate());
}

TEST(PermutationRotateRuntimeWithPrefixDuplicateAsserts) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    // Prefix duplicates an element in the array
    PositionIndex indices[] = {PositionIndex{1}, PositionIndex{2}};
    bool assertFired = false;
    try {
        perm.rotateRight(PositionIndex{1}, indices, (size_t)2);  // prefix 1 duplicates indices[0]
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
    ASSERT_TRUE(perm.validate());
}

TEST(PermutationRotateRuntimeWithSuffixDuplicateAsserts) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    // Suffix duplicates an element in the array
    PositionIndex indices[] = {PositionIndex{3}, PositionIndex{4}};
    bool assertFired = false;
    try {
        perm.rotateLeft(indices, (size_t)2, PositionIndex{4});  // suffix 4 duplicates indices[1]
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
    ASSERT_TRUE(perm.validate());
}

TEST(PermutationRotateRuntimePrefixSuffixDuplicateAsserts) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    // Prefix and suffix are the same
    PositionIndex indices[] = {PositionIndex{1}};
    bool assertFired = false;
    try {
        perm.rotateRight(PositionIndex{0}, indices, (size_t)1, PositionIndex{0});  // prefix == suffix
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
    ASSERT_TRUE(perm.validate());
}

TEST(PermutationWindowRotateDuplicateAsserts) {
    DensePermutationFixture<16> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<16>::PositionIndex;

    PermutationWindow<8, DENSE> window(perm, 4, 8);

    // Duplicate in window rotation
    bool assertFired = false;
    try {
        window.rotateRight(PositionIndex{0}, PositionIndex{1}, PositionIndex{0});  // 0 appears twice
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
    ASSERT_TRUE(window.validate());
}

TEST(PermutationWindowRotateRuntimeDuplicateAsserts) {
    DensePermutationFixture<16> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<16>::PositionIndex;

    PermutationWindow<8, DENSE> window(perm, 4, 8);

    PositionIndex indices[] = {PositionIndex{2}, PositionIndex{3}, PositionIndex{2}};  // 2 appears twice
    bool assertFired = false;
    try {
        window.rotateRight(indices, (size_t)3);
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
    ASSERT_TRUE(window.validate());
}

TEST(PermutationRotateDistinctElementsSucceed) {
    // Verify that rotations with distinct elements do NOT assert
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    bool assertFired = false;
    try {
        // All distinct - should work
        perm.rotateRight(PositionIndex{0}, PositionIndex{1});
        perm.rotateRight(PositionIndex{2}, PositionIndex{3}, PositionIndex{4});
        perm.rotateLeft(PositionIndex{5}, PositionIndex{6}, PositionIndex{7});

        // Rotation of 5 elements (still checked)
        perm.rotateRight(PositionIndex{0}, PositionIndex{2}, PositionIndex{4},
                         PositionIndex{6}, PositionIndex{1});

        // Runtime rotations
        PositionIndex indices[] = {PositionIndex{0}, PositionIndex{1}, PositionIndex{2}};
        perm.rotateRight(indices, (size_t)3);

        PositionIndex indicesMiddle[] = {PositionIndex{4}};
        perm.rotateRight(PositionIndex{3}, indicesMiddle, (size_t)1, PositionIndex{5});
    } catch (...) {
        assertFired = true;
    }

    ASSERT_FALSE(assertFired);
    ASSERT_TRUE(perm.validate());
}

TEST(PermutationSwapSelfDoesNotAssert) {
    // Swapping an element with itself is a no-op, not an error
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;

    bool assertFired = false;
    try {
        perm.swap(PositionIndex{3}, PositionIndex{3});
    } catch (...) {
        assertFired = true;
    }

    ASSERT_FALSE(assertFired);
    ASSERT_TRUE(perm.validate());
}

// Test that after many operations, we can always get back to identity
TEST(PermutationCanRestoreIdentity) {
    DensePermutationFixture<8> fixture;
    auto& perm = fixture.perm;
    using PositionIndex = DensePermutationFixture<8>::PositionIndex;
    using ElementIndex = DensePermutationFixture<8>::ElementIndex;

    // Scramble the permutation
    perm.swap(PositionIndex{0}, PositionIndex{7});
    perm.swap(PositionIndex{1}, PositionIndex{6});
    perm.rotateRight(PositionIndex{2}, PositionIndex{3}, PositionIndex{4}, PositionIndex{5});
    perm.rotateLeft(PositionIndex{0}, PositionIndex{2}, PositionIndex{4}, PositionIndex{6});

    ASSERT_TRUE(perm.validate());

    // Now restore by moving each element to its correct position
    // Using selection sort-like approach
    for (uint8_t pos = 0; pos < 8; pos++) {
        // Find where element 'pos' currently is
        uint8_t currentPos = perm.positionOf(ElementIndex{pos});
        if (currentPos != pos) {
            // Swap it into place
            perm.swap(PositionIndex{pos}, PositionIndex{currentPos});
        }
    }

    // Should now be identity
    for (uint8_t i = 0; i < 8; i++) {
        ASSERT_EQ(i, perm.elementAt(PositionIndex{i}));
        ASSERT_EQ(i, perm.positionOf(ElementIndex{i}));
    }

    ASSERT_TRUE(perm.validate());
}

// ==================== BucketedSet Tests ====================

// Helper fixture for BucketedSet tests
template <size_t N, size_t NumBuckets>
struct BucketedSetFixture {
    using PermType = Permutation<8, DENSE>;
    using IndexType = PermType::IndexType;
    using PositionIndex = PermType::PositionIndex;
    using ElementIndex = PermType::ElementIndex;

    IndexType forwardBuffer[N];
    IndexType backwardBuffer[N];
    IndexType markers[NumBuckets - 1];
    PermType perm;
    PermutationWindow<8, DENSE> window;
    BucketedSet<8, DENSE> buckets;

    BucketedSetFixture()
        : perm(forwardBuffer, backwardBuffer, N)
        , window(perm)
        , buckets(window, markers, NumBuckets)
    {
        perm.reset();
        buckets.reset();
    }

    // Helper to compute total elements across all buckets
    size_t totalElements() {
        size_t total = 0;
        for (size_t i = 0; i < NumBuckets; ++i) {
            total += buckets.bucketSize(i);
        }
        return total;
    }

    // Helper to verify bucket sizes sum to N
    bool validateBucketSizes() {
        return totalElements() == N;
    }
};

// ==================== Basic Construction and Query Tests ====================

TEST(BucketedSetConstruction) {
    BucketedSetFixture<12, 4> fixture;
    auto& buckets = fixture.buckets;

    // After reset, all elements should be in the last bucket
    ASSERT_EQ(0u, buckets.bucketSize(0));
    ASSERT_EQ(0u, buckets.bucketSize(1));
    ASSERT_EQ(0u, buckets.bucketSize(2));
    ASSERT_EQ(12u, buckets.bucketSize(3));

    ASSERT_TRUE(fixture.validateBucketSizes());
}

TEST(BucketedSetContains) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 3>::ElementIndex;

    // All elements 0-7 should be contained
    for (uint8_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(buckets.contains(ElementIndex{i}));
    }
}

TEST(BucketedSetGetBucketAfterReset) {
    BucketedSetFixture<10, 4> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<10, 4>::ElementIndex;

    // After reset, all elements are in the last bucket
    for (uint8_t i = 0; i < 10; ++i) {
        ASSERT_EQ(3u, buckets.getBucket(ElementIndex{i}));
    }
}

TEST(BucketedSetBucketEmpty) {
    BucketedSetFixture<8, 4> fixture;
    auto& buckets = fixture.buckets;

    // After reset, only the last bucket is non-empty
    ASSERT_TRUE(buckets.bucketEmpty(0));
    ASSERT_TRUE(buckets.bucketEmpty(1));
    ASSERT_TRUE(buckets.bucketEmpty(2));
    ASSERT_FALSE(buckets.bucketEmpty(3));
}

// ==================== Bucket Boundary Tests ====================

TEST(BucketedSetBucketBoundaries) {
    BucketedSetFixture<12, 4> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<12, 4>::ElementIndex;

    // Move some elements to create non-empty buckets
    // Move elements 0, 1, 2 to bucket 0
    buckets.moveToBucket(ElementIndex{0}, 0);
    buckets.moveToBucket(ElementIndex{1}, 0);
    buckets.moveToBucket(ElementIndex{2}, 0);

    // Move elements 3, 4 to bucket 1
    buckets.moveToBucket(ElementIndex{3}, 1);
    buckets.moveToBucket(ElementIndex{4}, 1);

    // Move element 5 to bucket 2
    buckets.moveToBucket(ElementIndex{5}, 2);

    // Elements 6-11 remain in bucket 3

    ASSERT_EQ(3u, buckets.bucketSize(0));
    ASSERT_EQ(2u, buckets.bucketSize(1));
    ASSERT_EQ(1u, buckets.bucketSize(2));
    ASSERT_EQ(6u, buckets.bucketSize(3));

    // Check bucket boundaries
    ASSERT_EQ(0u, buckets.bucketStart(0).index);
    ASSERT_EQ(3u, buckets.bucketEnd(0).index);

    ASSERT_EQ(3u, buckets.bucketStart(1).index);
    ASSERT_EQ(5u, buckets.bucketEnd(1).index);

    ASSERT_EQ(5u, buckets.bucketStart(2).index);
    ASSERT_EQ(6u, buckets.bucketEnd(2).index);

    ASSERT_EQ(6u, buckets.bucketStart(3).index);
    ASSERT_EQ(12u, buckets.bucketEnd(3).index);

    ASSERT_TRUE(fixture.validateBucketSizes());
}

TEST(BucketedSetTopAndBottomOfBucket) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 3>::ElementIndex;

    // Move elements to create structure: bucket 0 has 3, bucket 1 has 2, bucket 2 has 3
    buckets.moveToBucket(ElementIndex{0}, 0);
    buckets.moveToBucket(ElementIndex{1}, 0);
    buckets.moveToBucket(ElementIndex{2}, 0);
    buckets.moveToBucket(ElementIndex{3}, 1);
    buckets.moveToBucket(ElementIndex{4}, 1);

    // Verify top and bottom exist and are in correct buckets
    ElementIndex top0 = buckets.topOfBucket(0);
    ElementIndex bottom0 = buckets.bottomOfBucket(0);
    ASSERT_EQ(0u, buckets.getBucket(top0));
    ASSERT_EQ(0u, buckets.getBucket(bottom0));

    ElementIndex top1 = buckets.topOfBucket(1);
    ElementIndex bottom1 = buckets.bottomOfBucket(1);
    ASSERT_EQ(1u, buckets.getBucket(top1));
    ASSERT_EQ(1u, buckets.getBucket(bottom1));

    ElementIndex top2 = buckets.topOfBucket(2);
    ElementIndex bottom2 = buckets.bottomOfBucket(2);
    ASSERT_EQ(2u, buckets.getBucket(top2));
    ASSERT_EQ(2u, buckets.getBucket(bottom2));
}

// ==================== Transfer Tests (Adjacent Buckets) ====================

TEST(BucketedSetTransferToNextBucket) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 3>::ElementIndex;

    // Move all elements to bucket 0
    for (uint8_t i = 0; i < 8; ++i) {
        buckets.moveToBucket(ElementIndex{i}, 0);
    }

    ASSERT_EQ(8u, buckets.bucketSize(0));
    ASSERT_EQ(0u, buckets.bucketSize(1));
    ASSERT_EQ(0u, buckets.bucketSize(2));

    // Transfer one element from bucket 0 to bucket 1
    ElementIndex transferred = buckets.transferToNextBucket(0);

    ASSERT_EQ(7u, buckets.bucketSize(0));
    ASSERT_EQ(1u, buckets.bucketSize(1));
    ASSERT_EQ(0u, buckets.bucketSize(2));

    // The transferred element should now be in bucket 1
    ASSERT_EQ(1u, buckets.getBucket(transferred));

    // It should be at the top of bucket 1
    ASSERT_EQ(transferred.index, buckets.topOfBucket(1).index);

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetTransferToPrevBucket) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 3>::ElementIndex;

    // All elements start in bucket 2 (after reset)
    ASSERT_EQ(8u, buckets.bucketSize(2));

    // Transfer one element from bucket 2 to bucket 1
    ElementIndex transferred = buckets.transferToPrevBucket(2);

    ASSERT_EQ(0u, buckets.bucketSize(0));
    ASSERT_EQ(1u, buckets.bucketSize(1));
    ASSERT_EQ(7u, buckets.bucketSize(2));

    // The transferred element should now be in bucket 1
    ASSERT_EQ(1u, buckets.getBucket(transferred));

    // It should be at the bottom of bucket 1
    ASSERT_EQ(transferred.index, buckets.bottomOfBucket(1).index);

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMultipleTransfers) {
    BucketedSetFixture<10, 4> fixture;
    auto& buckets = fixture.buckets;

    // All elements start in bucket 3
    ASSERT_EQ(10u, buckets.bucketSize(3));

    // Transfer 3 elements to bucket 2
    buckets.transferToPrevBucket(3);
    buckets.transferToPrevBucket(3);
    buckets.transferToPrevBucket(3);

    ASSERT_EQ(0u, buckets.bucketSize(0));
    ASSERT_EQ(0u, buckets.bucketSize(1));
    ASSERT_EQ(3u, buckets.bucketSize(2));
    ASSERT_EQ(7u, buckets.bucketSize(3));

    // Transfer 2 from bucket 2 to bucket 1
    buckets.transferToPrevBucket(2);
    buckets.transferToPrevBucket(2);

    ASSERT_EQ(0u, buckets.bucketSize(0));
    ASSERT_EQ(2u, buckets.bucketSize(1));
    ASSERT_EQ(1u, buckets.bucketSize(2));
    ASSERT_EQ(7u, buckets.bucketSize(3));

    // Transfer 1 from bucket 1 to bucket 0
    buckets.transferToPrevBucket(1);

    ASSERT_EQ(1u, buckets.bucketSize(0));
    ASSERT_EQ(1u, buckets.bucketSize(1));
    ASSERT_EQ(1u, buckets.bucketSize(2));
    ASSERT_EQ(7u, buckets.bucketSize(3));

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

// ==================== moveToBucket Tests ====================

TEST(BucketedSetMoveToBucketSameBucket) {
    BucketedSetFixture<8, 4> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 4>::ElementIndex;

    // All elements in bucket 3
    size_t sizeBefore = buckets.bucketSize(3);

    // Moving to same bucket should be no-op
    buckets.moveToBucket(ElementIndex{0}, 3);

    ASSERT_EQ(sizeBefore, buckets.bucketSize(3));
    ASSERT_EQ(3u, buckets.getBucket(ElementIndex{0}));

    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMoveToBucketOneStepHigher) {
    BucketedSetFixture<8, 4> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 4>::ElementIndex;

    // Move element 0 to bucket 0 first
    buckets.moveToBucket(ElementIndex{0}, 0);
    ASSERT_EQ(0u, buckets.getBucket(ElementIndex{0}));
    ASSERT_EQ(1u, buckets.bucketSize(0));

    // Move it one step higher to bucket 1
    buckets.moveToBucket(ElementIndex{0}, 1);
    ASSERT_EQ(1u, buckets.getBucket(ElementIndex{0}));
    ASSERT_EQ(0u, buckets.bucketSize(0));
    ASSERT_EQ(1u, buckets.bucketSize(1));

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMoveToBucketOneStepLower) {
    BucketedSetFixture<8, 4> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 4>::ElementIndex;

    // All elements in bucket 3, move one to bucket 2
    buckets.moveToBucket(ElementIndex{0}, 2);
    ASSERT_EQ(2u, buckets.getBucket(ElementIndex{0}));
    ASSERT_EQ(1u, buckets.bucketSize(2));
    ASSERT_EQ(7u, buckets.bucketSize(3));

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMoveToBucketMultipleStepsHigher) {
    BucketedSetFixture<12, 5> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<12, 5>::ElementIndex;

    // Move element from bucket 4 (last) to bucket 0 (first)
    buckets.moveToBucket(ElementIndex{5}, 0);
    ASSERT_EQ(0u, buckets.getBucket(ElementIndex{5}));
    ASSERT_EQ(1u, buckets.bucketSize(0));
    ASSERT_EQ(11u, buckets.bucketSize(4));

    // Move another from bucket 4 to bucket 1
    buckets.moveToBucket(ElementIndex{6}, 1);
    ASSERT_EQ(1u, buckets.getBucket(ElementIndex{6}));
    ASSERT_EQ(1u, buckets.bucketSize(1));
    ASSERT_EQ(10u, buckets.bucketSize(4));

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMoveToBucketMultipleStepsLower) {
    BucketedSetFixture<12, 5> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<12, 5>::ElementIndex;

    // First move some elements to bucket 0
    buckets.moveToBucket(ElementIndex{0}, 0);
    buckets.moveToBucket(ElementIndex{1}, 0);
    buckets.moveToBucket(ElementIndex{2}, 0);

    ASSERT_EQ(3u, buckets.bucketSize(0));
    ASSERT_EQ(9u, buckets.bucketSize(4));

    // Now move one from bucket 0 to bucket 4 (multiple steps higher)
    buckets.moveToBucket(ElementIndex{1}, 4);
    ASSERT_EQ(4u, buckets.getBucket(ElementIndex{1}));
    ASSERT_EQ(2u, buckets.bucketSize(0));
    ASSERT_EQ(10u, buckets.bucketSize(4));

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMoveToBucketWithExpectedSource) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 3>::ElementIndex;

    // Element 0 is in bucket 2
    ASSERT_EQ(2u, buckets.getBucket(ElementIndex{0}));

    // Move with correct expected source
    buckets.moveToBucket(ElementIndex{0}, 2, 0);
    ASSERT_EQ(0u, buckets.getBucket(ElementIndex{0}));

    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMoveToBucketWithWrongExpectedSourceAsserts) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 3>::ElementIndex;

    // Element 0 is in bucket 2
    ASSERT_EQ(2u, buckets.getBucket(ElementIndex{0}));

    // Move with wrong expected source should assert
    bool assertFired = false;
    try {
        buckets.moveToBucket(ElementIndex{0}, 0, 1);  // Wrong: element is in 2, not 0
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
}

// ==================== Edge Cases ====================

TEST(BucketedSetSingleBucket) {
    BucketedSetFixture<8, 1> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 1>::ElementIndex;

    // With single bucket, all elements are always in bucket 0
    ASSERT_EQ(8u, buckets.bucketSize(0));

    for (uint8_t i = 0; i < 8; ++i) {
        ASSERT_EQ(0u, buckets.getBucket(ElementIndex{i}));
    }

    // Moving to same bucket should be no-op
    buckets.moveToBucket(ElementIndex{0}, 0);
    ASSERT_EQ(8u, buckets.bucketSize(0));
}

TEST(BucketedSetTwoBuckets) {
    BucketedSetFixture<6, 2> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<6, 2>::ElementIndex;

    // All elements in bucket 1
    ASSERT_EQ(0u, buckets.bucketSize(0));
    ASSERT_EQ(6u, buckets.bucketSize(1));

    // Move half to bucket 0
    buckets.moveToBucket(ElementIndex{0}, 0);
    buckets.moveToBucket(ElementIndex{1}, 0);
    buckets.moveToBucket(ElementIndex{2}, 0);

    ASSERT_EQ(3u, buckets.bucketSize(0));
    ASSERT_EQ(3u, buckets.bucketSize(1));

    // Move them back
    buckets.moveToBucket(ElementIndex{0}, 1);
    buckets.moveToBucket(ElementIndex{1}, 1);
    buckets.moveToBucket(ElementIndex{2}, 1);

    ASSERT_EQ(0u, buckets.bucketSize(0));
    ASSERT_EQ(6u, buckets.bucketSize(1));

    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMoveElementAtBoundary) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 3>::ElementIndex;

    // Create structure: bucket 0 has 3 elements
    buckets.moveToBucket(ElementIndex{0}, 0);
    buckets.moveToBucket(ElementIndex{1}, 0);
    buckets.moveToBucket(ElementIndex{2}, 0);

    // Get the element at the top (first) of bucket 0
    ElementIndex top = buckets.topOfBucket(0);

    // Move the top element to bucket 1
    buckets.moveToBucket(top, 1);
    ASSERT_EQ(1u, buckets.getBucket(top));
    ASSERT_EQ(2u, buckets.bucketSize(0));
    ASSERT_EQ(1u, buckets.bucketSize(1));

    // Get the element at the bottom (last) of bucket 0
    ElementIndex bottom = buckets.bottomOfBucket(0);

    // Move the bottom element to bucket 2
    buckets.moveToBucket(bottom, 2);
    ASSERT_EQ(2u, buckets.getBucket(bottom));
    ASSERT_EQ(1u, buckets.bucketSize(0));
    ASSERT_EQ(6u, buckets.bucketSize(2));

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMoveAcrossEmptyBuckets) {
    BucketedSetFixture<8, 5> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 5>::ElementIndex;

    // All elements in bucket 4
    // Buckets 0, 1, 2, 3 are empty

    // Move element directly to bucket 0 (across 4 empty buckets)
    buckets.moveToBucket(ElementIndex{0}, 0);
    ASSERT_EQ(0u, buckets.getBucket(ElementIndex{0}));
    ASSERT_EQ(1u, buckets.bucketSize(0));

    // Buckets 1, 2, 3 should still be empty
    ASSERT_TRUE(buckets.bucketEmpty(1));
    ASSERT_TRUE(buckets.bucketEmpty(2));
    ASSERT_TRUE(buckets.bucketEmpty(3));

    // Now move it back to bucket 4 (across empty buckets again)
    buckets.moveToBucket(ElementIndex{0}, 4);
    ASSERT_EQ(4u, buckets.getBucket(ElementIndex{0}));
    ASSERT_EQ(8u, buckets.bucketSize(4));
    ASSERT_TRUE(buckets.bucketEmpty(0));

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetMoveAllElementsToBucket0) {
    BucketedSetFixture<8, 4> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 4>::ElementIndex;

    // Move all elements to bucket 0
    for (uint8_t i = 0; i < 8; ++i) {
        buckets.moveToBucket(ElementIndex{i}, 0);
    }

    ASSERT_EQ(8u, buckets.bucketSize(0));
    ASSERT_EQ(0u, buckets.bucketSize(1));
    ASSERT_EQ(0u, buckets.bucketSize(2));
    ASSERT_EQ(0u, buckets.bucketSize(3));

    // All elements should report bucket 0
    for (uint8_t i = 0; i < 8; ++i) {
        ASSERT_EQ(0u, buckets.getBucket(ElementIndex{i}));
    }

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetDistributeElementsEvenly) {
    BucketedSetFixture<12, 4> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<12, 4>::ElementIndex;

    // Distribute: 3 elements per bucket
    // Elements 0-2 to bucket 0
    buckets.moveToBucket(ElementIndex{0}, 0);
    buckets.moveToBucket(ElementIndex{1}, 0);
    buckets.moveToBucket(ElementIndex{2}, 0);

    // Elements 3-5 to bucket 1
    buckets.moveToBucket(ElementIndex{3}, 1);
    buckets.moveToBucket(ElementIndex{4}, 1);
    buckets.moveToBucket(ElementIndex{5}, 1);

    // Elements 6-8 to bucket 2
    buckets.moveToBucket(ElementIndex{6}, 2);
    buckets.moveToBucket(ElementIndex{7}, 2);
    buckets.moveToBucket(ElementIndex{8}, 2);

    // Elements 9-11 remain in bucket 3

    ASSERT_EQ(3u, buckets.bucketSize(0));
    ASSERT_EQ(3u, buckets.bucketSize(1));
    ASSERT_EQ(3u, buckets.bucketSize(2));
    ASSERT_EQ(3u, buckets.bucketSize(3));

    // Verify each element is in correct bucket
    for (uint8_t i = 0; i < 12; ++i) {
        ASSERT_EQ(i / 3, buckets.getBucket(ElementIndex{i}));
    }

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

// ==================== Stress Tests ====================

TEST(BucketedSetStressRandomMoves) {
    BucketedSetFixture<20, 5> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<20, 5>::ElementIndex;

    uint32_t seed = 12345;
    auto nextRand = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return seed;
    };

    // Perform many random moves
    for (int i = 0; i < 500; ++i) {
        uint8_t elem = nextRand() % 20;
        size_t targetBucket = nextRand() % 5;

        buckets.moveToBucket(ElementIndex{elem}, targetBucket);

        // Verify element is in target bucket
        ASSERT_EQ(targetBucket, buckets.getBucket(ElementIndex{elem}));

        // Periodically validate
        if (i % 50 == 0) {
            ASSERT_TRUE(fixture.validateBucketSizes());
            ASSERT_TRUE(fixture.perm.validate());
        }
    }

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetStressMixedOperations) {
    BucketedSetFixture<16, 4> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<16, 4>::ElementIndex;

    uint32_t seed = 67890;
    auto nextRand = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return seed;
    };

    // First distribute elements across buckets
    for (uint8_t i = 0; i < 16; ++i) {
        buckets.moveToBucket(ElementIndex{i}, i % 4);
    }

    // Mix of transfers and moves
    for (int i = 0; i < 300; ++i) {
        int op = nextRand() % 4;

        switch (op) {
            case 0: {  // moveToBucket
                uint8_t elem = nextRand() % 16;
                size_t target = nextRand() % 4;
                buckets.moveToBucket(ElementIndex{elem}, target);
                break;
            }
            case 1: {  // transferToNextBucket
                size_t src = nextRand() % 3;  // Can't transfer from last bucket
                if (!buckets.bucketEmpty(src)) {
                    buckets.transferToNextBucket(src);
                }
                break;
            }
            case 2: {  // transferToPrevBucket
                size_t src = 1 + (nextRand() % 3);  // Can't transfer from bucket 0
                if (!buckets.bucketEmpty(src)) {
                    buckets.transferToPrevBucket(src);
                }
                break;
            }
            case 3: {  // Query operations (no mutation)
                uint8_t elem = nextRand() % 16;
                size_t b = buckets.getBucket(ElementIndex{elem});
                ASSERT_TRUE(b < 4);
                break;
            }
        }

        // Validate after every operation
        ASSERT_TRUE(fixture.validateBucketSizes());
    }

    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetStressEmptyBucketMoves) {
    BucketedSetFixture<10, 6> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<10, 6>::ElementIndex;

    uint32_t seed = 11111;
    auto nextRand = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return seed;
    };

    // Concentrate all elements in one bucket, then scatter
    for (int round = 0; round < 10; ++round) {
        size_t targetBucket = nextRand() % 6;

        // Move all elements to target bucket
        for (uint8_t i = 0; i < 10; ++i) {
            buckets.moveToBucket(ElementIndex{i}, targetBucket);
        }

        ASSERT_EQ(10u, buckets.bucketSize(targetBucket));

        // Scatter to random buckets (creates empty intermediate buckets)
        for (uint8_t i = 0; i < 10; ++i) {
            size_t dest = nextRand() % 6;
            buckets.moveToBucket(ElementIndex{i}, dest);
        }

        ASSERT_TRUE(fixture.validateBucketSizes());
        ASSERT_TRUE(fixture.perm.validate());
    }
}

TEST(BucketedSetStressBackAndForth) {
    BucketedSetFixture<8, 4> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 4>::ElementIndex;

    // Move element 0 back and forth many times
    for (int i = 0; i < 100; ++i) {
        size_t target = i % 4;
        buckets.moveToBucket(ElementIndex{0}, target);
        ASSERT_EQ(target, buckets.getBucket(ElementIndex{0}));
    }

    ASSERT_TRUE(fixture.validateBucketSizes());
    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetVerifyElementPositionsAfterMoves) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 3>::ElementIndex;

    // Move elements to create known structure
    buckets.moveToBucket(ElementIndex{0}, 0);
    buckets.moveToBucket(ElementIndex{1}, 0);
    buckets.moveToBucket(ElementIndex{2}, 1);
    buckets.moveToBucket(ElementIndex{3}, 1);
    buckets.moveToBucket(ElementIndex{4}, 1);
    // Elements 5, 6, 7 remain in bucket 2

    // Verify we can iterate through buckets and find all elements
    size_t found = 0;
    for (size_t b = 0; b < 3; ++b) {
        for (auto pos = buckets.bucketStart(b); pos.index < buckets.bucketEnd(b).index; ++pos.index) {
            ElementIndex elem{fixture.window.elementAt(pos)};
            ASSERT_EQ(b, buckets.getBucket(elem));
            ++found;
        }
    }
    ASSERT_EQ(8u, found);

    ASSERT_TRUE(fixture.perm.validate());
}

TEST(BucketedSetTransferEmptyBucketAsserts) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;

    // Buckets 0 and 1 are empty after reset
    ASSERT_TRUE(buckets.bucketEmpty(0));
    ASSERT_TRUE(buckets.bucketEmpty(1));

    // Transferring from empty bucket should assert
    bool assertFired = false;
    try {
        buckets.transferToNextBucket(0);
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);

    assertFired = false;
    try {
        buckets.transferToPrevBucket(1);
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
}

TEST(BucketedSetTransferBoundaryBucketsAssert) {
    BucketedSetFixture<8, 3> fixture;
    auto& buckets = fixture.buckets;
    using ElementIndex = BucketedSetFixture<8, 3>::ElementIndex;

    // Move an element to bucket 0
    buckets.moveToBucket(ElementIndex{0}, 0);

    // Cannot transfer to prev from bucket 0
    bool assertFired = false;
    try {
        buckets.transferToPrevBucket(0);
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);

    // Cannot transfer to next from last bucket
    assertFired = false;
    try {
        buckets.transferToNextBucket(2);
    } catch (...) {
        assertFired = true;
    }
    ASSERT_TRUE(assertFired);
}
