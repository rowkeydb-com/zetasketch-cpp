// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/sparse_buffer.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

namespace {

using zetasketch::hll::SparseBuffer;

std::vector<uint32_t> Collected(const SparseBuffer& buffer) {
  std::vector<uint32_t> values;
  buffer.ForEach([&values](uint32_t value) {
    values.push_back(value);
    return true;
  });
  return values;
}

std::vector<uint32_t> AsVector(std::span<const uint32_t> values) {
  return {values.begin(), values.end()};
}

TEST(SparseBufferTest, HoldsNothingUntilAValueIsInserted) {
  SparseBuffer buffer;
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.size(), 0U);
  EXPECT_TRUE(Collected(buffer).empty());
  EXPECT_TRUE(buffer.Sorted().empty());
}

TEST(SparseBufferTest, ARepeatedValueIsHeldOnceAndCountedOnce) {
  SparseBuffer buffer;
  EXPECT_TRUE(buffer.Insert(7));
  EXPECT_FALSE(buffer.Insert(7));
  EXPECT_TRUE(buffer.Insert(3));
  EXPECT_FALSE(buffer.Insert(3));
  EXPECT_FALSE(buffer.Insert(7));
  EXPECT_EQ(buffer.size(), 2U);
  EXPECT_FALSE(buffer.empty());
  EXPECT_EQ(AsVector(buffer.Sorted()), (std::vector<uint32_t>{3, 7}));
}

// Zero is a value like any other, though it is the value of a cleared
// register elsewhere.
TEST(SparseBufferTest, ZeroIsAValue) {
  SparseBuffer buffer;
  EXPECT_TRUE(buffer.Insert(0));
  EXPECT_FALSE(buffer.Insert(0));
  EXPECT_EQ(buffer.size(), 1U);
  EXPECT_EQ(Collected(buffer), (std::vector<uint32_t>{0}));
  EXPECT_EQ(AsVector(buffer.Sorted()), (std::vector<uint32_t>{0}));
}

// The largest value marks an empty slot inside the table, so it is
// held apart from the table; it must still be held, counted, visited
// and sorted last like any other value.
TEST(SparseBufferTest, TheLargestValueIsHeldCountedAndSortedLast) {
  constexpr uint32_t kLargest = std::numeric_limits<uint32_t>::max();
  SparseBuffer alone;
  EXPECT_TRUE(alone.Insert(kLargest));
  EXPECT_FALSE(alone.Insert(kLargest));
  EXPECT_EQ(alone.size(), 1U);
  EXPECT_FALSE(alone.empty());
  EXPECT_EQ(Collected(alone), (std::vector<uint32_t>{kLargest}));
  EXPECT_EQ(AsVector(alone.Sorted()), (std::vector<uint32_t>{kLargest}));

  SparseBuffer among;
  EXPECT_TRUE(among.Insert(5));
  EXPECT_TRUE(among.Insert(kLargest));
  EXPECT_TRUE(among.Insert(1));
  EXPECT_EQ(among.size(), 3U);
  EXPECT_EQ(AsVector(among.Sorted()), (std::vector<uint32_t>{1, 5, kLargest}));
  // Sorting twice arranges nothing anew.
  EXPECT_EQ(AsVector(among.Sorted()), (std::vector<uint32_t>{1, 5, kLargest}));
  EXPECT_EQ(Collected(among), (std::vector<uint32_t>{1, 5, kLargest}));

  among.clear();
  EXPECT_TRUE(among.empty());
  EXPECT_TRUE(among.Sorted().empty());
  EXPECT_TRUE(Collected(among).empty());
}

// The table grows as values arrive, and no value is lost or repeated
// across its growth. Consecutive values, values a fixed stride apart
// and values that differ only in their upper bits are all placed
// through the same mixing.
TEST(SparseBufferTest, GrowsWithoutLosingOrRepeatingAValue) {
  constexpr size_t kCount = 5000;
  SparseBuffer buffer;
  std::vector<uint32_t> expected;
  for (uint32_t i = 0; i < kCount; ++i) {
    const uint32_t consecutive = i;
    const uint32_t strided = (i << 6U) | 0x2000U;
    const uint32_t high = i << 20U;
    for (const uint32_t value : {consecutive, strided, high}) {
      if (buffer.Insert(value)) expected.push_back(value);
      EXPECT_FALSE(buffer.Insert(value));
    }
  }
  EXPECT_EQ(buffer.size(), expected.size());
  std::vector<uint32_t> visited = Collected(buffer);
  std::ranges::sort(visited);
  std::ranges::sort(expected);
  EXPECT_EQ(visited, expected);
  EXPECT_EQ(AsVector(buffer.Sorted()), expected);
}

// Once sorted the table is an array, and an insertion rebuilds the set
// from it; nothing sorted is lost and the repeat check still holds.
TEST(SparseBufferTest, InsertingAfterSortingRebuildsTheSet) {
  SparseBuffer buffer;
  for (const uint32_t value : {40U, 10U, 30U, 20U}) {
    EXPECT_TRUE(buffer.Insert(value));
  }
  EXPECT_EQ(AsVector(buffer.Sorted()), (std::vector<uint32_t>{10, 20, 30, 40}));
  EXPECT_FALSE(buffer.Insert(30));
  EXPECT_TRUE(buffer.Insert(25));
  EXPECT_EQ(buffer.size(), 5U);
  EXPECT_EQ(AsVector(buffer.Sorted()),
            (std::vector<uint32_t>{10, 20, 25, 30, 40}));

  // The same with the largest value held, which sits past the sorted
  // values rather than among them.
  constexpr uint32_t kLargest = std::numeric_limits<uint32_t>::max();
  EXPECT_TRUE(buffer.Insert(kLargest));
  EXPECT_EQ(AsVector(buffer.Sorted()),
            (std::vector<uint32_t>{10, 20, 25, 30, 40, kLargest}));
  EXPECT_FALSE(buffer.Insert(kLargest));
  EXPECT_FALSE(buffer.Insert(25));
  EXPECT_TRUE(buffer.Insert(35));
  EXPECT_EQ(buffer.size(), 7U);
  EXPECT_EQ(AsVector(buffer.Sorted()),
            (std::vector<uint32_t>{10, 20, 25, 30, 35, 40, kLargest}));
}

TEST(SparseBufferTest, ClearingForgetsEveryValueAndKeepsWorking) {
  constexpr uint32_t kHeldBeforeClearing = 100;
  SparseBuffer buffer;
  for (uint32_t value = 0; value < kHeldBeforeClearing; ++value) {
    EXPECT_TRUE(buffer.Insert(value));
  }
  buffer.clear();
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.size(), 0U);
  EXPECT_TRUE(Collected(buffer).empty());
  EXPECT_TRUE(buffer.Insert(50));
  EXPECT_TRUE(buffer.Insert(51));
  EXPECT_EQ(AsVector(buffer.Sorted()), (std::vector<uint32_t>{50, 51}));
  buffer.clear();
  EXPECT_TRUE(buffer.Sorted().empty());
  EXPECT_TRUE(buffer.Insert(50));
  EXPECT_EQ(buffer.size(), 1U);
}

// A representation that a refused merge was moved out of is still
// written and estimated afterwards, and its buffer with it. A buffer
// moved from must therefore be empty in count as well as in storage,
// and take values again; one moved to must hold what the other held.
TEST(SparseBufferTest, ABufferMovedFromIsEmptyAndUsable) {
  constexpr uint32_t kLargest = std::numeric_limits<uint32_t>::max();
  // The buffers live inside an object, as a representation's does, and
  // the one moved from is read on purpose afterwards.
  std::array<SparseBuffer, 3> buffers;
  SparseBuffer& source = buffers[0];
  SparseBuffer& assigned = buffers[1];
  SparseBuffer& constructed = buffers[2];
  EXPECT_TRUE(source.Insert(9));
  EXPECT_TRUE(source.Insert(kLargest));
  EXPECT_TRUE(source.Insert(4));

  constructed = SparseBuffer(std::move(buffers[0]));
  EXPECT_EQ(AsVector(constructed.Sorted()),
            (std::vector<uint32_t>{4, 9, kLargest}));
  EXPECT_TRUE(source.empty());
  EXPECT_EQ(source.size(), 0U);
  EXPECT_TRUE(source.Sorted().empty());
  EXPECT_TRUE(Collected(source).empty());
  EXPECT_TRUE(source.Insert(kLargest));
  EXPECT_TRUE(source.Insert(1));
  EXPECT_EQ(AsVector(source.Sorted()), (std::vector<uint32_t>{1, kLargest}));

  EXPECT_TRUE(assigned.Insert(2));
  assigned = std::move(buffers[2]);
  EXPECT_EQ(AsVector(assigned.Sorted()),
            (std::vector<uint32_t>{4, 9, kLargest}));
  EXPECT_TRUE(constructed.empty());
  EXPECT_TRUE(constructed.Sorted().empty());
  EXPECT_TRUE(constructed.Insert(4));
  EXPECT_EQ(constructed.size(), 1U);
}

// A visitor that returns false stops the visit; one that returns true
// sees every value once, whether or not the table has been sorted.
TEST(SparseBufferTest, TheVisitorSeesEachValueOnceAndCanStop) {
  constexpr uint32_t kHeld = 20;
  constexpr size_t kVisitedBeforeStopping = 5;
  SparseBuffer buffer;
  for (uint32_t value = 1; value <= kHeld; ++value) {
    EXPECT_TRUE(buffer.Insert(value * 3));
  }
  std::vector<uint32_t> unsorted = Collected(buffer);
  EXPECT_EQ(unsorted.size(), kHeld);
  size_t seen_in_the_table = 0;
  buffer.ForEach([&seen_in_the_table](uint32_t) {
    ++seen_in_the_table;
    return seen_in_the_table < kVisitedBeforeStopping;
  });
  EXPECT_EQ(seen_in_the_table, kVisitedBeforeStopping);

  std::ranges::sort(unsorted);
  EXPECT_EQ(unsorted, AsVector(buffer.Sorted()));
  EXPECT_EQ(Collected(buffer), AsVector(buffer.Sorted()));

  size_t seen = 0;
  buffer.ForEach([&seen](uint32_t) {
    ++seen;
    return seen < kVisitedBeforeStopping;
  });
  EXPECT_EQ(seen, kVisitedBeforeStopping);
}

}  // namespace
