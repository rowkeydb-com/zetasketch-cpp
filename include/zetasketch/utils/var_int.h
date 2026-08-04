// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_UTILS_VAR_INT_H_
#define ZETASKETCH_UTILS_VAR_INT_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zetasketch::utils {

class VarInt {
 public:
  static constexpr uint32_t kShiftBits = 7U;
  static constexpr uint8_t kDataMask = 0x7FU;
  static constexpr uint8_t kMoreBytesMask = 0x80U;

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

  // Decodes a varint from the source span.
  // The caller must ensure that `src` contains a valid varint.
  static constexpr Decoded Get(std::span<const uint8_t> src) {
    uint32_t result = 0;
    uint32_t shift = 0;
    size_t offset = 0;

    while (true) {
      assert(shift < 32 && "Varint too long");
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
