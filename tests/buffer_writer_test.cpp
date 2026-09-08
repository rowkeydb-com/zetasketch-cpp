#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "zetasketch/utils/buffer_traits.h"
#include "zetasketch/utils/error.h"

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

namespace {

using zetasketch::utils::BufferWriter;

// The writer may point at its own storage, so a copy or a move would
// leave the new object pointing into the old one. Neither exists.
static_assert(!std::is_copy_constructible_v<BufferWriter>);
static_assert(!std::is_copy_assignable_v<BufferWriter>);
static_assert(!std::is_move_constructible_v<BufferWriter>);
static_assert(!std::is_move_assignable_v<BufferWriter>);

TEST(BufferWriterTest, WritesIntoItsOwnStorageWhenGivenNone) {
  BufferWriter writer;
  EXPECT_EQ(writer.size(), 0U);
  EXPECT_TRUE(writer.empty());
  writer.WriteVarInt(300);
  EXPECT_EQ(writer.GetBuffer(), (std::vector<uint8_t>{0xac, 0x02}));
  EXPECT_EQ(writer.Consume(), (std::vector<uint8_t>{0xac, 0x02}));
}

TEST(BufferWriterTest, WritesIntoABorrowedVector) {
  std::vector<uint8_t> borrowed = {1};
  BufferWriter writer(borrowed);
  writer.WriteVarInt(5);
  EXPECT_EQ(borrowed, (std::vector<uint8_t>{1, 5}));
  EXPECT_EQ(writer.size(), 2U);
}

TEST(BufferWriterTest, AdoptsAndClearsAVectorItIsGiven) {
  std::vector<uint8_t> given = {9, 9, 9};
  given.reserve(64);
  const uint8_t* storage = given.data();
  BufferWriter writer(std::move(given));
  EXPECT_TRUE(writer.empty());
  writer.WriteVarInt(7);
  const std::vector<uint8_t> out = writer.Consume();
  EXPECT_EQ(out, (std::vector<uint8_t>{7}));
  EXPECT_EQ(out.data(), storage);
}

TEST(BufferWriterTest, WriteMaxRefusesAnIndexPastTheEnd) {
  BufferWriter writer;
  writer.WriteVarInt(1);
  EXPECT_TRUE(writer.WriteMax(0, 3).has_value());
  EXPECT_EQ(writer.GetBuffer(), (std::vector<uint8_t>{3}));
  EXPECT_TRUE(writer.WriteMax(0, 2).has_value());
  EXPECT_EQ(writer.GetBuffer(), (std::vector<uint8_t>{3}));
  auto refused = writer.WriteMax(1, 1);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, zetasketch::utils::ErrorCode::kInvalidState);
}

}  // namespace

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
