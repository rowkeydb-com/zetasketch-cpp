// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/math_utils.h"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <vector>
#include <gtest/gtest.h>
#include "zetasketch/bias_data.h"

namespace zetasketch::hll {
namespace {

TEST(MathUtilsTest, Alpha) { EXPECT_NEAR(Alpha(14), 0.7213, 1e-4); }

TEST(MathUtilsTest, EstimateBiasWhenExactlyDefined) {
  EXPECT_NEAR(EstimateBias(738.1256, 10), 737.1256, 1e-4);
  EXPECT_NEAR(EstimateBias(14573.7784, 14), 9248.7784, 1e-4);
}

// The reference's own cases, ported from its DataTest. The first two
// are at precisions this change adds, and the second drives the
// interpolation rather than an exact match.
TEST(MathUtilsTest, EstimateBiasMatchesTheReferenceCases) {
  EXPECT_NEAR(EstimateBias(193.8044, 6), 1.8044, 1e-4);
  EXPECT_NEAR(EstimateBias(25, 5), 19.5258, 1e-4);
  EXPECT_NEAR(EstimateBias(78, 4), 0.0, 1e-4);
}

TEST(MathUtilsTest, EstimateBiasWhenInterpolationNeeded) {
  EXPECT_NEAR(EstimateBias(1490.0, 11), 1456.8144, 1e-4);
  EXPECT_NEAR(EstimateBias(16300.0, 14), 8005.2257, 1e-4);
  EXPECT_NEAR(EstimateBias(653000.0, 17), -411.7805, 1e-4);
}

TEST(MathUtilsTest, EstimateBiasReturnsZeroWhenMeanOutOfRange) {
  EXPECT_NEAR(EstimateBias(738.0, 10), 0.0, 1e-4);
  EXPECT_NEAR(EstimateBias(1310000.0, 18), 0.0, 1e-4);
}

TEST(MathUtilsTest, EstimateBiasReturnsZeroWhenPrecisionOutOfRange) {
  EXPECT_NEAR(EstimateBias(1000.0, 3), 0.0, 1e-4);
  EXPECT_NEAR(EstimateBias(1000.0, 25), 0.0, 1e-4);
}

TEST(MathUtilsTest, LinearCountingThresholdWhenPreciselyDefined) {
  EXPECT_EQ(LinearCountingThreshold(14), 11500);
}

// The reference implementation replaces the alpha formula with a
// constant for the three lowest precisions. Without these the estimate
// at those precisions is wrong.
TEST(MathUtilsTest, AlphaUsesTheReferenceConstantsAtLowPrecisions) {
  EXPECT_DOUBLE_EQ(Alpha(4), 0.673101517);
  EXPECT_DOUBLE_EQ(Alpha(5), 0.697121585);
  EXPECT_DOUBLE_EQ(Alpha(6), 0.709209798);
  // Precision 7 and above use the formula.
  EXPECT_DOUBLE_EQ(Alpha(7), 0.7213 / (1.0 + 1.079 / 128.0));
  EXPECT_DOUBLE_EQ(Alpha(24), 0.7213 / (1.0 + 1.079 / 16777216.0));
}

// Every precision the tables cover has its own empirical threshold;
// precisions outside the tables fall back to five halves of the bucket
// count, as the original HyperLogLog paper prescribes.
TEST(MathUtilsTest, LinearCountingThresholdCoversEveryTabulatedPrecision) {
  const std::array<int32_t, 15> expected = {
      10,   20,   40,    80,    220,   400,    900,   1800,
      3100, 6500, 11500, 20000, 50000, 120000, 350000};
  for (int32_t precision = internal::kMinimumPrecision;
       precision <= internal::kMaximumPrecision; ++precision) {
    const auto index =
        static_cast<size_t>(precision - internal::kMinimumPrecision);
    EXPECT_EQ(LinearCountingThreshold(precision), expected.at(index))
        << "precision " << precision;
  }
  // Outside the tabulated range the threshold is five halves of the
  // bucket count: eight buckets at precision 3, and 524288 at
  // precision 19.
  EXPECT_EQ(LinearCountingThreshold(3), 20);
  EXPECT_EQ(LinearCountingThreshold(19), 1310720);
}

// The bias tables are indexed by precision less the minimum, so a
// change to the minimum shifts every row. Reading a table's own mean
// must return that same table's bias, at every precision and at both
// ends and the middle of each row: any shift moves all of them.
TEST(MathUtilsTest, BiasTablesAreIndexedByPrecisionThroughout) {
  for (int32_t precision = internal::kMinimumPrecision;
       precision <= internal::kMaximumPrecision; ++precision) {
    const auto row =
        static_cast<size_t>(precision - internal::kMinimumPrecision);
    const std::span<const double> means = internal::kMeanData.at(row);
    const std::span<const double> biases = internal::kBiasData.at(row);
    ASSERT_EQ(means.size(), biases.size()) << "precision " << precision;
    for (const size_t position :
         {size_t{0}, means.size() / 2, means.size() - 1}) {
      const auto offset = static_cast<std::ptrdiff_t>(position);
      EXPECT_DOUBLE_EQ(
          EstimateBias(*std::next(means.begin(), offset), precision),
          *std::next(biases.begin(), offset))
          << "precision " << precision << " position " << position;
    }
  }
}

// The reference's mean tables are not all sorted: the rows for
// precisions 5 and 6 each contain descending pairs. Both the
// reference's search and ours are binary searches, which have no
// defined result on unsorted input, so ours must be the reference's
// own algorithm rather than a lower bound. This records the
// condition; if the reference's data is ever corrected, the search
// may be reconsidered, and this test is where that is noticed.
TEST(MathUtilsTest, ReferenceMeanTablesContainDescendingPairs) {
  // One entry per tabulated precision, taken from the tables
  // themselves so that the two cannot fall out of step.
  std::array<int, internal::kMeanData.size()> descending{};
  for (int32_t precision = internal::kMinimumPrecision;
       precision <= internal::kMaximumPrecision; ++precision) {
    const auto row =
        static_cast<size_t>(precision - internal::kMinimumPrecision);
    const std::span<const double> means = internal::kMeanData.at(row);
    int count = 0;
    for (size_t i = 0; i + 1 < means.size(); ++i) {
      const auto offset = static_cast<std::ptrdiff_t>(i);
      if (*std::next(means.begin(), offset) >
          *std::next(means.begin(), offset + 1)) {
        ++count;
      }
    }
    descending.at(row) = count;
  }
  EXPECT_EQ(descending.at(1), 2) << "precision 5";
  EXPECT_EQ(descending.at(2), 2) << "precision 6";
  for (size_t row = 0; row < descending.size(); ++row) {
    if (row != 1 && row != 2) {
      EXPECT_EQ(descending.at(row), 0)
          << "precision " << (row + internal::kMinimumPrecision);
    }
  }
}

// Two means in the reference's precision-5 row are exactly equidistant
// from this estimate, so which of them is accumulated first decides the
// last bit of the correction. The reference keeps them in the order its
// window holds them, its sort being stable, and the value below is the
// reference's own. Of the 5,673 estimates formed from every tabulated
// mean and every midpoint between adjacent means, across all fifteen
// precisions, this is the only one whose correction a reversed tie
// order would change, and it changes by one unit in the last place.
// This assertion is therefore what defends the stability of the
// selection on any host; the comparison against the reference defends
// it only where the reference can be run.
TEST(MathUtilsTest, TiedNeighboursAreAccumulatedInWindowOrder) {
  EXPECT_EQ(std::bit_cast<uint64_t>(EstimateBias(129.19300000000001, 5)),
            std::bit_cast<uint64_t>(-0.15010441178807749));
}

// The reference divides each bias by its squared distance. Multiplying
// by the reciprocal instead would be one operation cheaper and would
// differ in the last bit, the reciprocal being rounded before the
// product is. Of the same 5,673 estimates, 515 have a correction that
// the reciprocal form would change; three of them are pinned here, at
// the lowest precision, in the middle of the range and at the highest.
// Like the assertion above, this defends the division on any host,
// where the comparison against the reference defends it only where the
// reference can be run.
TEST(MathUtilsTest, BiasesAreDividedByTheDistanceNotScaledByItsReciprocal) {
  EXPECT_EQ(std::bit_cast<uint64_t>(EstimateBias(17.1179, 4)),
            std::bit_cast<uint64_t>(5.611762757533696));
  EXPECT_EQ(std::bit_cast<uint64_t>(EstimateBias(821.2267, 10)),
            std::bit_cast<uint64_t>(654.2737871826133));
  EXPECT_EQ(std::bit_cast<uint64_t>(EstimateBias(197076.86, 18)),
            std::bit_cast<uint64_t>(180715.22831970637));
}

// The reference rounds with a routine of its language's, not with a
// conversion of a rounded double: it rounds a half towards positive
// infinity rather than away from zero, reports zero for a value that is
// not a number, and saturates at the extremes of the result type, where
// the conversion would be undefined. Estimates reach all three cases
// from sketches the reference accepts, because it validates a register
// array's contents and the sparse size not at all. Every expectation
// below is that routine's own output for the same value.
TEST(MathUtilsTest, RoundingReproducesTheReferenceRoutine) {
  struct Case {
    double value;
    int64_t rounded;
  };
  constexpr int64_t kLargest = std::numeric_limits<int64_t>::max();
  constexpr int64_t kSmallest = std::numeric_limits<int64_t>::min();
  const std::vector<Case> cases = {
      {.value = 0.0, .rounded = 0},
      {.value = -0.0, .rounded = 0},
      {.value = 0.5, .rounded = 1},
      {.value = -0.5, .rounded = 0},
      {.value = 1.5, .rounded = 2},
      {.value = -1.5, .rounded = -1},
      {.value = 2.5, .rounded = 3},
      {.value = -2.5, .rounded = -2},
      // The value below a half that a naive addition of a half would
      // carry up to one.
      {.value = 0.49999999999999994, .rounded = 0},
      {.value = -0.49999999999999994, .rounded = 0},
      // Either side of the magnitude above which every value is
      // already integral and the decomposition is not taken.
      {.value = 4503599627370495.5, .rounded = 4503599627370496},
      {.value = -4503599627370495.5, .rounded = -4503599627370495},
      {.value = 0x1.0p52, .rounded = 4503599627370496},
      {.value = 0x1.0p63, .rounded = kLargest},
      {.value = -0x1.0p63, .rounded = kSmallest},
      {.value = 1e300, .rounded = kLargest},
      {.value = -1e300, .rounded = kSmallest},
      {.value = std::numeric_limits<double>::infinity(), .rounded = kLargest},
      {.value = -std::numeric_limits<double>::infinity(), .rounded = kSmallest},
      {.value = std::numeric_limits<double>::quiet_NaN(), .rounded = 0},
      // The raw estimate a register array of 0xFF bytes produces, whose
      // sum is negative because the reference's shift yields the most
      // negative long.
      {.value = -9.95e19, .rounded = kSmallest},
      {.value = 1.0000000000000002, .rounded = 1},
      {.value = 123456.499999, .rounded = 123456},
      {.value = 123456.5, .rounded = 123457},
      {.value = -123456.5, .rounded = -123456},
  };

  for (const auto& test_case : cases) {
    EXPECT_EQ(RoundAsTheReferenceDoes(test_case.value), test_case.rounded)
        << "value " << test_case.value;
  }
}

// The reference locates a position with a binary search. Where a table
// is sorted a lower bound agrees with it; where one is not, it need
// not. Below are four estimates at which they disagree: the first
// three are tabulated means, the fourth the midpoint of two adjacent
// means. A lower bound reports 130, 148, 169 and 169 against the
// positions asserted here.
TEST(MathUtilsTest, InsertionPointIsTheReferenceSearchNotALowerBound) {
  // Rows are indexed by precision less the minimum. Precisions 5 and 6
  // are the two whose rows are not sorted; precision 10 is an ordinary
  // sorted one.
  constexpr int32_t kFirstUnsortedPrecision = 5;
  constexpr int32_t kSecondUnsortedPrecision = 6;
  constexpr int32_t kSortedPrecision = 10;
  const auto row_of = [](int32_t precision) {
    return static_cast<size_t>(precision - internal::kMinimumPrecision);
  };

  const std::span<const double> fifth =
      internal::kMeanData.at(row_of(kFirstUnsortedPrecision));
  const std::span<const double> sixth =
      internal::kMeanData.at(row_of(kSecondUnsortedPrecision));
  EXPECT_EQ(ReferenceInsertionPoint(fifth, 131.0042), size_t{131});
  EXPECT_EQ(ReferenceInsertionPoint(sixth, 237.7474), size_t{149});
  EXPECT_EQ(ReferenceInsertionPoint(sixth, 267.2566), size_t{167});
  EXPECT_EQ(ReferenceInsertionPoint(sixth, 267.2095), size_t{167});

  // Where a row is sorted the position is the ordinary one, at both
  // ends and outside the range.
  const std::span<const double> tenth =
      internal::kMeanData.at(row_of(kSortedPrecision));
  EXPECT_EQ(ReferenceInsertionPoint(tenth, tenth.front()), size_t{0});
  EXPECT_EQ(ReferenceInsertionPoint(tenth, tenth.front() - 1.0), size_t{0});
  EXPECT_EQ(ReferenceInsertionPoint(tenth, tenth.back() + 1.0), tenth.size());

  // Where neither value is less nor greater the reference compares bit
  // patterns, which places negative zero below zero. No table reaches
  // this, every mean being far above zero, but the contract above says
  // the position is the reference's and so it must be.
  const std::array<double, 1> only_negative_zero = {-0.0};
  EXPECT_EQ(ReferenceInsertionPoint(only_negative_zero, 0.0), size_t{1});
  const std::array<double, 3> around_zero = {-1.0, -0.0, 1.0};
  EXPECT_EQ(ReferenceInsertionPoint(around_zero, 0.0), size_t{2});
}

}  // namespace
}  // namespace zetasketch::hll
