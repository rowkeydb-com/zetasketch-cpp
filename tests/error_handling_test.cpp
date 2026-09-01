#include <cstdint>
#include <string>
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

// Result must report a defective stored stream, not the estimate 0.
// Estimation flushes buffered values before it counts, so one buffered
// addition makes the walk of the defective stream unavoidable.
TEST(ErrorHandlingTest, ResultReportsMalformedSparseData) {
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
  ASSERT_TRUE(sketch.AddHash(0x123456789ABCDEF0ULL).has_value());

  auto result = sketch.Result();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

// Enough additions fill the buffer and flush it into the stored
// stream; on a sketch holding defective sparse data the addition that
// triggers the flush must report the defect instead of succeeding.
TEST(ErrorHandlingTest, AddReportsMalformedSparseDataWhenBufferFlushes) {
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

  // The buffer flushes when its 257th element arrives at precision
  // 10; every earlier addition only buffers and must succeed.
  for (int i = 0; i < 256; ++i) {
    ASSERT_TRUE(
        sketch.Add(std::string("value_") + std::to_string(i)).has_value());
  }
  auto res = sketch.Add(std::string("value_256"));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

// With nothing buffered there is nothing to flush, so estimation does
// not walk the stored bytes and cannot see the defect. This pins the
// documented laziness; complete validation of untrusted bytes is a
// separate concern from estimation.
TEST(ErrorHandlingTest, ResultDoesNotWalkStoredBytesWithNothingBuffered) {
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
  auto result = sketch_res.value().Result();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 1);
}

// After an addition fails on the flush of a defective stored stream,
// the sketch is intact: estimation and serialization deterministically
// report the same defect rather than succeeding on partial state.
TEST(ErrorHandlingTest, ResultAndSerializeRepeatTheErrorAfterFailedAdd) {
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

  // The buffer flushes when its 257th element arrives at precision
  // 10; every earlier addition only buffers and must succeed.
  for (int i = 0; i < 256; ++i) {
    ASSERT_TRUE(
        sketch.Add(std::string("value_") + std::to_string(i)).has_value());
  }
  ASSERT_FALSE(sketch.Add(std::string("value_256")).has_value());

  auto result = sketch.Result();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, zetasketch::utils::ErrorCode::kInvalidState);

  auto ser = sketch.Serialize();
  ASSERT_FALSE(ser.has_value());
  EXPECT_EQ(ser.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

// A well-formed varint whose decoded index lies past the sparse
// precision must be refused when the flush merges it, not survive into
// the stream to fail only inside a later normalization.
TEST(ErrorHandlingTest, FlushRejectsOutOfRangeIndexInStoredStream) {
  State bad;
  bad.encoding_version = 2;
  bad.precision = 10;
  bad.sparse_precision = 15;
  bad.sparse_size = 1;
  // A complete five-byte varint encoding an index far out of range.
  bad.sparse_data = std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
  auto bytes = bad.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto sketch_res = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(sketch_res.has_value());
  auto sketch = std::move(sketch_res.value());
  ASSERT_TRUE(sketch.AddHash(0x123456789ABCDEF0ULL).has_value());

  auto ser = sketch.Serialize();
  ASSERT_FALSE(ser.has_value());
  EXPECT_EQ(ser.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

// The integer overload shares the error contract of the string one.
TEST(ErrorHandlingTest, IntegerAddReportsMalformedSparseDataWhenFlushing) {
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

  // The buffer flushes when its 257th element arrives at precision
  // 10; every earlier addition only buffers and must succeed.
  for (int64_t i = 0; i < 256; ++i) {
    ASSERT_TRUE(sketch.Add(i).has_value());
  }
  auto res = sketch.Add(int64_t{256});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

// The flush's index check reports through the addition that triggers
// the flush, and an index failure, like a decoder failure, leaves the
// stored state unchanged: estimation and serialization repeat the
// error.
TEST(ErrorHandlingTest, AddReportsOutOfRangeIndexAndStoredStateUnchanged) {
  State bad;
  bad.encoding_version = 2;
  bad.precision = 10;
  bad.sparse_precision = 15;
  bad.sparse_size = 1;
  bad.sparse_data = std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
  auto bytes = bad.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto sketch_res = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(sketch_res.has_value());
  auto sketch = std::move(sketch_res.value());

  // The buffer flushes when its 257th element arrives at precision
  // 10; every earlier addition only buffers and must succeed.
  for (int i = 0; i < 256; ++i) {
    ASSERT_TRUE(
        sketch.Add(std::string("value_") + std::to_string(i)).has_value());
  }
  auto res = sketch.Add(std::string("value_256"));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, zetasketch::utils::ErrorCode::kInvalidState);

  auto result = sketch.Result();
  ASSERT_FALSE(result.has_value());
  auto ser = sketch.Serialize();
  ASSERT_FALSE(ser.has_value());
}

// An addition to a sketch whose stored stream already exceeds the
// promotion threshold normalizes directly, with no flush. On this
// route a defect found mid-walk is reported, but the representation
// has already been moved from, and a later serialization succeeds on
// the remnant. This pins the current limit of the after-failure
// guarantee, which holds only for the flush route; complete
// validation of untrusted bytes at deserialization is the remedy.
TEST(ErrorHandlingTest, DirectNormalizationFailureDoesNotPreserveTheSketch) {
  State bad;
  bad.encoding_version = 2;
  bad.precision = 10;
  bad.sparse_precision = 15;
  bad.sparse_size = 1;
  // 768 one-byte varints keep every cumulative index in range and put
  // the stream at the promotion threshold; the final delta of 40000
  // pushes the cumulative index past 2^15.
  std::vector<uint8_t> stream(768, 0x01);
  stream.push_back(0xC0);
  stream.push_back(0xB8);
  stream.push_back(0x02);
  bad.sparse_data = std::move(stream);
  auto bytes = bad.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto sketch_res = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(sketch_res.has_value());
  auto sketch = std::move(sketch_res.value());

  auto add_res = sketch.Add(std::string("first"));
  ASSERT_FALSE(add_res.has_value());

  auto ser = sketch.Serialize();
  EXPECT_TRUE(ser.has_value());
}

// Serialization of a defective stream at the promotion threshold is
// not verbatim even with nothing buffered: compaction normalizes at
// the threshold, walks the stream, and reports the defect.
TEST(ErrorHandlingTest, SerializeReportsDefectiveStreamAtThreshold) {
  State bad;
  bad.encoding_version = 2;
  bad.precision = 10;
  bad.sparse_precision = 15;
  bad.sparse_size = 1;
  std::vector<uint8_t> stream(768, 0x01);
  stream.push_back(0xC0);
  stream.push_back(0xB8);
  stream.push_back(0x02);
  bad.sparse_data = std::move(stream);
  auto bytes = bad.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto sketch_res = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(sketch_res.has_value());
  auto ser = sketch_res.value().Serialize();
  ASSERT_FALSE(ser.has_value());
  EXPECT_EQ(ser.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

// A valid stored stream at the promotion threshold is normalized by
// serialization even with nothing buffered: the emitted form is the
// normal one, not the stored bytes verbatim.
TEST(ErrorHandlingTest, SerializeNormalizesValidStreamAtThreshold) {
  State stored;
  stored.encoding_version = 2;
  stored.precision = 10;
  stored.sparse_precision = 15;
  stored.sparse_size = 768;
  stored.sparse_data = std::vector<uint8_t>(768, 0x01);
  auto bytes = stored.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto sketch_res = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(sketch_res.has_value());
  auto ser = sketch_res.value().Serialize();
  ASSERT_TRUE(ser.has_value());

  auto reparsed = State::Parse(ser.value());
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->data.has_value());
  EXPECT_FALSE(reparsed->sparse_data.has_value());
}

// An addition to a valid stored stream at the promotion threshold
// takes the direct-normalization route and succeeds.
TEST(ErrorHandlingTest, AddSucceedsOnValidStreamAtThreshold) {
  State stored;
  stored.encoding_version = 2;
  stored.precision = 10;
  stored.sparse_precision = 15;
  stored.sparse_size = 768;
  stored.sparse_data = std::vector<uint8_t>(768, 0x01);
  auto bytes = stored.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto sketch_res = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(sketch_res.has_value());
  auto sketch = std::move(sketch_res.value());
  ASSERT_TRUE(sketch.Add(std::string("first")).has_value());
  auto result = sketch.Result();
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result.value(), 0);
}

// Estimation stays lazy at the promotion threshold: unlike
// serialization it flushes only and never normalizes, so a defective
// stream there is not walked when nothing is buffered.
TEST(ErrorHandlingTest, ResultStaysLazyOnDefectiveStreamAtThreshold) {
  State bad;
  bad.encoding_version = 2;
  bad.precision = 10;
  bad.sparse_precision = 15;
  bad.sparse_size = 1;
  std::vector<uint8_t> stream(768, 0x01);
  stream.push_back(0xC0);
  stream.push_back(0xB8);
  stream.push_back(0x02);
  bad.sparse_data = std::move(stream);
  auto bytes = bad.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto sketch_res = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(sketch_res.has_value());
  auto result = sketch_res.value().Result();
  EXPECT_TRUE(result.has_value());
}

// With nothing buffered and the stream below the promotion threshold,
// serialization emits the stored bytes verbatim, defective or not;
// validation happens only on a walk.
TEST(ErrorHandlingTest, SerializeRoundTripsStoredBytesWithNothingBuffered) {
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
  auto ser = sketch_res.value().Serialize();
  ASSERT_TRUE(ser.has_value());
  EXPECT_EQ(ser.value(), bytes.value());
}

}  // namespace
// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
