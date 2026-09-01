// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hyperloglogplusplus.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "zetasketch/hll/state.h"

namespace zetasketch {
namespace {

constexpr int32_t kTestNormalPrecision = 15;
constexpr int32_t kTestSparsePrecision = 20;
constexpr int64_t kTestValue1 = 10;
constexpr int64_t kTestValue2 = 20;
constexpr int64_t kTestValue3 = 30;
constexpr int32_t kLowNormalPrecision = 10;
constexpr int32_t kLowSparsePrecision = 15;
constexpr int64_t kPromotionAddCount = 5000;
constexpr int kFirstBatchCount = 10;
constexpr int kTotalCount = 20;
// One more addition than the buffer holds at precision 10: the 257th
// addition triggers exactly one flush, which consumes the whole
// buffer and leaves it empty.
constexpr int kFlushedAddCount = 257;

TEST(HyperLogLogPlusPlusTest, RoundTripSerializationDense) {
  auto sketch_res = HyperLogLogPlusPlus::Create(
      kTestNormalPrecision, HyperLogLogPlusPlus::kSparsePrecisionDisabled);
  ASSERT_TRUE(sketch_res.has_value());
  auto& sketch = sketch_res.value();

  ASSERT_TRUE(sketch.Add(kTestValue1).has_value());
  ASSERT_TRUE(sketch.Add(kTestValue2).has_value());
  ASSERT_TRUE(sketch.Add(kTestValue3).has_value());

  auto serialized_res = sketch.Serialize();
  ASSERT_TRUE(serialized_res.has_value());

  auto deserialized_res =
      HyperLogLogPlusPlus::FromBytes(serialized_res.value());
  ASSERT_TRUE(deserialized_res.has_value());
  auto& deserialized = deserialized_res.value();

  auto original_estimate = sketch.Result();
  auto restored_estimate = deserialized.Result();
  ASSERT_TRUE(original_estimate.has_value());
  ASSERT_TRUE(restored_estimate.has_value());
  EXPECT_EQ(original_estimate.value(), restored_estimate.value());
}

TEST(HyperLogLogPlusPlusTest, RoundTripSerializationSparse) {
  auto sketch_res =
      HyperLogLogPlusPlus::Create(kTestNormalPrecision, kTestSparsePrecision);
  ASSERT_TRUE(sketch_res.has_value());
  auto& sketch = sketch_res.value();

  ASSERT_TRUE(sketch.Add(kTestValue1).has_value());
  ASSERT_TRUE(sketch.Add(kTestValue2).has_value());

  auto serialized_res = sketch.Serialize();
  ASSERT_TRUE(serialized_res.has_value()) << serialized_res.error().message;

  auto deserialized_res =
      HyperLogLogPlusPlus::FromBytes(serialized_res.value());
  ASSERT_TRUE(deserialized_res.has_value());
  auto& deserialized = deserialized_res.value();

  auto original_estimate = sketch.Result();
  auto restored_estimate = deserialized.Result();
  ASSERT_TRUE(original_estimate.has_value());
  ASSERT_TRUE(restored_estimate.has_value());
  EXPECT_EQ(original_estimate.value(), restored_estimate.value());
}

TEST(HyperLogLogPlusPlusTest, ResultOnFreshSketchIsZero) {
  auto sparse =
      HyperLogLogPlusPlus::Create(kTestNormalPrecision, kTestSparsePrecision);
  ASSERT_TRUE(sparse.has_value());
  auto sparse_estimate = sparse.value().Result();
  ASSERT_TRUE(sparse_estimate.has_value());
  EXPECT_EQ(sparse_estimate.value(), 0);

  auto dense = HyperLogLogPlusPlus::Create(
      kTestNormalPrecision, HyperLogLogPlusPlus::kSparsePrecisionDisabled);
  ASSERT_TRUE(dense.has_value());
  auto dense_estimate = dense.value().Result();
  ASSERT_TRUE(dense_estimate.has_value());
  EXPECT_EQ(dense_estimate.value(), 0);
}

// Enough integer additions promote the sparse representation to the
// normal one, and the estimate survives a serialization round trip.
TEST(HyperLogLogPlusPlusTest, IntegerAddPromotesSparseToNormal) {
  auto sketch_res =
      HyperLogLogPlusPlus::Create(kLowNormalPrecision, kLowSparsePrecision);
  ASSERT_TRUE(sketch_res.has_value());
  auto sketch = std::move(sketch_res.value());
  for (int64_t i = 0; i < kPromotionAddCount; ++i) {
    ASSERT_TRUE(sketch.Add(i).has_value());
  }

  auto serialized = sketch.Serialize();
  ASSERT_TRUE(serialized.has_value());
  auto state = hll::State::Parse(serialized.value());
  ASSERT_TRUE(state.has_value());
  EXPECT_TRUE(state->data.has_value());
  EXPECT_FALSE(state->sparse_data.has_value());

  auto restored = HyperLogLogPlusPlus::FromBytes(serialized.value());
  ASSERT_TRUE(restored.has_value());
  auto original_estimate = sketch.Result();
  auto restored_estimate = restored.value().Result();
  ASSERT_TRUE(original_estimate.has_value());
  ASSERT_TRUE(restored_estimate.has_value());
  EXPECT_EQ(original_estimate.value(), restored_estimate.value());
}

// Additions made after deserialization must leave the sketch
// indistinguishable from one built in a single pass over the same
// values.
TEST(HyperLogLogPlusPlusTest, AddAfterDeserializationMatchesOnePass) {
  auto first_res =
      HyperLogLogPlusPlus::Create(kLowNormalPrecision, kLowSparsePrecision);
  ASSERT_TRUE(first_res.has_value());
  auto first = std::move(first_res.value());
  for (int i = 0; i < kFirstBatchCount; ++i) {
    ASSERT_TRUE(
        first.Add(std::string("item_") + std::to_string(i)).has_value());
  }
  auto intermediate = first.Serialize();
  ASSERT_TRUE(intermediate.has_value());

  auto resumed_res = HyperLogLogPlusPlus::FromBytes(intermediate.value());
  ASSERT_TRUE(resumed_res.has_value());
  auto resumed = std::move(resumed_res.value());
  for (int i = kFirstBatchCount; i < kTotalCount; ++i) {
    ASSERT_TRUE(
        resumed.Add(std::string("item_") + std::to_string(i)).has_value());
  }

  auto one_pass_res =
      HyperLogLogPlusPlus::Create(kLowNormalPrecision, kLowSparsePrecision);
  ASSERT_TRUE(one_pass_res.has_value());
  auto one_pass = std::move(one_pass_res.value());
  for (int i = 0; i < kTotalCount; ++i) {
    ASSERT_TRUE(
        one_pass.Add(std::string("item_") + std::to_string(i)).has_value());
  }

  auto resumed_bytes = resumed.Serialize();
  auto one_pass_bytes = one_pass.Serialize();
  ASSERT_TRUE(resumed_bytes.has_value());
  ASSERT_TRUE(one_pass_bytes.has_value());
  EXPECT_EQ(resumed_bytes.value(), one_pass_bytes.value());
}

// Additions to a deserialized dense sketch must match one-pass
// construction byte for byte: registers merge by maximum and the
// value count is additive.
TEST(HyperLogLogPlusPlusTest, AddAfterDenseDeserializationMatchesOnePass) {
  auto first_res = HyperLogLogPlusPlus::Create(
      kLowNormalPrecision, HyperLogLogPlusPlus::kSparsePrecisionDisabled);
  ASSERT_TRUE(first_res.has_value());
  auto first = std::move(first_res.value());
  for (int i = 0; i < kFirstBatchCount; ++i) {
    ASSERT_TRUE(
        first.Add(std::string("item_") + std::to_string(i)).has_value());
  }
  auto intermediate = first.Serialize();
  ASSERT_TRUE(intermediate.has_value());

  auto resumed_res = HyperLogLogPlusPlus::FromBytes(intermediate.value());
  ASSERT_TRUE(resumed_res.has_value());
  auto resumed = std::move(resumed_res.value());
  for (int i = kFirstBatchCount; i < kTotalCount; ++i) {
    ASSERT_TRUE(
        resumed.Add(std::string("item_") + std::to_string(i)).has_value());
  }

  auto one_pass_res = HyperLogLogPlusPlus::Create(
      kLowNormalPrecision, HyperLogLogPlusPlus::kSparsePrecisionDisabled);
  ASSERT_TRUE(one_pass_res.has_value());
  auto one_pass = std::move(one_pass_res.value());
  for (int i = 0; i < kTotalCount; ++i) {
    ASSERT_TRUE(
        one_pass.Add(std::string("item_") + std::to_string(i)).has_value());
  }

  auto resumed_bytes = resumed.Serialize();
  auto one_pass_bytes = one_pass.Serialize();
  ASSERT_TRUE(resumed_bytes.has_value());
  ASSERT_TRUE(one_pass_bytes.has_value());
  EXPECT_EQ(resumed_bytes.value(), one_pass_bytes.value());
}

// Integer additions resumed after deserialization must also match
// one-pass construction byte for byte.
TEST(HyperLogLogPlusPlusTest, IntegerAddAfterDeserializationMatchesOnePass) {
  auto first_res =
      HyperLogLogPlusPlus::Create(kLowNormalPrecision, kLowSparsePrecision);
  ASSERT_TRUE(first_res.has_value());
  auto first = std::move(first_res.value());
  for (int64_t i = 0; i < kFirstBatchCount; ++i) {
    ASSERT_TRUE(first.Add(i).has_value());
  }
  auto intermediate = first.Serialize();
  ASSERT_TRUE(intermediate.has_value());

  auto resumed_res = HyperLogLogPlusPlus::FromBytes(intermediate.value());
  ASSERT_TRUE(resumed_res.has_value());
  auto resumed = std::move(resumed_res.value());
  for (int64_t i = kFirstBatchCount; i < kTotalCount; ++i) {
    ASSERT_TRUE(resumed.Add(i).has_value());
  }

  auto one_pass_res =
      HyperLogLogPlusPlus::Create(kLowNormalPrecision, kLowSparsePrecision);
  ASSERT_TRUE(one_pass_res.has_value());
  auto one_pass = std::move(one_pass_res.value());
  for (int64_t i = 0; i < kTotalCount; ++i) {
    ASSERT_TRUE(one_pass.Add(i).has_value());
  }

  auto resumed_bytes = resumed.Serialize();
  auto one_pass_bytes = one_pass.Serialize();
  ASSERT_TRUE(resumed_bytes.has_value());
  ASSERT_TRUE(one_pass_bytes.has_value());
  EXPECT_EQ(resumed_bytes.value(), one_pass_bytes.value());
}

// A freshly created, never-added sketch serializes to the exact bytes
// the Java library produces for an empty sketch at the same
// precisions: the P10_SP15_POP0 vector of golden_corpus.tsv.
TEST(HyperLogLogPlusPlusTest, FreshSketchSerializesToTheJavaEmptyVector) {
  // NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
  const std::vector<uint8_t> java_empty = {0x08, 0x70, 0x10, 0x00, 0x18, 0x02,
                                           0x20, 0x0B, 0x82, 0x07, 0x06, 0x18,
                                           0x0A, 0x20, 0x0F, 0x32, 0x00};
  // NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
  auto sketch =
      HyperLogLogPlusPlus::Create(kLowNormalPrecision, kLowSparsePrecision);
  ASSERT_TRUE(sketch.has_value());
  auto ser = sketch.value().Serialize();
  ASSERT_TRUE(ser.has_value());
  EXPECT_EQ(ser.value(), java_empty);
}

// Integer additions resumed on a deserialized dense sketch must also
// match one-pass construction byte for byte.
TEST(HyperLogLogPlusPlusTest, IntegerAddAfterDenseDeserializationOnePass) {
  auto first_res = HyperLogLogPlusPlus::Create(
      kLowNormalPrecision, HyperLogLogPlusPlus::kSparsePrecisionDisabled);
  ASSERT_TRUE(first_res.has_value());
  auto first = std::move(first_res.value());
  for (int64_t i = 0; i < kFirstBatchCount; ++i) {
    ASSERT_TRUE(first.Add(i).has_value());
  }
  auto intermediate = first.Serialize();
  ASSERT_TRUE(intermediate.has_value());

  auto resumed_res = HyperLogLogPlusPlus::FromBytes(intermediate.value());
  ASSERT_TRUE(resumed_res.has_value());
  auto resumed = std::move(resumed_res.value());
  for (int64_t i = kFirstBatchCount; i < kTotalCount; ++i) {
    ASSERT_TRUE(resumed.Add(i).has_value());
  }

  auto one_pass_res = HyperLogLogPlusPlus::Create(
      kLowNormalPrecision, HyperLogLogPlusPlus::kSparsePrecisionDisabled);
  ASSERT_TRUE(one_pass_res.has_value());
  auto one_pass = std::move(one_pass_res.value());
  for (int64_t i = 0; i < kTotalCount; ++i) {
    ASSERT_TRUE(one_pass.Add(i).has_value());
  }

  auto resumed_bytes = resumed.Serialize();
  auto one_pass_bytes = one_pass.Serialize();
  ASSERT_TRUE(resumed_bytes.has_value());
  ASSERT_TRUE(one_pass_bytes.has_value());
  EXPECT_EQ(resumed_bytes.value(), one_pass_bytes.value());
}

// A sketch whose buffer has flushed in memory still estimates and
// serializes through the public interface, and remains sparse.
TEST(HyperLogLogPlusPlusTest, FlushedSketchEstimatesAndSerializes) {
  auto sketch_res =
      HyperLogLogPlusPlus::Create(kLowNormalPrecision, kLowSparsePrecision);
  ASSERT_TRUE(sketch_res.has_value());
  auto sketch = std::move(sketch_res.value());
  for (int i = 0; i < kFlushedAddCount; ++i) {
    ASSERT_TRUE(
        sketch.Add(std::string("item_") + std::to_string(i)).has_value());
  }

  auto estimate = sketch.Result();
  ASSERT_TRUE(estimate.has_value());
  EXPECT_GT(estimate.value(), 0);

  auto ser = sketch.Serialize();
  ASSERT_TRUE(ser.has_value());
  auto reparsed = hll::State::Parse(ser.value());
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->sparse_data.has_value());
  EXPECT_FALSE(reparsed->data.has_value());
}

}  // namespace
}  // namespace zetasketch
