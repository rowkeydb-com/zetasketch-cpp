// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hyperloglogplusplus.h"
#include <cstdint>
#include <gtest/gtest.h>

namespace zetasketch {
namespace {

constexpr int32_t kTestNormalPrecision = 15;
constexpr int32_t kTestSparsePrecision = 20;
constexpr int64_t kTestValue1 = 10;
constexpr int64_t kTestValue2 = 20;
constexpr int64_t kTestValue3 = 30;

TEST(HyperLogLogPlusPlusTest, RoundTripSerializationDense) {
  auto sketch_res = HyperLogLogPlusPlus::Create(
      kTestNormalPrecision, HyperLogLogPlusPlus::kSparsePrecisionDisabled);
  ASSERT_TRUE(sketch_res.has_value());
  auto& sketch = sketch_res.value();

  sketch.Add(kTestValue1);
  sketch.Add(kTestValue2);
  sketch.Add(kTestValue3);

  auto serialized_res = sketch.Serialize();
  ASSERT_TRUE(serialized_res.has_value());

  auto deserialized_res =
      HyperLogLogPlusPlus::FromBytes(serialized_res.value());
  ASSERT_TRUE(deserialized_res.has_value());
  auto& deserialized = deserialized_res.value();

  EXPECT_EQ(sketch.Result(), deserialized.Result());
}

TEST(HyperLogLogPlusPlusTest, RoundTripSerializationSparse) {
  auto sketch_res =
      HyperLogLogPlusPlus::Create(kTestNormalPrecision, kTestSparsePrecision);
  ASSERT_TRUE(sketch_res.has_value());
  auto& sketch = sketch_res.value();

  sketch.Add(kTestValue1);
  sketch.Add(kTestValue2);

  auto serialized_res = sketch.Serialize();
  ASSERT_TRUE(serialized_res.has_value());

  auto deserialized_res =
      HyperLogLogPlusPlus::FromBytes(serialized_res.value());
  ASSERT_TRUE(deserialized_res.has_value());
  auto& deserialized = deserialized_res.value();

  EXPECT_EQ(sketch.Result(), deserialized.Result());
}

}  // namespace
}  // namespace zetasketch
