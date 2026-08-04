// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/math_utils.h"
#include <gtest/gtest.h>

namespace zetasketch::hll {
namespace {

TEST(MathUtilsTest, Alpha) { EXPECT_NEAR(Alpha(14), 0.7213, 1e-4); }

TEST(MathUtilsTest, EstimateBiasWhenExactlyDefined) {
  EXPECT_NEAR(EstimateBias(738.1256, 10), 737.1256, 1e-4);
  EXPECT_NEAR(EstimateBias(14573.7784, 14), 9248.7784, 1e-4);
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
  EXPECT_NEAR(EstimateBias(1000.0, 9), 0.0, 1e-4);
  EXPECT_NEAR(EstimateBias(1000.0, 25), 0.0, 1e-4);
}

TEST(MathUtilsTest, LinearCountingThresholdWhenPreciselyDefined) {
  EXPECT_EQ(LinearCountingThreshold(14), 11500);
}

}  // namespace
}  // namespace zetasketch::hll
