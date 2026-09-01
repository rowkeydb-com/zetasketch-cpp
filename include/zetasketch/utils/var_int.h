// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_UTILS_VAR_INT_H_
#define ZETASKETCH_UTILS_VAR_INT_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include "zetasketch/utils/error.h"

namespace zetasketch::utils {

class VarInt {
 public:
  static constexpr uint32_t kShiftBits = 7U;
  static constexpr uint8_t kDataMask = 0x7FU;
  static constexpr uint8_t kMoreBytesMask = 0x80U;
  // A 32-bit value occupies at most five 7-bit groups.
  static constexpr size_t kMaxEncodedBytes = 5;

  // Returns the number of bytes required to encode the given value as a varint.
  static constexpr size_t Size(int32_t value) {
    size_t result = 0;
    auto uval = static_cast<uint32_t>(value);
    while (true) {
      result++;
      uval >>= kShiftBits;
      if (uval == 0) {
        break;
      }
    }
    return result;
  }

  struct Decoded {
    int32_t value;
    size_t bytes_read;
  };

  // Decodes a varint from the source span. The input is not trusted:
  // decoding fails if the encoding continues past the end of the span
  // or exceeds the maximum encoded length of a 32-bit value. In a
  // five-byte encoding the final byte contributes only its low four
  // value bits; higher bits fall outside a 32-bit value and are
  // discarded.
  [[nodiscard]] static constexpr std::expected<Decoded, Error> Get(
      std::span<const uint8_t> src) {
    // The in-loop bounds check would also catch this case; the branch
    // exists to report an accurate message for an absent varint.
    if (src.empty()) {
      return std::unexpected(
          Error{.code = ErrorCode::kInvalidState,
                .message = "Cannot decode a varint from an empty buffer"});
    }
    uint32_t result = 0;
    uint32_t shift = 0;
    size_t offset = 0;

    while (true) {
      if (offset >= kMaxEncodedBytes) {
        return std::unexpected(
            Error{.code = ErrorCode::kInvalidState,
                  .message = "Varint encoding exceeds five bytes"});
      }
      if (offset >= src.size()) {
        return std::unexpected(
            Error{.code = ErrorCode::kInvalidState,
                  .message = "Varint continues past the end of the buffer"});
      }
      const uint8_t b = src[offset++];
      result |= (static_cast<uint32_t>(b & kDataMask) << shift);
      shift += kShiftBits;
      if ((b & kMoreBytesMask) == 0) {
        break;
      }
    }
    return Decoded{.value = static_cast<int32_t>(result), .bytes_read = offset};
  }

  // Encodes the value as a varint into the sink span.
  // Returns the number of bytes written.
  static constexpr size_t Set(int32_t value, std::span<uint8_t> sink) {
    size_t offset = 0;
    auto uval = static_cast<uint32_t>(value);
    while (true) {
      const auto bits = static_cast<uint8_t>(uval & kDataMask);
      uval >>= kShiftBits;
      const auto b =
          static_cast<uint8_t>(bits | (uval != 0 ? kMoreBytesMask : 0x00U));
      sink[offset++] = b;
      if (uval == 0) {
        break;
      }
    }
    return offset;
  }
};

}  // namespace zetasketch::utils

#endif  // ZETASKETCH_UTILS_VAR_INT_H_
