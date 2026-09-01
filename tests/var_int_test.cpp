// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/utils/var_int.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <gtest/gtest.h>
#include "zetasketch/utils/error.h"

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

namespace {

using zetasketch::utils::ErrorCode;
using zetasketch::utils::VarInt;

struct LengthCase {
  int32_t value;
  size_t length;
};

// The smallest and largest value at every encoded length, plus -1,
// whose two's-complement bit pattern is the largest unsigned value.
const std::vector<LengthCase>& LengthCases() {
  static const std::vector<LengthCase> kCases = {
      {0, 1},         {1, 1},         {127, 1},        {128, 2},
      {16383, 2},     {16384, 3},     {2097151, 3},    {2097152, 4},
      {268435455, 4}, {268435456, 5}, {2147483647, 5}, {-1, 5},
  };
  return kCases;
}

TEST(VarIntTest, SizeMatchesEncodedLengthAtEveryBoundary) {
  for (const auto& c : LengthCases()) {
    EXPECT_EQ(VarInt::Size(c.value), c.length) << "value " << c.value;
  }
}

TEST(VarIntTest, RoundTripAtEveryBoundary) {
  for (const auto& c : LengthCases()) {
    std::vector<uint8_t> buf(VarInt::Size(c.value));
    EXPECT_EQ(VarInt::Set(c.value, buf), c.length) << "value " << c.value;
    auto decoded = VarInt::Get(buf);
    ASSERT_TRUE(decoded.has_value()) << "value " << c.value;
    EXPECT_EQ(decoded->value, c.value);
    EXPECT_EQ(decoded->bytes_read, c.length);
  }
}

TEST(VarIntTest, GetIsUsableInConstantEvaluation) {
  static constexpr std::array<uint8_t, 2> kEncoded = {0x96, 0x01};
  static_assert(VarInt::Get(kEncoded).has_value());
  static_assert(VarInt::Get(kEncoded)->value == 150);
  static_assert(VarInt::Get(kEncoded)->bytes_read == 2);
  // The failure path constructs an Error holding a std::string, which
  // constant evaluation permits only transiently; pin that it stays
  // usable there.
  static constexpr std::array<uint8_t, 1> kTruncated = {0x80};
  static_assert(!VarInt::Get(kTruncated).has_value());
}

TEST(VarIntTest, GetStopsAtTheFinalByteOfTheEncoding) {
  // The value 150 occupies two bytes; the bytes after it are not read.
  const std::vector<uint8_t> buf = {0x96, 0x01, 0xFF, 0xFF};
  auto decoded = VarInt::Get(buf);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->value, 150);
  EXPECT_EQ(decoded->bytes_read, 2U);
}

TEST(VarIntTest, GetMaximumUnsignedValue) {
  const std::vector<uint8_t> buf = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
  auto decoded = VarInt::Get(buf);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->value, -1);
  EXPECT_EQ(decoded->bytes_read, 5U);
}

TEST(VarIntTest, GetFiveByteEncodingDiscardsBitsAboveThirtyTwo) {
  // The final byte contributes only its low four value bits; the rest
  // fall outside a 32-bit value and are discarded.
  const std::vector<uint8_t> buf = {0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
  auto decoded = VarInt::Get(buf);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->value, -1);
  EXPECT_EQ(decoded->bytes_read, 5U);
}

TEST(VarIntTest, GetOnEmptySpanFails) {
  auto decoded = VarInt::Get(std::span<const uint8_t>());
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().code, ErrorCode::kInvalidState);
}

TEST(VarIntTest, GetTruncatedEncodingFails) {
  // Every byte carries the continuation bit, and the span ends before
  // the encoding does.
  const std::vector<std::vector<uint8_t>> cases = {
      {0x80},
      {0xFF, 0xFF},
      {0x80, 0x80, 0x80},
      {0xFF, 0xFF, 0xFF, 0xFF},
  };
  for (const auto& bytes : cases) {
    auto decoded = VarInt::Get(bytes);
    ASSERT_FALSE(decoded.has_value()) << "length " << bytes.size();
    EXPECT_EQ(decoded.error().code, ErrorCode::kInvalidState);
  }
}

TEST(VarIntTest, GetOverlongEncodingFails) {
  // A fifth byte with its continuation bit set can never complete a
  // 32-bit value, however much data follows.
  const std::vector<std::vector<uint8_t>> cases = {
      {0x80, 0x80, 0x80, 0x80, 0x80},
      {0x80, 0x80, 0x80, 0x80, 0x80, 0x01},
      {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01},
  };
  for (const auto& bytes : cases) {
    auto decoded = VarInt::Get(bytes);
    ASSERT_FALSE(decoded.has_value()) << "length " << bytes.size();
    EXPECT_EQ(decoded.error().code, ErrorCode::kInvalidState);
  }
}

}  // namespace

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
