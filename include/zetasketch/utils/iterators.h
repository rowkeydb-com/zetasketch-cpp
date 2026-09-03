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
#include "zetasketch/utils/error.h"

namespace zetasketch::utils {

class DifferenceDecoder {
 public:
  explicit DifferenceDecoder(std::span<const uint8_t> data) : reader_(data) {}

  // Returns the next decoded value. Returns std::nullopt when the span
  // is exhausted, and also when decoding fails; error() distinguishes
  // the two cases once iteration has ended.
  [[nodiscard]] std::optional<uint32_t> Next() {
    if (error_.has_value() || !reader_.HasRemaining()) {
      return std::nullopt;
    }
    auto result = reader_.ReadVarInt();
    if (!result.has_value()) {
      error_ = result.error();
      return std::nullopt;
    }
    last_ = last_ + static_cast<uint32_t>(result.value());
    return last_;
  }

  // Holds the decode failure if iteration ended on one.
  [[nodiscard]] const std::optional<Error>& error() const { return error_; }

 private:
  BufferReader reader_;
  uint32_t last_{0};
  std::optional<Error> error_;
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
      return std::unexpected(Error{
          .code = ErrorCode::kIllegalArgument,
          .message =
              std::format("only positive integers supported but got {}", val)});
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
    if (has_peek1_ && has_peek2_) {
      if (peek1_ <= peek2_) {
        const uint32_t val = peek1_;
        Advance1();
        return val;
      }
      const uint32_t val = peek2_;
      Advance2();
      return val;
    }
    if (has_peek1_) {
      const uint32_t val = peek1_;
      Advance1();
      return val;
    }
    if (has_peek2_) {
      const uint32_t val = peek2_;
      Advance2();
      return val;
    }
    return std::nullopt;
  }

 private:
  void Advance1() {
    has_peek1_ = iter1_ != end1_;
    if (has_peek1_) {
      peek1_ = *iter1_;
      ++iter1_;
    }
  }

  void Advance2() {
    has_peek2_ = iter2_ != end2_;
    if (has_peek2_) {
      peek2_ = *iter2_;
      ++iter2_;
    }
  }

  I1 iter1_;
  I1 end1_;
  I2 iter2_;
  I2 end2_;
  // Each peeked value is stored as a plain value and a flag, with the
  // value always initialized, because GCC cannot prove the guarded read
  // of a std::optional payload initialized once Next() is inlined under
  // optimization, and warnings are errors.
  uint32_t peek1_ = 0;
  uint32_t peek2_ = 0;
  bool has_peek1_ = false;
  bool has_peek2_ = false;
};

}  // namespace zetasketch::utils

#endif  // ZETASKETCH_UTILS_ITERATORS_H_
