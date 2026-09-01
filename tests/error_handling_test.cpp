#include <cstdint>
#include <utility>
// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
#include <vector>
#include <gtest/gtest.h>
#include "zetasketch/hll/encoding.h"
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/sparse_representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/hyperloglogplusplus.h"
#include "zetasketch/utils/error.h"

namespace {

using zetasketch::HyperLogLogPlusPlus;
using zetasketch::hll::NormalRepresentation;
using zetasketch::hll::SparseRepresentation;
using zetasketch::hll::State;

auto CreateNormalRepresentation(int precision, int sparse_precision,
                                int data_size) {
  State state;
  state.precision = precision;
  state.sparse_precision = sparse_precision;
  state.data = std::vector<uint8_t>(data_size, 0);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  return NormalRepresentation::Create(std::move(state));
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

TEST(ErrorHandlingTest, HllMergeIncompatiblePrecision) {
  auto hll1 = HyperLogLogPlusPlus::Create(10, 15).value();
  auto hll2 = HyperLogLogPlusPlus::Create(12, 15).value();

  auto res = hll1.Merge(std::move(hll2));
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code,
            zetasketch::utils::ErrorCode::kIncompatiblePrecision);
}

TEST(ErrorHandlingTest, NormalCreateInvalidDataLength) {
  State state;
  state.precision = 10;
  state.sparse_precision = 15;
  state.data = std::vector<uint8_t>(500, 0);

  auto res = NormalRepresentation::Create(std::move(state));
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

TEST(ErrorHandlingTest, NormalMergeDowngradePath) {
  auto res1 = CreateNormalRepresentation(10, 15, 1024);
  ASSERT_TRUE(res1.has_value());
  auto norm1 = std::move(res1.value());

  State state2;
  state2.precision = 12;
  state2.sparse_precision = 15;
  state2.data = std::vector<uint8_t>(4096, 0);
  state2.data.value()[1000] = 5;
  state2.data.value()[3000] = 10;
  auto res2 = NormalRepresentation::Create(std::move(state2));
  ASSERT_TRUE(res2.has_value());
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  auto norm2 = std::move(res2.value());
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  auto res = norm1.MergeFromNormal(std::move(norm2));
  EXPECT_TRUE(res.has_value());
}

TEST(ErrorHandlingTest, NormalAddSparseValueOutOfBounds) {
  auto res1 = CreateNormalRepresentation(10, 15, 1024);
  ASSERT_TRUE(res1.has_value());
  auto norm1 = std::move(res1.value());

  auto sparse_enc = zetasketch::hll::encoding::Sparse::Create(10, 15).value();
  auto add_res = norm1.AddSparseValue(sparse_enc, 0xFFFFFFFF);
  EXPECT_FALSE(add_res.has_value());
}

TEST(ErrorHandlingTest, HllFromBytesInvalidData) {
  std::vector<uint8_t> bad_bytes = {0x00, 0xFF, 0x11, 0x22};
  auto res = HyperLogLogPlusPlus::FromBytes(bad_bytes);
  EXPECT_FALSE(res.has_value());
}

TEST(ErrorHandlingTest, SparseNormalizeInvalidSparseIndex) {
  State state;
  state.precision = 10;
  state.sparse_precision = 15;
  state.sparse_data = std::vector<uint8_t>();
  // Push varint for a huge index difference to exceed bounds (e.g., 0xFFFFFFFF)
  state.sparse_data->push_back(0xFF);
  state.sparse_data->push_back(0xFF);
  state.sparse_data->push_back(0xFF);
  state.sparse_data->push_back(0xFF);
  state.sparse_data->push_back(0x0F);

  auto res = SparseRepresentation::Create(std::move(state));
  ASSERT_TRUE(res.has_value());
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  auto norm_res = std::move(res.value()).Normalize();
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  EXPECT_FALSE(norm_res.has_value());
  if (!norm_res.has_value()) {
    EXPECT_EQ(norm_res.error().code,
              zetasketch::utils::ErrorCode::kInvalidState);
  }
}

TEST(ErrorHandlingTest, SparseNormalizeTruncatedVarint) {
  State state;
  state.precision = 10;
  state.sparse_precision = 15;
  // Every byte carries the continuation bit, so the varint never ends.
  state.sparse_data = std::vector<uint8_t>{0xFF, 0xFF};

  auto res = SparseRepresentation::Create(std::move(state));
  ASSERT_TRUE(res.has_value());
// GCC reports a maybe-uninitialized false positive on the value moved
// out of the std::expected in the call below; the same class of
// false positive is suppressed throughout this file.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  auto norm_res = std::move(res.value()).Normalize();
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  ASSERT_FALSE(norm_res.has_value());
  EXPECT_EQ(norm_res.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

TEST(ErrorHandlingTest, SparseNormalizeVarintContinuationOnFinalByte) {
  State state;
  state.precision = 10;
  state.sparse_precision = 15;
  // A complete varint, then a final byte whose continuation bit is set.
  state.sparse_data = std::vector<uint8_t>{0x02, 0x80};

  auto res = SparseRepresentation::Create(std::move(state));
  ASSERT_TRUE(res.has_value());
// GCC reports a maybe-uninitialized false positive on the value moved
// out of the std::expected in the call below; the same class of
// false positive is suppressed throughout this file.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  auto norm_res = std::move(res.value()).Normalize();
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  ASSERT_FALSE(norm_res.has_value());
  EXPECT_EQ(norm_res.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

TEST(ErrorHandlingTest, SparseNormalizeOverlongVarint) {
  State state;
  state.precision = 10;
  state.sparse_precision = 15;
  // Five continuation bytes exceed the maximum encoded length of a
  // 32-bit value, whatever follows them.
  state.sparse_data = std::vector<uint8_t>{0x80, 0x80, 0x80, 0x80, 0x80, 0x01};

  auto res = SparseRepresentation::Create(std::move(state));
  ASSERT_TRUE(res.has_value());
// GCC reports a maybe-uninitialized false positive on the value moved
// out of the std::expected in the call below; the same class of
// false positive is suppressed throughout this file.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  auto norm_res = std::move(res.value()).Normalize();
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  ASSERT_FALSE(norm_res.has_value());
  EXPECT_EQ(norm_res.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

// A serialized sketch whose envelope is valid but whose sparse data is
// a truncated varint deserializes successfully, because deserialization
// validates the envelope only. The defect must then surface as an error
// from the first operation that walks the sparse data, not as an
// out-of-bounds read.
TEST(ErrorHandlingTest, MergeSparseReportsMalformedSparseDataInOperand) {
  State bad;
  bad.encoding_version = 2;
  bad.precision = 10;
  bad.sparse_precision = 15;
  bad.sparse_size = 1;
  bad.sparse_data = std::vector<uint8_t>{0x80};
  auto bytes = bad.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto operand = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(operand.has_value());

  auto target_res = HyperLogLogPlusPlus::Create(10, 15);
  ASSERT_TRUE(target_res.has_value());
  auto target = std::move(target_res.value());
  auto merge_res = target.Merge(std::move(operand.value()));
  ASSERT_FALSE(merge_res.has_value());
  EXPECT_EQ(merge_res.error().code,
            zetasketch::utils::ErrorCode::kInvalidState);
}

// A defective stored sparse stream must also surface when buffered
// additions force serialization's compaction to merge with it. This is
// the flush path, distinct from normalization and from merging.
TEST(ErrorHandlingTest, SerializeReportsMalformedSparseDataWhenFlushing) {
  State bad;
  bad.encoding_version = 2;
  bad.precision = 10;
  bad.sparse_precision = 15;
  bad.sparse_size = 1;
  bad.sparse_data = std::vector<uint8_t>{0x80};
  auto bytes = bad.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto sketch_res = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(sketch_res.has_value());
  auto sketch = std::move(sketch_res.value());

  // One buffered value: too few to flush on its own, so the walk of
  // the stored stream happens inside serialization's compaction.
  ASSERT_TRUE(sketch.AddHash(0x123456789ABCDEF0ULL).has_value());

  auto ser = sketch.Serialize();
  ASSERT_FALSE(ser.has_value());
  EXPECT_EQ(ser.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

TEST(ErrorHandlingTest, MergeIntoNormalReportsMalformedSparseDataInOperand) {
  State bad;
  bad.encoding_version = 2;
  bad.precision = 10;
  bad.sparse_precision = 15;
  bad.sparse_size = 1;
  bad.sparse_data = std::vector<uint8_t>{0x80};
  auto bytes = bad.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto operand = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(operand.has_value());

  // A normal-representation target forces normalization of the sparse
  // operand, which walks its sparse data.
  auto target_res = HyperLogLogPlusPlus::Create(10, 0);
  ASSERT_TRUE(target_res.has_value());
  auto target = std::move(target_res.value());
  auto merge_res = target.Merge(std::move(operand.value()));
  ASSERT_FALSE(merge_res.has_value());
  EXPECT_EQ(merge_res.error().code,
            zetasketch::utils::ErrorCode::kInvalidState);
}

}  // namespace
// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
