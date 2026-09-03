// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hyperloglogplusplus.h"
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/sparse_representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/error.h"

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

// The reference fixes an aggregator's value type when it is built and
// refuses an integer added to one built for text. This library's
// factory builds for text, as the reference's buildForStrings does, so
// a sketch that admits integers is obtained by reading one that records
// no value type, which is what the reference's forProto returns for a
// state without the field. The integer factory arrives with the integer
// hash, which is still a placeholder.
std::expected<HyperLogLogPlusPlus, utils::Error> SketchAdmittingIntegers(
    int32_t precision, int32_t sparse_precision) {
  hll::State state;
  state.encoding_version = 2;
  state.precision = precision;
  state.sparse_precision = sparse_precision;
  state.value_type = hll::ValueType::kUnknown;
  auto bytes = state.ToByteArray();
  if (!bytes.has_value()) {
    return std::unexpected(bytes.error());
  }
  return HyperLogLogPlusPlus::FromBytes(bytes.value());
}

TEST(HyperLogLogPlusPlusTest, RoundTripSerializationDense) {
  auto sketch_res = SketchAdmittingIntegers(
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
      SketchAdmittingIntegers(kTestNormalPrecision, kTestSparsePrecision);
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
      SketchAdmittingIntegers(kLowNormalPrecision, kLowSparsePrecision);
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
      SketchAdmittingIntegers(kLowNormalPrecision, kLowSparsePrecision);
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
      SketchAdmittingIntegers(kLowNormalPrecision, kLowSparsePrecision);
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
  const std::vector<uint8_t> java_empty = {0x08, 0x70, 0x10, 0x00, 0x18, 0x02,
                                           0x20, 0x0B, 0x82, 0x07, 0x06, 0x18,
                                           0x0A, 0x20, 0x0F, 0x32, 0x00};
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
  auto first_res = SketchAdmittingIntegers(
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

  auto one_pass_res = SketchAdmittingIntegers(
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

// The advertised limits must be the limits the implementation
// enforces, and must equal the reference implementation's public
// constants: normal precision 4 through 24, sparse precision at
// most 25.
TEST(HyperLogLogPlusPlusTest, AdvertisedPrecisionLimitsAreTheEnforcedOnes) {
  static_assert(HyperLogLogPlusPlus::kMinimumPrecision ==
                hll::NormalRepresentation::kMinimumPrecision);
  static_assert(HyperLogLogPlusPlus::kMaximumPrecision ==
                hll::NormalRepresentation::kMaximumPrecision);
  static_assert(HyperLogLogPlusPlus::kMaximumSparsePrecision ==
                hll::SparseRepresentation::kMaximumSparsePrecision);
  EXPECT_EQ(HyperLogLogPlusPlus::kMinimumPrecision, 4);
  EXPECT_EQ(HyperLogLogPlusPlus::kMaximumPrecision, 24);
  EXPECT_EQ(HyperLogLogPlusPlus::kMaximumSparsePrecision, 25);
}

// The two extreme configurations the Java library accepts. The
// expected bytes are what it emits for an empty sketch at each,
// obtained by running it at those precisions.
TEST(HyperLogLogPlusPlusTest, CreateAcceptsThePrecisionLimits) {
  const std::vector<uint8_t> java_empty_15_25 = {
      0x08, 0x70, 0x10, 0x00, 0x18, 0x02, 0x20, 0x0B, 0x82,
      0x07, 0x06, 0x18, 0x0F, 0x20, 0x19, 0x32, 0x00};
  const std::vector<uint8_t> java_empty_24_25 = {
      0x08, 0x70, 0x10, 0x00, 0x18, 0x02, 0x20, 0x0B, 0x82,
      0x07, 0x06, 0x18, 0x18, 0x20, 0x19, 0x32, 0x00};

  auto highest_sparse = HyperLogLogPlusPlus::Create(
      kTestNormalPrecision, HyperLogLogPlusPlus::kMaximumSparsePrecision);
  ASSERT_TRUE(highest_sparse.has_value());
  auto highest_sparse_bytes = highest_sparse.value().Serialize();
  ASSERT_TRUE(highest_sparse_bytes.has_value());
  EXPECT_EQ(highest_sparse_bytes.value(), java_empty_15_25);

  auto both_maxima =
      HyperLogLogPlusPlus::Create(HyperLogLogPlusPlus::kMaximumPrecision,
                                  HyperLogLogPlusPlus::kMaximumSparsePrecision);
  ASSERT_TRUE(both_maxima.has_value());
  auto both_maxima_bytes = both_maxima.value().Serialize();
  ASSERT_TRUE(both_maxima_bytes.has_value());
  EXPECT_EQ(both_maxima_bytes.value(), java_empty_24_25);

  // A sparse precision equal to the normal precision is the other
  // edge of the accepted range.
  auto equal_precisions =
      HyperLogLogPlusPlus::Create(kTestNormalPrecision, kTestNormalPrecision);
  EXPECT_TRUE(equal_precisions.has_value());
}

// The whole acceptance product: every combination of a normal
// precision at and around both limits with a sparse precision that is
// disabled, below the normal precision, equal to it, five above it, at
// the maximum, and one past the maximum. The rule is the reference
// implementation's own: the normal precision must lie between the
// minimum and the maximum, and the sparse precision must be either
// disabled or between the normal precision and the sparse maximum.
TEST(HyperLogLogPlusPlusTest, CreateAcceptsExactlyWhatTheReferenceAccepts) {
  const std::vector<int32_t> normal_precisions = {3, 4,  5,  6,  7,  8,
                                                  9, 10, 15, 23, 24, 25};
  for (const int32_t np : normal_precisions) {
    const std::vector<int32_t> sparse_precisions = {
        HyperLogLogPlusPlus::kSparsePrecisionDisabled,
        np - 1,
        np,
        np + HyperLogLogPlusPlus::kDefaultSparsePrecisionDelta,
        HyperLogLogPlusPlus::kMaximumSparsePrecision,
        HyperLogLogPlusPlus::kMaximumSparsePrecision + 1};
    for (const int32_t sp : sparse_precisions) {
      // The reference's published bounds, stated as literals so that
      // this expectation is independent of the constants the code
      // under test compares against. Confirmed by running the
      // reference over these same seventy-two configurations.
      const bool normal_in_range = np >= 4 && np <= 24;
      const bool sparse_in_range = sp == 0 || (sp >= np && sp <= 25);
      const bool expected = normal_in_range && sparse_in_range;

      auto res = HyperLogLogPlusPlus::Create(np, sp);
      EXPECT_EQ(res.has_value(), expected)
          << "normal precision " << np << ", sparse precision " << sp;
      if (!res.has_value()) {
        EXPECT_EQ(res.error().code, utils::ErrorCode::kIllegalArgument)
            << "normal precision " << np << ", sparse precision " << sp;
      }
    }
  }
}

// The refusal messages are the reference implementation's own, word
// for word, so that an operator reading one of ours finds the same
// text as in its documentation.
TEST(HyperLogLogPlusPlusTest, RefusalMessagesMatchTheReference) {
  auto below_minimum = HyperLogLogPlusPlus::Create(
      HyperLogLogPlusPlus::kMinimumPrecision - 1, kTestSparsePrecision);
  ASSERT_FALSE(below_minimum.has_value());
  EXPECT_EQ(below_minimum.error().message,
            "Expected normal precision to be >= 4 and <= 24 but was 3");

  // Above the maximum on the normal axis alone: this sparse precision
  // is legal in itself and against the normal precision, so only the
  // normal bound can refuse it.
  auto above_maximum =
      HyperLogLogPlusPlus::Create(HyperLogLogPlusPlus::kMaximumPrecision + 1,
                                  HyperLogLogPlusPlus::kMaximumSparsePrecision);
  ASSERT_FALSE(above_maximum.has_value());
  EXPECT_EQ(above_maximum.error().message,
            "Expected normal precision to be >= 4 and <= 24 but was 25");

  auto sparse_too_high = HyperLogLogPlusPlus::Create(
      kTestNormalPrecision, HyperLogLogPlusPlus::kMaximumSparsePrecision + 1);
  ASSERT_FALSE(sparse_too_high.has_value());
  EXPECT_EQ(sparse_too_high.error().message,
            "Expected sparse precision to be >= normal precision (15) and "
            "<= 25 but was 26.");
}

// Above the precisions the bias tables cover, the correction is zero
// and the linear counting threshold falls back to five halves of the
// bucket count. Nothing else in the suite estimates a sketch at these
// precisions. The expected values are the reference's own.
TEST(HyperLogLogPlusPlusTest, EstimatesAboveTheTabulatedPrecisions) {
  constexpr int32_t kFirstUntabulated = 23;
  constexpr int32_t kSecondUntabulated = 24;
  constexpr int kPopulation = 100;

  for (const int32_t precision : {kFirstUntabulated, kSecondUntabulated}) {
    auto sketch_res = HyperLogLogPlusPlus::Create(
        precision, HyperLogLogPlusPlus::kSparsePrecisionDisabled);
    ASSERT_TRUE(sketch_res.has_value()) << "precision " << precision;
    auto sketch = std::move(sketch_res.value());
    for (int i = 1; i <= kPopulation; ++i) {
      ASSERT_TRUE(
          sketch.Add(std::string("item_") + std::to_string(i)).has_value());
    }
    auto estimate = sketch.Result();
    ASSERT_TRUE(estimate.has_value()) << "precision " << precision;
    EXPECT_EQ(estimate.value(), kPopulation) << "precision " << precision;
  }
}

// The reference's own lowest-precision cases, ported to the string
// path because the integer path is still a placeholder. A single
// value at the minimum precision, with sparse mode disabled and with
// it equal to the normal precision: the estimate is one, the bytes
// are the reference's own, and a sketch parsed back from them
// estimates the same.
TEST(HyperLogLogPlusPlusTest, LowestPrecisionBasicOperations) {
  const std::vector<uint8_t> reference_dense = {
      0x08, 0x70, 0x10, 0x01, 0x18, 0x02, 0x20, 0x0B, 0x82, 0x07, 0x14,
      0x18, 0x04, 0x2A, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const std::vector<uint8_t> reference_sparse = {
      0x08, 0x70, 0x10, 0x01, 0x18, 0x02, 0x20, 0x0B, 0x82, 0x07, 0x0A,
      0x10, 0x01, 0x18, 0x04, 0x20, 0x04, 0x32, 0x02, 0xC2, 0x0B};

  const auto check = [](int32_t sparse_precision,
                        const std::vector<uint8_t>& expected) {
    auto sketch_res = HyperLogLogPlusPlus::Create(
        HyperLogLogPlusPlus::kMinimumPrecision, sparse_precision);
    ASSERT_TRUE(sketch_res.has_value());
    auto sketch = std::move(sketch_res.value());
    ASSERT_TRUE(sketch.Add(std::string("item_1")).has_value());

    auto estimate = sketch.Result();
    ASSERT_TRUE(estimate.has_value());
    EXPECT_EQ(estimate.value(), 1);

    auto bytes = sketch.Serialize();
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes.value(), expected);

    auto restored = HyperLogLogPlusPlus::FromBytes(bytes.value());
    ASSERT_TRUE(restored.has_value());
    auto restored_estimate = restored.value().Result();
    ASSERT_TRUE(restored_estimate.has_value());
    EXPECT_EQ(restored_estimate.value(), 1);
  };

  check(HyperLogLogPlusPlus::kSparsePrecisionDisabled, reference_dense);
  check(HyperLogLogPlusPlus::kMinimumPrecision, reference_sparse);
}

}  // namespace
}  // namespace zetasketch
