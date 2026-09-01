// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/utils/iterators.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "zetasketch/utils/error.h"
#include "zetasketch/utils/var_int.h"

namespace zetasketch::utils {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers,misc-redundant-expression)

std::vector<uint8_t> EncodeVarInts(const std::vector<int32_t>& values) {
  std::vector<uint8_t> buf;
  for (const int32_t val : values) {
    const size_t size = VarInt::Size(val);
    const size_t pos = buf.size();
    buf.resize(buf.size() + size);
    VarInt::Set(val, std::span<uint8_t>(buf).subspan(pos));
  }
  return buf;
}

TEST(DifferenceDecoderTest, EmptyReturnsNullopt) {
  std::vector<uint8_t> empty_data;
  DifferenceDecoder decoder(empty_data);
  EXPECT_FALSE(decoder.Next().has_value());
  // An empty span is a clean end, not a failure.
  EXPECT_FALSE(decoder.error().has_value());
}

TEST(DifferenceDecoderTest, DecodesIntegers) {
  auto values = EncodeVarInts({42, 170 - 42, 2903 - 170, 20160531 - 2903});
  DifferenceDecoder decoder(values);

  EXPECT_EQ(decoder.Next().value_or(0), 42);
  EXPECT_EQ(decoder.Next().value_or(0), 170);
  EXPECT_EQ(decoder.Next().value_or(0), 2903);
  EXPECT_EQ(decoder.Next().value_or(0), 20160531);
  EXPECT_FALSE(decoder.Next().has_value());
}

TEST(DifferenceDecoderTest, CleanEndLeavesNoError) {
  auto values = EncodeVarInts({42, 170 - 42});
  DifferenceDecoder decoder(values);
  while (decoder.Next().has_value()) {
  }
  EXPECT_FALSE(decoder.error().has_value());
}

TEST(DifferenceDecoderTest, RecordsDecodeFailure) {
  // One complete varint, then a final byte with its continuation bit
  // set. Iteration ends as it would at a clean end of data, and the
  // failure is recorded for the caller to inspect.
  std::vector<uint8_t> data = EncodeVarInts({42});
  data.push_back(0x80);
  DifferenceDecoder decoder(data);

  EXPECT_EQ(decoder.Next().value_or(0), 42);
  EXPECT_FALSE(decoder.Next().has_value());
  ASSERT_TRUE(decoder.error().has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(decoder.error()->code, ErrorCode::kInvalidState);
  // The failure persists across further calls.
  EXPECT_FALSE(decoder.Next().has_value());
  ASSERT_TRUE(decoder.error().has_value());
}

TEST(DifferenceEncoderTest, WritesEqualElements) {
  DifferenceEncoder encoder;
  EXPECT_TRUE(encoder.PutInt(42).has_value());
  EXPECT_TRUE(encoder.PutInt(42).has_value());

  const auto expected = EncodeVarInts({42, 42 - 42});
  EXPECT_EQ(std::move(encoder).IntoVec(), expected);
}

TEST(DifferenceEncoderTest, WritesMultiple) {
  DifferenceEncoder encoder;
  EXPECT_TRUE(encoder.PutInt(42).has_value());
  EXPECT_TRUE(encoder.PutInt(170).has_value());
  EXPECT_TRUE(encoder.PutInt(2903).has_value());

  const auto expected = EncodeVarInts({42, 170 - 42, 2903 - 170});
  EXPECT_EQ(std::move(encoder).IntoVec(), expected);
}

TEST(DifferenceEncoderTest, WritesSingle) {
  DifferenceEncoder encoder;
  EXPECT_TRUE(encoder.PutInt(42).has_value());

  const auto expected = EncodeVarInts({42});
  EXPECT_EQ(std::move(encoder).IntoVec(), expected);
}

TEST(DifferenceEncoderTest, ThrowsWhenNegative) {
  DifferenceEncoder encoder;
  auto result = encoder.PutInt(-1);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::kIllegalArgument);
}

TEST(DifferenceEncoderTest, ThrowsWhenNotSorted) {
  DifferenceEncoder encoder;
  EXPECT_TRUE(encoder.PutInt(42).has_value());

  auto result = encoder.PutInt(12);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::kIllegalArgument);
}

TEST(MergedIntIteratorTest, ReturnsValuesInSortedOrder) {
  std::vector<uint32_t> data_a = {1, 2, 4};
  std::vector<uint32_t> data_b = {2, 3, 4, 5, 6, 7};

  MergedIntIterator<std::vector<uint32_t>::const_iterator,
                    std::vector<uint32_t>::const_iterator>
      iter(data_a.begin(), data_a.end(), data_b.begin(), data_b.end());

  std::vector<uint32_t> result;
  while (auto val = iter.Next()) {
    result.push_back(val.value());
  }

  const std::vector<uint32_t> expected = {1, 2, 2, 3, 4, 4, 5, 6, 7};
  EXPECT_EQ(result, expected);
  // Exhaustion is stable: querying again yields nothing.
  EXPECT_FALSE(iter.Next().has_value());
}

TEST(MergedIntIteratorTest, PreservesDuplicatesWithinOneSide) {
  const std::vector<uint32_t> data_a = {2, 2, 3};
  const std::vector<uint32_t> data_b = {2, 4};

  MergedIntIterator<std::vector<uint32_t>::const_iterator,
                    std::vector<uint32_t>::const_iterator>
      iter(data_a.begin(), data_a.end(), data_b.begin(), data_b.end());

  std::vector<uint32_t> result;
  while (auto val = iter.Next()) {
    result.push_back(val.value());
  }
  const std::vector<uint32_t> expected = {2, 2, 2, 3, 4};
  EXPECT_EQ(result, expected);
}

TEST(MergedIntIteratorTest, YieldsTheOtherSideWhenOneSideIsEmpty) {
  const std::vector<uint32_t> empty;
  const std::vector<uint32_t> data = {3, 5, 8};

  MergedIntIterator<std::vector<uint32_t>::const_iterator,
                    std::vector<uint32_t>::const_iterator>
      left_empty(empty.begin(), empty.end(), data.begin(), data.end());
  std::vector<uint32_t> from_right;
  while (auto val = left_empty.Next()) {
    from_right.push_back(val.value());
  }
  EXPECT_EQ(from_right, data);

  MergedIntIterator<std::vector<uint32_t>::const_iterator,
                    std::vector<uint32_t>::const_iterator>
      right_empty(data.begin(), data.end(), empty.begin(), empty.end());
  std::vector<uint32_t> from_left;
  while (auto val = right_empty.Next()) {
    from_left.push_back(val.value());
  }
  EXPECT_EQ(from_left, data);
}

TEST(MergedIntIteratorTest, EndsImmediatelyWhenBothSidesAreEmpty) {
  const std::vector<uint32_t> empty;
  MergedIntIterator<std::vector<uint32_t>::const_iterator,
                    std::vector<uint32_t>::const_iterator>
      iter(empty.begin(), empty.end(), empty.begin(), empty.end());
  EXPECT_FALSE(iter.Next().has_value());
  EXPECT_FALSE(iter.Next().has_value());
}

}  // namespace
// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers,misc-redundant-expression)
}  // namespace zetasketch::utils
