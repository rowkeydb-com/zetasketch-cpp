#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
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

// Decodes a sketch written as hexadecimal, which is the form in which
// the reference harness reports the bytes it reads and writes.
std::vector<uint8_t> DecodeHex(std::string_view hex) {
  const auto digit = [](char character) -> uint8_t {
    return static_cast<uint8_t>(character <= '9' ? character - '0'
                                                 : character - 'a' + 10);
  };
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(static_cast<uint8_t>(
        static_cast<unsigned>(digit(hex[i])) << 4U | digit(hex[i + 1])));
  }
  return bytes;
}

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

// A merge lowers whichever operand is higher, so a difference in one
// precision alone is merged rather than refused. Only a pair where one
// precision rises while the other falls has no common encoding to lower
// into, and the reference refuses that pair in the words below, in
// whichever order it is given. Every verdict and message here is the
// reference's own.
TEST(ErrorHandlingTest, MergeRefusesOnlyEncodingsWithNoCommonPrecision) {
  struct Pair {
    const char* description;
    int32_t target_precision;
    int32_t target_sparse_precision;
    int32_t operand_precision;
    int32_t operand_sparse_precision;
    bool merged;
    std::string_view message;
  };
  const std::vector<Pair> pairs = {
      {"the same precisions", 10, 15, 10, 15, true, ""},
      {"a lower normal precision alone", 10, 15, 12, 15, true, ""},
      {"a higher normal precision alone", 12, 15, 10, 15, true, ""},
      {"a lower sparse precision alone", 10, 15, 10, 20, true, ""},
      {"a higher sparse precision alone", 10, 20, 10, 15, true, ""},
      {"both precisions lower", 10, 15, 12, 20, true, ""},
      {"both precisions higher", 12, 20, 10, 15, true, ""},
      {"one precision higher and the other lower", 10, 20, 12, 15, false,
       "Precisions (p=10, sp=20) are not compatible to (p=12, sp=15)"},
      {"the same pair in the other order", 12, 15, 10, 20, false,
       "Precisions (p=12, sp=15) are not compatible to (p=10, sp=20)"},
  };

  for (const auto& pair : pairs) {
    auto target = HyperLogLogPlusPlus::Create(pair.target_precision,
                                              pair.target_sparse_precision);
    ASSERT_TRUE(target.has_value()) << pair.description;
    auto operand = HyperLogLogPlusPlus::Create(pair.operand_precision,
                                               pair.operand_sparse_precision);
    ASSERT_TRUE(operand.has_value()) << pair.description;

    auto merged = target.value().Merge(std::move(operand.value()));
    ASSERT_EQ(merged.has_value(), pair.merged) << pair.description;
    if (!pair.merged) {
      EXPECT_EQ(merged.error().code,
                zetasketch::utils::ErrorCode::kIncompatiblePrecision)
          << pair.description;
      EXPECT_EQ(merged.error().message, pair.message) << pair.description;
    }
  }
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
// The reference bounds a sparse value only where it encodes one, and
// bounds it against zero rather than against the sparse precision: its
// difference encoder refuses a value that is negative when read as a
// signed 32-bit integer, in the words asserted below, and accepts every
// other value however large the index it decodes to. The stored stream
// here holds such a negative value.
TEST(ErrorHandlingTest, FlushRejectsNegativeValueInStoredStream) {
  State bad;
  bad.encoding_version = 2;
  bad.precision = 10;
  bad.sparse_precision = 15;
  bad.sparse_size = 1;
  // A complete five-byte varint whose value is negative when read as a
  // signed 32-bit integer.
  bad.sparse_data = std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
  auto bytes = bad.ToByteArray();
  ASSERT_TRUE(bytes.has_value());

  auto sketch_res = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_TRUE(sketch_res.has_value());
  auto sketch = std::move(sketch_res.value());
  ASSERT_TRUE(sketch.AddHash(0x123456789ABCDEF0ULL).has_value());

  auto ser = sketch.Serialize();
  ASSERT_FALSE(ser.has_value());
  EXPECT_EQ(ser.error().code, zetasketch::utils::ErrorCode::kIllegalArgument);
  EXPECT_EQ(ser.error().message, "only positive integers supported but got -1");
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

// The flush's bound reports through the addition that triggers the
// flush, and such a failure, like a decoder failure, leaves the stored
// state unchanged: estimation and serialization repeat the error.
TEST(ErrorHandlingTest, AddReportsNegativeValueAndStoredStateUnchanged) {
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
  EXPECT_EQ(res.error().code, zetasketch::utils::ErrorCode::kIllegalArgument);
  EXPECT_EQ(res.error().message, "only positive integers supported but got -1");

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

// A sparse sketch whose sparse precision equals its normal precision,
// holding one value that carries no flag bit. Nothing this library
// writes takes that form, because at equal precisions every value it
// encodes is flagged, but a deserialised sketch may hold one.
// Normalising it recomputes the rank from the value itself, shifting
// by the full width: the reference's language reduces that count
// modulo the width and ours now does the same, which is why the
// register below is 62 rather than anything else. The expected bytes
// are the reference's own output for this merge.
TEST(ErrorHandlingTest, MergingAnUnflaggedValueAtEqualPrecisions) {
  const std::vector<uint8_t> unflagged = {
      0x08, 0x70, 0x10, 0x01, 0x18, 0x02, 0x20, 0x0B, 0x82, 0x07,
      0x09, 0x10, 0x01, 0x18, 0x04, 0x20, 0x04, 0x32, 0x01, 0x05};
  const std::vector<uint8_t> reference_merge = {
      0x08, 0x70, 0x10, 0x15, 0x18, 0x02, 0x20, 0x0B, 0x82, 0x07, 0x14,
      0x18, 0x04, 0x2A, 0x10, 0x01, 0x06, 0x02, 0x02, 0x04, 0x3E, 0x02,
      0x02, 0x02, 0x00, 0x00, 0x02, 0x00, 0x02, 0x02, 0x04};

  auto operand = HyperLogLogPlusPlus::FromBytes(unflagged);
  ASSERT_TRUE(operand.has_value());

  auto target_res = HyperLogLogPlusPlus::Create(4, 0);
  ASSERT_TRUE(target_res.has_value());
  auto target = std::move(target_res.value());
  for (int i = 1; i <= 20; ++i) {
    ASSERT_TRUE(
        target.Add(std::string("item_") + std::to_string(i)).has_value());
  }

  ASSERT_TRUE(target.Merge(std::move(operand.value())).has_value());
  auto bytes = target.Serialize();
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(bytes.value(), reference_merge);
}

// Which representation a sketch becomes is decided before anything is
// validated, and the choice decides which precisions are inspected: a
// sketch carrying dense data, or none at all with sparse mode
// disabled, becomes a normal representation, whose construction never
// looks at the sparse precision. So the reference accepts a dense
// sketch whose sparse precision is out of range and refuses the same
// sparse precision when there is no dense data to read.
//
// The reference also distinguishes a field that is present from a
// field that holds a byte: both of its selection predicates require a
// byte, so a present but empty data field is invisible to the choice
// and an empty sparse data field beside no sparse precision is
// accepted rather than refused. The two empty contents below therefore
// stand beside the occupied ones, and are expected to be accepted
// wherever nothing at all is.
//
// The product below is five normal precisions, at and around both
// limits, by five sparse precisions each: none, one below the normal
// precision, the normal precision itself, the maximum accepted, and
// one above it. At precision 25 the third and fourth of those coincide,
// so the widest encodable sparse precision stands in for one of them
// and all five remain distinct. Each pair is taken with nothing
// stored, with sparse data, with an empty data field and with an empty
// sparse data field. The two contents that carry a full register array
// are taken only at the two smallest normal precisions, because such
// an array is sixteen megabytes at precision 24 and thirty-two at
// precision 25, which is more than a unit test should build. That is
// 120 shapes, all distinct. Every verdict was taken by running the
// reference over these same bytes.
TEST(ErrorHandlingTest, FromBytesMatchesTheReferenceOnEveryParseShape) {
  enum class Content {
    kNothing,
    kSparse,
    kEmptyDense,
    kEmptySparse,
    kDense,
    kDenseAndSparse
  };
  struct Shape {
    int32_t precision;
    int32_t sparse_precision;
    Content content;
    bool accepted;
  };
  const std::vector<Shape> shapes = {
      {3, 0, Content::kNothing, false},
      {3, 0, Content::kSparse, false},
      {3, 0, Content::kEmptyDense, false},
      {3, 0, Content::kEmptySparse, false},
      {3, 0, Content::kDense, false},
      {3, 0, Content::kDenseAndSparse, false},
      {3, 2, Content::kNothing, false},
      {3, 2, Content::kSparse, false},
      {3, 2, Content::kEmptyDense, false},
      {3, 2, Content::kEmptySparse, false},
      {3, 2, Content::kDense, false},
      {3, 2, Content::kDenseAndSparse, false},
      {3, 3, Content::kNothing, false},
      {3, 3, Content::kSparse, false},
      {3, 3, Content::kEmptyDense, false},
      {3, 3, Content::kEmptySparse, false},
      {3, 3, Content::kDense, false},
      {3, 3, Content::kDenseAndSparse, false},
      {3, 25, Content::kNothing, false},
      {3, 25, Content::kSparse, false},
      {3, 25, Content::kEmptyDense, false},
      {3, 25, Content::kEmptySparse, false},
      {3, 25, Content::kDense, false},
      {3, 25, Content::kDenseAndSparse, false},
      {3, 26, Content::kNothing, false},
      {3, 26, Content::kSparse, false},
      {3, 26, Content::kEmptyDense, false},
      {3, 26, Content::kEmptySparse, false},
      {3, 26, Content::kDense, false},
      {3, 26, Content::kDenseAndSparse, false},
      {4, 0, Content::kNothing, true},
      {4, 0, Content::kSparse, false},
      {4, 0, Content::kEmptyDense, true},
      {4, 0, Content::kEmptySparse, true},
      {4, 0, Content::kDense, true},
      {4, 0, Content::kDenseAndSparse, false},
      {4, 3, Content::kNothing, false},
      {4, 3, Content::kSparse, false},
      {4, 3, Content::kEmptyDense, false},
      {4, 3, Content::kEmptySparse, false},
      {4, 3, Content::kDense, true},
      {4, 3, Content::kDenseAndSparse, true},
      {4, 4, Content::kNothing, true},
      {4, 4, Content::kSparse, true},
      {4, 4, Content::kEmptyDense, true},
      {4, 4, Content::kEmptySparse, true},
      {4, 4, Content::kDense, true},
      {4, 4, Content::kDenseAndSparse, true},
      {4, 25, Content::kNothing, true},
      {4, 25, Content::kSparse, true},
      {4, 25, Content::kEmptyDense, true},
      {4, 25, Content::kEmptySparse, true},
      {4, 25, Content::kDense, true},
      {4, 25, Content::kDenseAndSparse, true},
      {4, 26, Content::kNothing, false},
      {4, 26, Content::kSparse, false},
      {4, 26, Content::kEmptyDense, false},
      {4, 26, Content::kEmptySparse, false},
      {4, 26, Content::kDense, true},
      {4, 26, Content::kDenseAndSparse, true},
      {15, 0, Content::kNothing, true},
      {15, 0, Content::kSparse, false},
      {15, 0, Content::kEmptyDense, true},
      {15, 0, Content::kEmptySparse, true},
      {15, 14, Content::kNothing, false},
      {15, 14, Content::kSparse, false},
      {15, 14, Content::kEmptyDense, false},
      {15, 14, Content::kEmptySparse, false},
      {15, 15, Content::kNothing, true},
      {15, 15, Content::kSparse, true},
      {15, 15, Content::kEmptyDense, true},
      {15, 15, Content::kEmptySparse, true},
      {15, 25, Content::kNothing, true},
      {15, 25, Content::kSparse, true},
      {15, 25, Content::kEmptyDense, true},
      {15, 25, Content::kEmptySparse, true},
      {15, 26, Content::kNothing, false},
      {15, 26, Content::kSparse, false},
      {15, 26, Content::kEmptyDense, false},
      {15, 26, Content::kEmptySparse, false},
      {24, 0, Content::kNothing, true},
      {24, 0, Content::kSparse, false},
      {24, 0, Content::kEmptyDense, true},
      {24, 0, Content::kEmptySparse, true},
      {24, 23, Content::kNothing, false},
      {24, 23, Content::kSparse, false},
      {24, 23, Content::kEmptyDense, false},
      {24, 23, Content::kEmptySparse, false},
      {24, 24, Content::kNothing, true},
      {24, 24, Content::kSparse, true},
      {24, 24, Content::kEmptyDense, true},
      {24, 24, Content::kEmptySparse, true},
      {24, 25, Content::kNothing, true},
      {24, 25, Content::kSparse, true},
      {24, 25, Content::kEmptyDense, true},
      {24, 25, Content::kEmptySparse, true},
      {24, 26, Content::kNothing, false},
      {24, 26, Content::kSparse, false},
      {24, 26, Content::kEmptyDense, false},
      {24, 26, Content::kEmptySparse, false},
      {25, 0, Content::kNothing, false},
      {25, 0, Content::kSparse, false},
      {25, 0, Content::kEmptyDense, false},
      {25, 0, Content::kEmptySparse, false},
      {25, 24, Content::kNothing, false},
      {25, 24, Content::kSparse, false},
      {25, 24, Content::kEmptyDense, false},
      {25, 24, Content::kEmptySparse, false},
      {25, 25, Content::kNothing, false},
      {25, 25, Content::kSparse, false},
      {25, 25, Content::kEmptyDense, false},
      {25, 25, Content::kEmptySparse, false},
      {25, 26, Content::kNothing, false},
      {25, 26, Content::kSparse, false},
      {25, 26, Content::kEmptyDense, false},
      {25, 26, Content::kEmptySparse, false},
      {25, 30, Content::kNothing, false},
      {25, 30, Content::kSparse, false},
      {25, 30, Content::kEmptyDense, false},
      {25, 30, Content::kEmptySparse, false},
  };

  for (const auto& shape : shapes) {
    State state;
    state.encoding_version = 2;
    state.precision = shape.precision;
    state.sparse_precision = shape.sparse_precision;
    switch (shape.content) {
      case Content::kNothing:
        break;
      case Content::kSparse:
        state.sparse_size = 1;
        state.sparse_data = std::vector<uint8_t>{0x05};
        break;
      case Content::kEmptyDense:
        state.data = std::vector<uint8_t>();
        break;
      case Content::kEmptySparse:
        state.sparse_data = std::vector<uint8_t>();
        break;
      case Content::kDense:
        state.data = std::vector<uint8_t>(
            size_t{1} << static_cast<size_t>(shape.precision), 0);
        break;
      case Content::kDenseAndSparse:
        state.data = std::vector<uint8_t>(
            size_t{1} << static_cast<size_t>(shape.precision), 0);
        state.sparse_size = 1;
        state.sparse_data = std::vector<uint8_t>{0x05};
        break;
    }

    auto bytes = state.ToByteArray();
    ASSERT_TRUE(bytes.has_value());
    auto sketch = HyperLogLogPlusPlus::FromBytes(bytes.value());
    EXPECT_EQ(sketch.has_value(), shape.accepted)
        << "normal precision " << shape.precision << ", sparse precision "
        << shape.sparse_precision << ", content "
        << static_cast<int>(shape.content);
  }
}

// A sketch the reference accepts is written back out by it exactly as
// recorded below; every pair is the reference's own input and its own
// output. Three of these shapes are the ones that distinguish a field
// which is present from a field which holds a byte. An empty data
// field is invisible to the choice of representation, so a sketch
// carrying one beside a sparse precision becomes sparse and keeps the
// empty field in its output; an empty sparse data field beside no
// sparse precision is accepted rather than refused; and a sketch
// carrying a register array and sparse data together keeps both,
// because the reference discards the sparse fields when it normalises
// and not when it constructs a normal representation. A sparse sketch
// that has nothing stored acquires an empty sparse data field, which
// is why two of the outputs are longer than their inputs.
TEST(ErrorHandlingTest, SerializingAParsedSketchReproducesTheReferenceBytes) {
  struct RoundTrip {
    const char* description;
    std::string_view parsed;
    std::string_view written;
  };
  const std::vector<RoundTrip> round_trips = {
      {"nothing stored and no sparse precision", "087010011802200b8207021804",
       "087010011802200b8207021804"},
      {"an empty data field and no sparse precision",
       "087010011802200b82070418042a00", "087010011802200b82070418042a00"},
      {"an empty sparse data field and no sparse precision",
       "087010011802200b82070418043200", "087010011802200b82070418043200"},
      {"a full register array and no sparse precision",
       "087010011802200b82071418042a1000000000000000000000000000000000",
       "087010011802200b82071418042a1000000000000000000000000000000000"},
      {"nothing stored beside a sparse precision",
       "087010011802200b82070418042004", "087010011802200b820706180420043200"},
      {"sparse data beside a sparse precision",
       "087010011802200b820709100118042004320105",
       "087010011802200b820709100118042004320105"},
      {"an empty data field beside a sparse precision",
       "087010011802200b820706180420042a00",
       "087010011802200b820708180420042a003200"},
      {"an empty sparse data field beside a sparse precision",
       "087010011802200b820706180420043200",
       "087010011802200b820706180420043200"},
      {"a full register array beside a sparse precision",
       "087010011802200b820716180420042a1000000000000000000000000000000000",
       "087010011802200b820716180420042a1000000000000000000000000000000000"},
      {"a full register array and sparse data together",
       "087010011802200b82071b1001180420042a10000000000000000000000000000000003"
       "20105",
       "087010011802200b82071b1001180420042a10000000000000000000000000000000003"
       "20105"},
  };

  for (const auto& round_trip : round_trips) {
    auto sketch = HyperLogLogPlusPlus::FromBytes(DecodeHex(round_trip.parsed));
    ASSERT_TRUE(sketch.has_value()) << round_trip.description;
    auto bytes = sketch.value().Serialize();
    ASSERT_TRUE(bytes.has_value()) << round_trip.description;
    EXPECT_EQ(bytes.value(), DecodeHex(round_trip.written))
        << round_trip.description;
  }
}

// Parsing a sketch and writing it straight back out never reaches an
// operation, and the states below are reached only through one. Each
// row is the reference's own: the sketch it was given, the cardinality
// it reported, and the bytes it then wrote. The cardinality is taken
// before the bytes, as the reference's harness takes it, because
// reporting one flushes a sparse sketch's buffer and so decides what
// is written.
//
// Three of these states exist only because the reference validates so
// little of what it reads. It checks the length of a register array
// and never its contents, so an array of registers at or above 0x80
// reaches an estimator that shifts by a register and a maximum that
// compares registers as signed bytes; the first two rows of that kind
// below would be undefined behaviour under a shift by 255 or a cast of
// an out-of-range double, and the third would keep a register the
// reference lowers. It validates the sparse size against nothing, so a
// size filling every bucket makes the estimate infinite and one beyond
// them makes it not a number, which its rounding reports as the
// extreme value and as zero respectively.
TEST(ErrorHandlingTest, OperationsOnParsedSketchesMatchTheReference) {
  struct Transition {
    const char* description;
    std::string_view parsed;
    int additions;
    const char* prefix;
    int64_t result;
    std::string_view written;
  };
  const std::vector<Transition> transitions = {
      {"empty data field, result", "087010001802200b82070418042a00", 0, "", 0,
       "087010001802200b82070418042a00"},
      {"empty data field, one addition", "087010001802200b82070418042a00", 1,
       "q", 1,
       "087010011802200b82071418042a1000000000000000040000000000000000"},
      {"high registers, result",
       "087010001802200b82071418042a10ffffffffffffffffffffffffffffffff", 0, "",
       std::numeric_limits<int64_t>::min(),
       "087010001802200b82071418042a10ffffffffffffffffffffffffffffffff"},
      {"high registers, one addition",
       "087010001802200b82071418042a10ffffffffffffffffffffffffffffffff", 1, "q",
       2757, "087010011802200b82071418042a10ffffffffffffff04ffffffffffffffff"},
      {"no value type, one addition", "0870100018028207021804", 1, "a", 1,
       "087010011802200b82071418042a1000000000000100000000000000000000"},
      {"sparse size filling every bucket",
       "087010001802200b82070b10808002180a200f320105", 0, "",
       std::numeric_limits<int64_t>::max(),
       "087010001802200b82070b10808002180a200f320105"},
      {"sparse size beyond every bucket",
       "087010001802200b82070b10c0b802180a200f320105", 0, "", 0,
       "087010001802200b82070b10c0b802180a200f320105"},
      {"sparse index beyond the precision",
       "087010001802200b82070b1001180a200f3203feff7f", 5, "v", 6,
       "087010051802200b8207151006180a200f320dd577d90ab818b815eb17f5b77e"},
      {"sparse stream below the threshold",
       "087010001802200b8207890610ff05180a200f32ff05010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "0101010101010101",
       0, "x", 776,
       "087010001802200b8207890610ff05180a200f32ff05010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "0101010101010101"},
      {"sparse stream at the threshold",
       "087010001802200b82078a06108006180a200f328006010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "01010101010101010101010101010101010101010101010101010101010101010101010"
       "10101010101010101010101010101010101010101010101010101010101010101010101"
       "010101010101010101",
       0, "x", 777,
       "087010001802200b82078708180a200f2a8008050606060606060606060606060606060"
       "60606060606060606000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000000000000000"
       "000000000000000000000000000"},
  };

  for (const auto& transition : transitions) {
    auto sketch = HyperLogLogPlusPlus::FromBytes(DecodeHex(transition.parsed));
    ASSERT_TRUE(sketch.has_value()) << transition.description;
    for (int i = 0; i < transition.additions; ++i) {
      ASSERT_TRUE(
          sketch.value().Add(transition.prefix + std::to_string(i)).has_value())
          << transition.description;
    }
    auto result = sketch.value().Result();
    ASSERT_TRUE(result.has_value()) << transition.description;
    EXPECT_EQ(result.value(), transition.result) << transition.description;
    auto bytes = sketch.value().Serialize();
    ASSERT_TRUE(bytes.has_value()) << transition.description;
    EXPECT_EQ(bytes.value(), DecodeHex(transition.written))
        << transition.description;
  }
}

// Merging reaches the same states from a second direction, and the
// reference discards its sparse fields when it normalises rather than
// when it constructs a normal representation, so a merge that has
// nothing to transfer still leaves a full register array behind. Each
// row is the reference's own output for these two operands.
TEST(ErrorHandlingTest, MergingParsedSketchesMatchesTheReference) {
  struct Merge {
    const char* description;
    std::string_view target;
    std::string_view operand;
    int64_t result;
    std::string_view written;
  };
  const std::vector<Merge> merges = {
      {"empty data field merged with itself", "087010001802200b82070418042a00",
       "087010001802200b82070418042a00", 0,
       "087010001802200b82071418042a1000000000000000000000000000000000"},
      {"empty data field merged with registers",
       "087010001802200b82070418042a00",
       "087010001802200b82071418042a1002020202020202020202020202020202", 43,
       "087010001802200b82071418042a1002020202020202020202020202020202"},
      {"registers merged with an empty field",
       "087010001802200b82071418042a1002020202020202020202020202020202",
       "087010001802200b82070418042a00", 43,
       "087010001802200b82071418042a1002020202020202020202020202020202"},
      {"high registers merged with low ones",
       "087010001802200b82071418042a10ffffffffffffffffffffffffffffffff",
       "087010001802200b82071418042a1002020202020202020202020202020202", 43,
       "087010001802200b82071418042a1002020202020202020202020202020202"},
      {"low registers merged with high ones",
       "087010001802200b82071418042a1002020202020202020202020202020202",
       "087010001802200b82071418042a10ffffffffffffffffffffffffffffffff", 43,
       "087010001802200b82071418042a1002020202020202020202020202020202"},
  };

  for (const auto& merge : merges) {
    auto target = HyperLogLogPlusPlus::FromBytes(DecodeHex(merge.target));
    ASSERT_TRUE(target.has_value()) << merge.description;
    auto operand = HyperLogLogPlusPlus::FromBytes(DecodeHex(merge.operand));
    ASSERT_TRUE(operand.has_value()) << merge.description;
    ASSERT_TRUE(target.value().Merge(std::move(operand.value())).has_value())
        << merge.description;
    auto result = target.value().Result();
    ASSERT_TRUE(result.has_value()) << merge.description;
    EXPECT_EQ(result.value(), merge.result) << merge.description;
    auto bytes = target.value().Serialize();
    ASSERT_TRUE(bytes.has_value()) << merge.description;
    EXPECT_EQ(bytes.value(), DecodeHex(merge.written)) << merge.description;
  }
}

// Before the reference looks at a sketch's contents it checks the
// aggregator's own fields, and it reads them with a parser it wrote by
// hand: no field is required, an absent aggregator type means its own,
// and an absent encoding version means the one that preceded this
// library's. A state belonging to another aggregator, or written under
// the earlier encoding, is refused rather than reinterpreted, and a
// value type the aggregator cannot hash is refused by name. Each row
// is the reference's verdict, and for a sketch it reads, the bytes it
// writes back; for one it refuses, its own words.
TEST(ErrorHandlingTest, FromBytesMatchesTheReferenceOnEveryAggregatorField) {
  struct Shape {
    const char* description;
    std::string_view parsed;
    bool accepted;
    std::string_view expectation;
  };
  const std::vector<Shape> shapes = {
      {"aggregator type absent", "10001802200b8207021804", true,
       "087010001802200b8207021804"},
      {"aggregator type 0", "080010001802200b8207021804", false,
       "Expected proto to be of type HYPERLOGLOG_PLUS_UNIQUE but was "
       "AGGREGATOR_TYPE_UNSPECIFIED"},
      {"aggregator type 1", "080110001802200b8207021804", false,
       "Expected proto to be of type HYPERLOGLOG_PLUS_UNIQUE but was null"},
      {"aggregator type 100", "086410001802200b8207021804", false,
       "Expected proto to be of type HYPERLOGLOG_PLUS_UNIQUE but was SUM"},
      {"aggregator type 112", "087010001802200b8207021804", true,
       "087010001802200b8207021804"},
      {"aggregator type 113", "087110001802200b8207021804", false,
       "Expected proto to be of type HYPERLOGLOG_PLUS_UNIQUE but was null"},
      {"aggregator type 200", "08c80110001802200b8207021804", false,
       "Expected proto to be of type HYPERLOGLOG_PLUS_UNIQUE but was null"},
      {"encoding version absent", "08701000200b8207021804", false,
       "Expected encoding version to be 2 but was 1"},
      {"encoding version 0", "087010001800200b8207021804", false,
       "Expected encoding version to be 2 but was 0"},
      {"encoding version 1", "087010001801200b8207021804", false,
       "Expected encoding version to be 2 but was 1"},
      {"encoding version 2", "087010001802200b8207021804", true,
       "087010001802200b8207021804"},
      {"encoding version 3", "087010001803200b8207021804", false,
       "Expected encoding version to be 2 but was 3"},
      {"value type absent", "0870100018028207021804", true,
       "0870100018028207021804"},
      {"value type 0", "08701000180220008207021804", true,
       "0870100018028207021804"},
      {"value type 1", "08701000180220018207021804", false,
       "Unsupported value type DefaultOpsType.Id.INT8"},
      {"value type 4", "08701000180220048207021804", false,
       "Unsupported value type DefaultOpsType.Id.INT64"},
      {"value type 6", "08701000180220068207021804", false,
       "Unsupported value type DefaultOpsType.Id.UINT16"},
      {"value type 7", "08701000180220078207021804", true,
       "08701000180220078207021804"},
      {"value type 8", "08701000180220088207021804", true,
       "08701000180220088207021804"},
      {"value type 9", "08701000180220098207021804", false,
       "Unsupported value type DefaultOpsType.Id.FLOAT"},
      {"value type 10", "087010001802200a8207021804", false,
       "Unsupported value type DefaultOpsType.Id.DOUBLE"},
      {"value type 11", "087010001802200b8207021804", true,
       "087010001802200b8207021804"},
      {"value type 12", "087010001802200c8207021804", false,
       "Unsupported value type <unnamed custom value type 12>"},
      {"value type 1000", "08701000180220e8078207021804", false,
       "Unsupported value type <unnamed custom value type 1000>"},
      {"value count absent", "08701802200b8207021804", true,
       "087010001802200b8207021804"},
      {"value count 0", "087010001802200b8207021804", true,
       "087010001802200b8207021804"},
      {"value count 5", "087010051802200b8207021804", true,
       "087010051802200b8207021804"},
  };

  for (const auto& shape : shapes) {
    auto sketch = HyperLogLogPlusPlus::FromBytes(DecodeHex(shape.parsed));
    ASSERT_EQ(sketch.has_value(), shape.accepted) << shape.description;
    if (!shape.accepted) {
      EXPECT_EQ(sketch.error().message, shape.expectation) << shape.description;
      continue;
    }
    auto bytes = sketch.value().Serialize();
    ASSERT_TRUE(bytes.has_value()) << shape.description;
    EXPECT_EQ(bytes.value(), DecodeHex(shape.expectation)) << shape.description;
  }
}

// A sketch that records no value type takes the type of its first
// addition, which is why one parsed without the field is not written
// back without it. A sketch that records a value type its additions
// cannot satisfy refuses them in the reference's own words.
TEST(ErrorHandlingTest, AddingRecordsTheValueTypeAsTheReferenceDoes) {
  struct Addition {
    const char* description;
    std::string_view parsed;
    bool admitted;
    std::string_view expectation;
  };
  const std::vector<Addition> additions = {
      {"no value type recorded", "0870100018028207021804", true,
       "087010011802200b82071418042a1000000000000000000000010000000000"},
      {"the value type for text recorded", "087010001802200b8207021804", true,
       "087010011802200b82071418042a1000000000000000000000010000000000"},
      {"the value type for 32-bit integers recorded",
       "08701000180220078207021804", false,
       "unable to add type STRING to aggregator of type [INTEGER]"},
      {"the value type for 64-bit integers recorded",
       "08701000180220088207021804", false,
       "unable to add type STRING to aggregator of type [LONG]"},
  };

  for (const auto& addition : additions) {
    auto sketch = HyperLogLogPlusPlus::FromBytes(DecodeHex(addition.parsed));
    ASSERT_TRUE(sketch.has_value()) << addition.description;
    auto added = sketch.value().Add(std::string("0"));
    ASSERT_EQ(added.has_value(), addition.admitted) << addition.description;
    if (!addition.admitted) {
      EXPECT_EQ(added.error().message, addition.expectation)
          << addition.description;
      continue;
    }
    auto bytes = sketch.value().Serialize();
    ASSERT_TRUE(bytes.has_value()) << addition.description;
    EXPECT_EQ(bytes.value(), DecodeHex(addition.expectation))
        << addition.description;
  }
}

// The reference reads the aggregator type from the wire and keeps the
// last field it sees, recognised or not, so a recognised type followed
// by an unrecognised one is refused although the recognised one is
// still in the parsed message. A field of that number carrying anything
// but a variable-length integer is skipped, so a sketch carrying one is
// read as though it declared no type at all. Every verdict and every
// message below is the reference's own.
TEST(ErrorHandlingTest, FromBytesReadsTheAggregatorTypeFromTheWire) {
  struct Shape {
    const char* description;
    std::string_view parsed;
    bool accepted;
    std::string_view expectation;
  };
  const std::vector<Shape> shapes = {
      {"a recognised type followed by an unrecognised one",
       "0870088f4e10001802200b820704180a200f", false,
       "Expected proto to be of type HYPERLOGLOG_PLUS_UNIQUE but was null"},
      {"an unrecognised type followed by a recognised one",
       "088f4e087010001802200b820704180a200f", true,
       "087010001802200b820706180a200f3200"},
      {"a recognised type followed by another recognised one",
       "0870086410001802200b820704180a200f", false,
       "Expected proto to be of type HYPERLOGLOG_PLUS_UNIQUE but was SUM"},
      {"three type fields ending in an unrecognised one",
       "08700864088f4e10001802200b820704180a200f", false,
       "Expected proto to be of type HYPERLOGLOG_PLUS_UNIQUE but was null"},
      {"field one carrying a length-delimited value",
       "0a0010001802200b820704180a200f", true,
       "087010001802200b820706180a200f3200"},
      {"field one carrying a fixed 32-bit value",
       "0d0000000010001802200b820704180a200f", true,
       "087010001802200b820706180a200f3200"},
      {"field one carrying a fixed 64-bit value",
       "09000000000000000010001802200b820704180a200f", true,
       "087010001802200b820706180a200f3200"},
  };

  for (const auto& shape : shapes) {
    auto sketch = HyperLogLogPlusPlus::FromBytes(DecodeHex(shape.parsed));
    ASSERT_EQ(sketch.has_value(), shape.accepted) << shape.description;
    if (!shape.accepted) {
      EXPECT_EQ(sketch.error().message, shape.expectation) << shape.description;
      continue;
    }
    auto bytes = sketch.value().Serialize();
    ASSERT_TRUE(bytes.has_value()) << shape.description;
    EXPECT_EQ(bytes.value(), DecodeHex(shape.expectation)) << shape.description;
  }
}

// A sparse operand merged into a dense one is added value by value, so
// it touches only the registers its values name. Normalising it into a
// register array and taking a maximum over the whole of it would lower
// every other register, the maximum being over signed bytes, and a
// register at or above 0x80 counts as negative there. The targets below
// carry such registers, which the reference accepts because it
// validates a register array's length and never its contents.
TEST(ErrorHandlingTest, MergingSparseIntoDenseTouchesOnlyNamedRegisters) {
  struct Merge {
    const char* description;
    std::string_view target;
    std::string_view operand;
    int64_t result;
    std::string_view written;
  };
  const std::vector<Merge> merges = {
      {"high registers merged with an empty sparse operand",
       "087010001802200b82071418042a10ffffffffffffffffffffffffffffffff",
       "087010001802200b820706180420093200",
       std::numeric_limits<int64_t>::min(),
       "087010001802200b82071418042a10ffffffffffffffffffffffffffffffff"},
      {"high registers merged with a sparse operand",
       "087010001802200b82071418042a10ffffffffffffffffffffffffffffffff",
       "087010001802200b820709100118042009320105", 1379,
       "087010001802200b82071418042a1003ffffffffffffffffffffffffffffff"},
      {"mixed registers merged with a sparse operand",
       "087010001802200b82071418042a1000017f80ff02030405060708090a0b0c",
       "087010001802200b820709100118042009320105", 81,
       "087010001802200b82071418042a1003017f80ff02030405060708090a0b0c"},
  };

  for (const auto& merge : merges) {
    auto target = HyperLogLogPlusPlus::FromBytes(DecodeHex(merge.target));
    ASSERT_TRUE(target.has_value()) << merge.description;
    auto operand = HyperLogLogPlusPlus::FromBytes(DecodeHex(merge.operand));
    ASSERT_TRUE(operand.has_value()) << merge.description;
    ASSERT_TRUE(target.value().Merge(std::move(operand.value())).has_value())
        << merge.description;
    auto result = target.value().Result();
    ASSERT_TRUE(result.has_value()) << merge.description;
    EXPECT_EQ(result.value(), merge.result) << merge.description;
    auto bytes = target.value().Serialize();
    ASSERT_TRUE(bytes.has_value()) << merge.description;
    EXPECT_EQ(bytes.value(), DecodeHex(merge.written)) << merge.description;
  }
}

// Serialization compacts the sketch itself, so a sparse sketch that
// compaction promotes is estimated as a dense one afterwards. The three
// populations below sit either side of that promotion: at 200 nothing
// is promoted and the two orders agree, at 760 serializing first
// promotes and changes the estimate, and at 800 the sketch has already
// promoted before either call.
TEST(ErrorHandlingTest, SerializingPromotesTheSketchAsTheReferenceDoes) {
  struct Case {
    int additions;
    int64_t after_serializing;
    int64_t without_serializing;
  };
  const std::vector<Case> cases = {
      {.additions = 200, .after_serializing = 201, .without_serializing = 201},
      {.additions = 760, .after_serializing = 761, .without_serializing = 764},
      {.additions = 800, .after_serializing = 800, .without_serializing = 800},
  };
  constexpr std::string_view kEmptySparseSketch =
      "087010001802200b820706180a200f3200";

  for (const auto& test_case : cases) {
    const auto fill = [&test_case](HyperLogLogPlusPlus& sketch) {
      for (int i = 0; i < test_case.additions; ++i) {
        ASSERT_TRUE(sketch.Add("v" + std::to_string(i)).has_value());
      }
    };

    auto serialized_first =
        HyperLogLogPlusPlus::FromBytes(DecodeHex(kEmptySparseSketch));
    ASSERT_TRUE(serialized_first.has_value());
    fill(serialized_first.value());
    ASSERT_TRUE(serialized_first.value().Serialize().has_value());
    auto after = serialized_first.value().Result();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after.value(), test_case.after_serializing)
        << "additions " << test_case.additions;

    auto estimated_only =
        HyperLogLogPlusPlus::FromBytes(DecodeHex(kEmptySparseSketch));
    ASSERT_TRUE(estimated_only.has_value());
    fill(estimated_only.value());
    auto without = estimated_only.value().Result();
    ASSERT_TRUE(without.has_value());
    EXPECT_EQ(without.value(), test_case.without_serializing)
        << "additions " << test_case.additions;
  }
}

// Which additions a sketch admits is decided by the value type it
// records, and needs no hash, so it is reproduced for both additions
// this library offers. The product below is every value type the
// reference accepts, including the field being absent, against both
// additions. Only admission and the refusal's words are asserted for an
// integer addition; the bytes an integer addition produces are
// compared against the reference by the golden rows and by the integer
// comparison in the differential suite. Every
// verdict and message is the reference's own.
TEST(ErrorHandlingTest, AdditionsAreAdmittedByTheRecordedValueType) {
  struct Admission {
    const char* description;
    std::string_view parsed;
    bool admits_text;
    std::string_view text_message;
    bool admits_integers;
    std::string_view integer_message;
  };
  const std::vector<Admission> admissions = {
      {"no value type recorded", "0870100018028207021804", true, "", true, ""},
      {"the unknown value type recorded", "08701000180220008207021804", true,
       "", true, ""},
      {"the value type for 32-bit integers recorded",
       "08701000180220078207021804", false,
       "unable to add type STRING to aggregator of type [INTEGER]", false,
       "unable to add type LONG to aggregator of type [INTEGER]"},
      {"the value type for 64-bit integers recorded",
       "08701000180220088207021804", false,
       "unable to add type STRING to aggregator of type [LONG]", true, ""},
      {"the value type for text recorded", "087010001802200b8207021804", true,
       "", false,
       "unable to add type LONG to aggregator of type [STRING, BYTES]"},
  };

  for (const auto& admission : admissions) {
    auto text = HyperLogLogPlusPlus::FromBytes(DecodeHex(admission.parsed));
    ASSERT_TRUE(text.has_value()) << admission.description;
    auto added_text = text.value().Add(std::string("a"));
    ASSERT_EQ(added_text.has_value(), admission.admits_text)
        << admission.description;
    if (!admission.admits_text) {
      EXPECT_EQ(added_text.error().message, admission.text_message)
          << admission.description;
    }

    auto integers = HyperLogLogPlusPlus::FromBytes(DecodeHex(admission.parsed));
    ASSERT_TRUE(integers.has_value()) << admission.description;
    auto added_integer = integers.value().Add(int64_t{7});
    ASSERT_EQ(added_integer.has_value(), admission.admits_integers)
        << admission.description;
    if (!admission.admits_integers) {
      EXPECT_EQ(added_integer.error().message, admission.integer_message)
          << admission.description;
    }
  }
}

// A sketch built for integers records the value type the reference
// records for one, and writes it even when nothing has been added, so
// the empty sketch alone distinguishes the two constructions. Every
// pair below is the reference's own output for the two builders at the
// same configuration.
TEST(ErrorHandlingTest, ConstructionRecordsTheValueTypeAsTheReferenceDoes) {
  struct Shape {
    int32_t precision;
    int32_t sparse_precision;
    std::string_view for_integers;
    std::string_view for_text;
  };
  const std::vector<Shape> shapes = {
      {15, 20, "0870100018022008820706180f20143200",
       "087010001802200b820706180f20143200"},
      {10, 15, "0870100018022008820706180a200f3200",
       "087010001802200b820706180a200f3200"},
      {15, 0, "0870100018022008820702180f", "087010001802200b820702180f"},
      {10, 0, "0870100018022008820702180a", "087010001802200b820702180a"},
      {4, 9, "0870100018022008820706180420093200",
       "087010001802200b820706180420093200"},
      {24, 25, "0870100018022008820706181820193200",
       "087010001802200b820706181820193200"},
  };

  for (const auto& shape : shapes) {
    const std::string context = std::format(
        "precision {} sparse {}", shape.precision, shape.sparse_precision);

    auto integers =
        HyperLogLogPlusPlus::Create(shape.precision, shape.sparse_precision,
                                    zetasketch::hll::ValueType::kUnsignedInt64);
    ASSERT_TRUE(integers.has_value()) << context;
    auto integer_bytes = integers.value().Serialize();
    ASSERT_TRUE(integer_bytes.has_value()) << context;
    EXPECT_EQ(integer_bytes.value(), DecodeHex(shape.for_integers)) << context;

    auto text =
        HyperLogLogPlusPlus::Create(shape.precision, shape.sparse_precision);
    ASSERT_TRUE(text.has_value()) << context;
    auto text_bytes = text.value().Serialize();
    ASSERT_TRUE(text_bytes.has_value()) << context;
    EXPECT_EQ(text_bytes.value(), DecodeHex(shape.for_text)) << context;
  }
}

// The set of additions a sketch admits narrows to one on its first
// addition, and the refusal names the narrowed set. That narrowing
// lives beside the sketch rather than in its bytes, so it cannot be
// seen by comparing what a sketch writes; the message is the only place
// it shows. Every message here is the reference's own.
TEST(ErrorHandlingTest, TheAdmittedSetNarrowsOnTheFirstAddition) {
  struct Case {
    const char* description;
    zetasketch::hll::ValueType constructed_with;
    bool add_text_first;
    std::string_view message;
  };
  const std::vector<Case> cases = {
      {"text first, then an integer", zetasketch::hll::ValueType::kUnknown,
       true, "unable to add type LONG to aggregator of type [STRING]"},
      {"an integer first, then text", zetasketch::hll::ValueType::kUnknown,
       false, "unable to add type STRING to aggregator of type [LONG]"},
      {"built for text, text first, then an integer",
       zetasketch::hll::ValueType::kBytesOrUtf8String, true,
       "unable to add type LONG to aggregator of type [STRING]"},
      {"built for integers, an integer first, then text",
       zetasketch::hll::ValueType::kUnsignedInt64, false,
       "unable to add type STRING to aggregator of type [LONG]"},
  };

  for (const auto& test_case : cases) {
    auto sketch =
        HyperLogLogPlusPlus::Create(10, 15, test_case.constructed_with);
    ASSERT_TRUE(sketch.has_value()) << test_case.description;
    if (test_case.add_text_first) {
      ASSERT_TRUE(sketch.value().Add(std::string("a")).has_value())
          << test_case.description;
    } else {
      ASSERT_TRUE(sketch.value().Add(int64_t{7}).has_value())
          << test_case.description;
    }

    auto refused = test_case.add_text_first
                       ? sketch.value().Add(int64_t{7})
                       : sketch.value().Add(std::string("a"));
    ASSERT_FALSE(refused.has_value()) << test_case.description;
    EXPECT_EQ(refused.error().code,
              zetasketch::utils::ErrorCode::kIllegalArgument)
        << test_case.description;
    EXPECT_EQ(refused.error().message, test_case.message)
        << test_case.description;
  }
}

// Which additions a constructed sketch admits before anything has been
// added, over every value type it can be constructed with against both
// additions this library offers.
// The reference fixes the type when it builds and refuses an addition
// of another type; a sketch constructed without a type takes the type
// of its first addition, as a parsed one does. Nothing here throws: a
// refused addition is an error value, because this library is linked
// into a server that must not fault on what a caller passes it.
TEST(ErrorHandlingTest, ConstructedSketchesAdmitAdditionsByValueType) {
  struct Cell {
    const char* description;
    zetasketch::hll::ValueType constructed_with;
    bool admits_text;
    std::string_view text_message;
    bool admits_integers;
    std::string_view integer_message;
  };
  const std::vector<Cell> cells = {
      {"constructed without a value type", zetasketch::hll::ValueType::kUnknown,
       true, "", true, ""},
      {"constructed for text", zetasketch::hll::ValueType::kBytesOrUtf8String,
       true, "", false,
       "unable to add type LONG to aggregator of type [STRING, BYTES]"},
      {"constructed for 64-bit integers",
       zetasketch::hll::ValueType::kUnsignedInt64, false,
       "unable to add type STRING to aggregator of type [LONG]", true, ""},
      {"constructed for 32-bit integers",
       zetasketch::hll::ValueType::kUnsignedInt32, false,
       "unable to add type STRING to aggregator of type [INTEGER]", false,
       "unable to add type LONG to aggregator of type [INTEGER]"},
  };

  for (const auto& cell : cells) {
    auto text = HyperLogLogPlusPlus::Create(10, 15, cell.constructed_with);
    ASSERT_TRUE(text.has_value()) << cell.description;
    auto added_text = text.value().Add(std::string("a"));
    ASSERT_EQ(added_text.has_value(), cell.admits_text) << cell.description;
    if (!cell.admits_text) {
      EXPECT_EQ(added_text.error().code,
                zetasketch::utils::ErrorCode::kIllegalArgument)
          << cell.description;
      EXPECT_EQ(added_text.error().message, cell.text_message)
          << cell.description;
    }

    auto integers = HyperLogLogPlusPlus::Create(10, 15, cell.constructed_with);
    ASSERT_TRUE(integers.has_value()) << cell.description;
    auto added_integer = integers.value().Add(int64_t{7});
    ASSERT_EQ(added_integer.has_value(), cell.admits_integers)
        << cell.description;
    if (!cell.admits_integers) {
      EXPECT_EQ(added_integer.error().code,
                zetasketch::utils::ErrorCode::kIllegalArgument)
          << cell.description;
      EXPECT_EQ(added_integer.error().message, cell.integer_message)
          << cell.description;
    }
  }
}

// A value type the reference will not read cannot be constructed
// either, in the reference's own words, so a sketch cannot be built
// that it would refuse to parse back.
TEST(ErrorHandlingTest, ConstructionRefusesValueTypesTheReferenceRefuses) {
  struct Refusal {
    int32_t value_type;
    std::string_view message;
  };
  const std::vector<Refusal> refusals = {
      {1, "Unsupported value type DefaultOpsType.Id.INT8"},
      {2, "Unsupported value type DefaultOpsType.Id.INT16"},
      {3, "Unsupported value type DefaultOpsType.Id.INT32"},
      {4, "Unsupported value type DefaultOpsType.Id.INT64"},
      {5, "Unsupported value type DefaultOpsType.Id.UINT8"},
      {6, "Unsupported value type DefaultOpsType.Id.UINT16"},
      {9, "Unsupported value type DefaultOpsType.Id.FLOAT"},
      {10, "Unsupported value type DefaultOpsType.Id.DOUBLE"},
      {12, "Unsupported value type <unnamed custom value type 12>"},
      {1000, "Unsupported value type <unnamed custom value type 1000>"},
  };

  for (const auto& refusal : refusals) {
    auto sketch = HyperLogLogPlusPlus::Create(
        10, 15, static_cast<zetasketch::hll::ValueType>(refusal.value_type));
    ASSERT_FALSE(sketch.has_value()) << refusal.value_type;
    EXPECT_EQ(sketch.error().code,
              zetasketch::utils::ErrorCode::kIllegalArgument)
        << refusal.value_type;
    EXPECT_EQ(sketch.error().message, refusal.message) << refusal.value_type;
  }
}

// An integer addition survives a write and a read: the sketch it
// produces parses back, keeps its recorded type, and accepts further
// integers, which is the sequence a caller performs across a
// serialization boundary.
TEST(ErrorHandlingTest, IntegerAdditionsSurviveAWriteAndARead) {
  auto sketch = HyperLogLogPlusPlus::Create(
      10, 15, zetasketch::hll::ValueType::kUnsignedInt64);
  ASSERT_TRUE(sketch.has_value());
  for (int64_t value = 0; value < 40; ++value) {
    ASSERT_TRUE(sketch.value().Add(value).has_value()) << value;
  }
  auto written = sketch.value().Serialize();
  ASSERT_TRUE(written.has_value());

  auto reread = HyperLogLogPlusPlus::FromBytes(written.value());
  ASSERT_TRUE(reread.has_value());
  EXPECT_FALSE(reread.value().Add(std::string("a")).has_value());
  for (int64_t value = 40; value < 80; ++value) {
    ASSERT_TRUE(reread.value().Add(value).has_value()) << value;
  }

  auto one_pass = HyperLogLogPlusPlus::Create(
      10, 15, zetasketch::hll::ValueType::kUnsignedInt64);
  ASSERT_TRUE(one_pass.has_value());
  for (int64_t value = 0; value < 80; ++value) {
    ASSERT_TRUE(one_pass.value().Add(value).has_value()) << value;
  }
  auto resumed_bytes = reread.value().Serialize();
  auto one_pass_bytes = one_pass.value().Serialize();
  ASSERT_TRUE(resumed_bytes.has_value());
  ASSERT_TRUE(one_pass_bytes.has_value());
  EXPECT_EQ(resumed_bytes.value(), one_pass_bytes.value());
}

// A sketch that records no value type takes the type of its first
// addition, and an integer addition records the type the reference
// records for integers. This is the path the reference's own reader
// produces: a sketch read from bytes that carry no value type, given
// integers, must reach the sketch the reference's integer builder
// reaches from the same integers. Every expectation is the reference's
// own output.
TEST(ErrorHandlingTest, IntegerAdditionsRecordTheValueTypeOnAnUntypedSketch) {
  struct Case {
    int32_t precision;
    int32_t sparse_precision;
    int additions;
    std::string_view written;
  };
  const std::vector<Case> cases = {
      {10, 15, 1, "087010011802200882070b1001180a200f32039d8501"},
      {10, 15, 10,
       "0870100a1802200882071c100a180a200f32149915c60af8039a60ac01a71bec14dc03"
       "a623a903"},
      {4, 0, 1,
       "087010011802200882071418042a1000000000000000000200000000000000"},
  };

  for (const auto& test_case : cases) {
    const std::string context =
        std::format("precision {} sparse {} additions {}", test_case.precision,
                    test_case.sparse_precision, test_case.additions);

    // Built with no value type, as a sketch read from bytes without one
    // is, and given integers.
    auto untyped = HyperLogLogPlusPlus::Create(
        test_case.precision, test_case.sparse_precision,
        zetasketch::hll::ValueType::kUnknown);
    ASSERT_TRUE(untyped.has_value()) << context;
    for (int64_t value = 0; value < test_case.additions; ++value) {
      ASSERT_TRUE(untyped.value().Add(value).has_value()) << context;
    }
    auto from_untyped = untyped.value().Serialize();
    ASSERT_TRUE(from_untyped.has_value()) << context;

    // Built for integers from the start, which must reach the same
    // sketch once the first addition has recorded the type.
    auto typed = HyperLogLogPlusPlus::Create(
        test_case.precision, test_case.sparse_precision,
        zetasketch::hll::ValueType::kUnsignedInt64);
    ASSERT_TRUE(typed.has_value()) << context;
    for (int64_t value = 0; value < test_case.additions; ++value) {
      ASSERT_TRUE(typed.value().Add(value).has_value()) << context;
    }
    auto from_typed = typed.value().Serialize();
    ASSERT_TRUE(from_typed.has_value()) << context;

    // Both must be the reference's own sketch, not merely each other's.
    EXPECT_EQ(from_untyped.value(), DecodeHex(test_case.written)) << context;
    EXPECT_EQ(from_typed.value(), DecodeHex(test_case.written)) << context;
  }
}

// Sparse data without a sparse precision is refused before any
// precision is examined, in the reference's own words.
TEST(ErrorHandlingTest, FromBytesRefusesSparseDataWithNoSparsePrecision) {
  State state;
  state.encoding_version = 2;
  state.precision = 15;
  state.sparse_size = 1;
  state.sparse_data = std::vector<uint8_t>{0x05};

  auto bytes = state.ToByteArray();
  ASSERT_TRUE(bytes.has_value());
  auto sketch = HyperLogLogPlusPlus::FromBytes(bytes.value());
  ASSERT_FALSE(sketch.has_value());
  EXPECT_EQ(sketch.error().message,
            "Must have a sparse precision when sparse data is set");
}

TEST(ErrorHandlingTest, FromBytesRefusesPrecisionsOutsideTheAcceptedRange) {
  struct RefusedShape {
    const char* description;
    int32_t precision;
    int32_t sparse_precision;
    bool with_sparse_data;
  };
  const std::vector<RefusedShape> shapes = {
      {"normal precision below the minimum", 3, 0, false},
      {"normal precision above the maximum", 25, 0, false},
      {"sparse precision below the normal precision", 15, 14, true},
      {"sparse precision above the maximum", 15, 26, true},
      {"sparse data with no sparse precision", 15, 0, true},
  };

  for (const auto& shape : shapes) {
    State state;
    state.encoding_version = 2;
    state.precision = shape.precision;
    state.sparse_precision = shape.sparse_precision;
    if (shape.with_sparse_data) {
      state.sparse_size = 1;
      state.sparse_data = std::vector<uint8_t>{0x02};
    }
    auto bytes = state.ToByteArray();
    ASSERT_TRUE(bytes.has_value()) << shape.description;

    auto sketch = HyperLogLogPlusPlus::FromBytes(bytes.value());
    ASSERT_FALSE(sketch.has_value()) << shape.description;
    EXPECT_EQ(sketch.error().code,
              zetasketch::utils::ErrorCode::kIllegalArgument)
        << shape.description;
  }
}

}  // namespace
// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
