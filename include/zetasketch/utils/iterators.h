// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_UTILS_ITERATORS_H_
#define ZETASKETCH_UTILS_ITERATORS_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "zetasketch/utils/buffer_traits.h"

namespace zetasketch::utils {

class DifferenceDecoder {
 public:
  explicit DifferenceDecoder(std::span<const uint8_t> data) : reader_(data) {}

  // Returns the next decoded value, or std::nullopt if at the end of the span.
  std::optional<uint32_t> Next() {
    if (reader_.HasRemaining()) {
      auto result = reader_.ReadVarInt();
      if (!result.has_value()) {
        return std::nullopt;
      }
      last_ = last_ + static_cast<uint32_t>(result.value());
      return last_;
    }
    return std::nullopt;
  }

 private:
  BufferReader reader_;
  uint32_t last_{0};
};

class DifferenceDecoderIterator {
 public:
  using iterator_category = std::input_iterator_tag;
  using value_type = uint32_t;
  using difference_type = std::ptrdiff_t;
  using pointer = const uint32_t*;
  using reference = const uint32_t&;

  DifferenceDecoderIterator() = default;
  explicit DifferenceDecoderIterator(DifferenceDecoder* decoder)
      : decoder_(decoder) {
    Advance();
  }

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  uint32_t operator*() const { return current_.value(); }
  DifferenceDecoderIterator& operator++() {
    Advance();
    return *this;
  }
  DifferenceDecoderIterator operator++(int) {
    DifferenceDecoderIterator tmp = *this;
    ++(*this);
    return tmp;
  }
  bool operator==(const DifferenceDecoderIterator& other) const {
    return current_.has_value() == other.current_.has_value();
  }
  bool operator!=(const DifferenceDecoderIterator& other) const {
    return !(*this == other);
  }

 private:
  void Advance() {
    if (decoder_) {
      current_ = decoder_->Next();
    } else {
      current_ = std::nullopt;
    }
  }
  DifferenceDecoder* decoder_ = nullptr;
  std::optional<uint32_t> current_;
};

class DifferenceEncoder {
 public:
  DifferenceEncoder() : last_(std::nullopt) {}
  explicit DifferenceEncoder(std::vector<uint8_t>&& existing)
      : writer_(std::move(existing)), last_(std::nullopt) {}

  std::expected<void, Error> PutInt(int32_t val) {
    if (val < 0) {
      return std::unexpected(
          Error{.code = ErrorCode::kIllegalArgument,
                .message = "Only positive integers are supported"});
    }
    if (last_.has_value() && val < last_.value()) {
      return std::unexpected(
          Error{.code = ErrorCode::kIllegalArgument,
                .message = std::format("{} put after {} but values are "
                                       "required to be in increasing order",
                                       val, last_.value())});
    }
    writer_.WriteVarInt(val - last_.value_or(0));
    last_ = val;
    return {};
  }

  std::vector<uint8_t> IntoVec() && { return writer_.Consume(); }

 private:
  BufferWriter writer_;
  std::optional<int32_t> last_;
};

template <typename I1, typename I2>
class MergedIntIterator {
 public:
  MergedIntIterator(I1 begin1, I1 end1, I2 begin2, I2 end2)
      : iter1_(std::move(begin1)),
        end1_(std::move(end1)),
        iter2_(std::move(begin2)),
        end2_(std::move(end2)) {
    Advance1();
    Advance2();
  }

  std::optional<uint32_t> Next() {
    if (peek1_.has_value() && peek2_.has_value()) {
      if (peek1_.value() <= peek2_.value()) {
        uint32_t val = peek1_.value();
        Advance1();
        return val;
      }
      uint32_t val = peek2_.value();
      Advance2();
      return val;
    }
    if (peek1_.has_value()) {
      uint32_t val = peek1_.value();
      Advance1();
      return val;
    }
    if (peek2_.has_value()) {
      uint32_t val = peek2_.value();
      Advance2();
      return val;
    }
    return std::nullopt;
  }

 private:
  void Advance1() {
    if (iter1_ != end1_) {
      peek1_ = *iter1_;
      ++iter1_;
    } else {
      peek1_ = std::nullopt;
    }
  }

  void Advance2() {
    if (iter2_ != end2_) {
      peek2_ = *iter2_;
      ++iter2_;
    } else {
      peek2_ = std::nullopt;
    }
  }

  I1 iter1_;
  I1 end1_;
  I2 iter2_;
  I2 end2_;
  std::optional<uint32_t> peek1_;
  std::optional<uint32_t> peek2_;
};

}  // namespace zetasketch::utils

#endif  // ZETASKETCH_UTILS_ITERATORS_H_
