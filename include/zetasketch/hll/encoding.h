// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_ENCODING_H_
#define ZETASKETCH_HLL_ENCODING_H_

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include "zetasketch/utils/error.h"

namespace zetasketch::hll::encoding {

constexpr int32_t kHashBits = 64;
constexpr int32_t kIndexBits = 32;

// A shift count in C++ must be below the width of the promoted
// operand, where the reference's language reduces the count modulo
// that width instead. Masking a count with one of these reproduces
// the reduction exactly.
constexpr uint32_t kHashShiftMask = static_cast<uint32_t>(kHashBits - 1);
constexpr uint32_t kIndexShiftMask = static_cast<uint32_t>(kIndexBits - 1);

[[nodiscard]] inline uint8_t ComputeRhoW(uint64_t value_suffix, int32_t bits) {
  // The reference shifts by kHashBits - bits, which its language
  // reduces modulo the width. Where the sparse and normal precisions
  // are equal, bits is zero and the shift is by the full width, which
  // C++ leaves undefined; reducing it explicitly reproduces the
  // reference exactly.
  const uint64_t w = value_suffix << (static_cast<uint32_t>(kHashBits - bits) &
                                      kHashShiftMask);
  if (w == 0) {
    return static_cast<uint8_t>(bits + 1);
  }
  return static_cast<uint8_t>(std::countl_zero(w) + 1);
}

[[nodiscard]] inline uint8_t DowngradeRhoW(uint32_t index, uint8_t rho_w,
                                           int32_t source_p, int32_t target_p) {
  if (source_p == target_p) {
    return rho_w;
  }
  assert(target_p < source_p);
  // Reduced modulo the width, as the reference's language does and as
  // ComputeRhoW above is. Equal precisions return earlier, so this is
  // defensive rather than reachable.
  const uint32_t suffix =
      index << (static_cast<uint32_t>(kIndexBits - source_p + target_p) &
                kIndexShiftMask);
  if (suffix == 0) {
    return rho_w + static_cast<uint8_t>(source_p) -
           static_cast<uint8_t>(target_p);
  }
  return static_cast<uint8_t>(1 + std::countl_zero(suffix));
}

class Normal {
 public:
  explicit Normal(int32_t precision) : precision_(precision) {}

  static constexpr int32_t kMaxNormalPrecision = 63;
  [[nodiscard]] static std::expected<Normal, utils::Error> Create(
      int32_t precision) {
    if (precision < 1 || precision > kMaxNormalPrecision) {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kIllegalArgument,
          .message = std::format(
              "Normal precision must be between 1 and 63, got {}", precision)});
    }
    return Normal(precision);
  }

  [[nodiscard]] uint32_t Index(uint64_t hash) const {
    return static_cast<uint32_t>(hash >>
                                 static_cast<uint32_t>(kHashBits - precision_));
  }

  [[nodiscard]] uint8_t RhoW(uint64_t hash) const {
    const int32_t num_suffix_bits = kHashBits - precision_;
    const uint64_t suffix_mask =
        (num_suffix_bits >= kHashBits)
            ? ~uint64_t{0}
            : (uint64_t{1} << static_cast<uint32_t>(num_suffix_bits)) - 1;
    const uint64_t value_suffix = hash & suffix_mask;
    return ComputeRhoW(value_suffix, num_suffix_bits);
  }

  [[nodiscard]] uint32_t DowngradeIndex(uint32_t index,
                                        int32_t target_precision) const {
    assert(target_precision <= precision_);
    // Reduced modulo the width, as the reference's language does and as
    // the two shifts above are. The precisions this class admits keep
    // the difference below the width, so this is defensive rather than
    // reachable.
    return index >> (static_cast<uint32_t>(precision_ - target_precision) &
                     kIndexShiftMask);
  }

  [[nodiscard]] uint8_t DowngradeRhoW(uint32_t index, uint8_t rho_w,
                                      int32_t target_precision) const {
    if (rho_w == 0) return 0;
    return encoding::DowngradeRhoW(index, rho_w, precision_, target_precision);
  }

  [[nodiscard]] int32_t precision() const { return precision_; }

 private:
  int32_t precision_;
};

class Sparse {
 public:
  // These are the limits of the encoding itself rather than of a
  // sketch, and are named for what they bound so that they cannot be
  // read as the sketch-level limits. An encoded value is either a
  // sparse index below 2^sparse_precision, or the flag bit
  // 1 << max(sparse_precision, normal_precision + 6) set above a normal
  // index and six rho bits; either way it must remain a non-negative
  // int32, because that is what the difference encoder writes. These
  // are the largest precisions for which that holds, and 31 on either
  // axis would place the flag bit in the sign bit. The limit a sketch
  // may use is lower and is enforced above this layer: see
  // SparseRepresentation::kMaximumSparsePrecision, which matches the
  // reference implementation's public maximum of 25.
  static constexpr int32_t kMaxEncodableNormalPrecision = 24;
  static constexpr int32_t kMaxEncodableSparsePrecision = 30;
  static constexpr int32_t kRhoWBits = 6;
  static constexpr uint32_t kRhoWMask =
      (1U << static_cast<uint32_t>(kRhoWBits)) - 1;

  // The widest value the encoding can produce: the flag bit above a
  // full normal index and a full rho field. The expression grows with
  // both precisions, so evaluating it at the maxima bounds every other
  // configuration.
  static constexpr uint64_t kWidestEncoding =
      (uint64_t{1} << static_cast<uint32_t>(
           std::max(kMaxEncodableSparsePrecision,
                    kMaxEncodableNormalPrecision + kRhoWBits))) +
      (uint64_t{1} << static_cast<uint32_t>(kMaxEncodableNormalPrecision +
                                            kRhoWBits)) -
      1;
  static_assert(kWidestEncoding <=
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max()),
                "the widest sparse encoding must fit in a non-negative int32");

  [[nodiscard]] static std::expected<Sparse, utils::Error> Create(
      int32_t normal_precision, int32_t sparse_precision) {
    if (normal_precision < 1 ||
        normal_precision > kMaxEncodableNormalPrecision) {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kIllegalArgument,
          .message =
              std::format("Sparse mode: normal precision must be 1-{}, got {}",
                          kMaxEncodableNormalPrecision, normal_precision)});
    }
    if (sparse_precision < 1 ||
        sparse_precision > kMaxEncodableSparsePrecision) {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kIllegalArgument,
          .message =
              std::format("Sparse mode: sparse precision must be 1-{}, got {}",
                          kMaxEncodableSparsePrecision, sparse_precision)});
    }
    if (sparse_precision < normal_precision) {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kIllegalArgument,
          .message = std::format(
              "Sparse precision ({}) must be >= normal precision ({})",
              sparse_precision, normal_precision)});
    }

    const int32_t shift =
        std::max(sparse_precision, normal_precision + kRhoWBits);
    const uint32_t rho_encoded_flag = 1U << static_cast<uint32_t>(shift);

    auto normal_encoder = Normal::Create(normal_precision);
    if (!normal_encoder) return std::unexpected(normal_encoder.error());

    return Sparse(normal_precision, sparse_precision, rho_encoded_flag,
                  *normal_encoder);
  }

  [[nodiscard]] uint32_t Encode(uint64_t hash) const {
    const auto sparse_index = static_cast<uint32_t>(
        hash >> static_cast<uint32_t>(kHashBits - sparse_precision_));
    const uint8_t sparse_rho_w =
        ComputeRhoW(hash, kHashBits - sparse_precision_);
    return EncodeParts(sparse_index, sparse_rho_w);
  }

  [[nodiscard]] uint32_t EncodeParts(uint32_t sparse_index,
                                     uint8_t sparse_rho_w) const {
    assert(sparse_index < (1U << static_cast<uint32_t>(sparse_precision_)));
    assert(sparse_rho_w < (1U << static_cast<uint32_t>(kRhoWBits)));

    const uint32_t mask =
        (1U << static_cast<uint32_t>(sparse_precision_ - normal_precision_)) -
        1;
    if ((sparse_index & mask) != 0) {
      return sparse_index;
    }
    const uint32_t normal_index =
        sparse_index >>
        static_cast<uint32_t>(sparse_precision_ - normal_precision_);
    return rho_encoded_flag_ |
           (normal_index << static_cast<uint32_t>(kRhoWBits)) |
           static_cast<uint32_t>(sparse_rho_w);
  }

  [[nodiscard]] uint32_t DecodeSparseIndex(uint32_t sparse_value) const {
    if ((sparse_value & rho_encoded_flag_) == 0) {
      return sparse_value;
    }
    return ((sparse_value ^ rho_encoded_flag_) >>
            static_cast<uint32_t>(kRhoWBits))
           << static_cast<uint32_t>(sparse_precision_ - normal_precision_);
  }

  [[nodiscard]] uint32_t DecodeNormalIndex(uint32_t sparse_value) const {
    if ((sparse_value & rho_encoded_flag_) == 0) {
      return sparse_value >>
             static_cast<uint32_t>(sparse_precision_ - normal_precision_);
    }
    return (sparse_value ^ rho_encoded_flag_) >>
           static_cast<uint32_t>(kRhoWBits);
  }

  [[nodiscard]] uint8_t DecodeSparseRhoWIfPresent(uint32_t sparse_value) const {
    if ((sparse_value & rho_encoded_flag_) == 0) {
      return 0;
    }
    return static_cast<uint8_t>(sparse_value & kRhoWMask);
  }

  [[nodiscard]] uint8_t DecodeNormalRhoW(uint32_t sparse_value) const {
    if ((sparse_value & rho_encoded_flag_) == 0) {
      return ComputeRhoW(sparse_value, sparse_precision_ - normal_precision_);
    }
    return static_cast<uint8_t>((sparse_value & kRhoWMask) +
                                (sparse_precision_ - normal_precision_));
  }

  [[nodiscard]] uint32_t DowngradeSparseValue(uint32_t sparse_value,
                                              const Sparse& target) const {
    const uint32_t old_sparse_index = DecodeSparseIndex(sparse_value);
    const uint8_t old_sparse_rho_w = DecodeSparseRhoWIfPresent(sparse_value);

    // Reduced modulo the width for the same reason as the two shifts
    // above, though unlike ComputeRhoW's this one cannot change a
    // result: every downgrade gives a count below the width.
    const uint32_t new_sparse_index =
        old_sparse_index >>
        (static_cast<uint32_t>(sparse_precision_ - target.sparse_precision()) &
         kIndexShiftMask);
    const uint8_t new_sparse_rho_w =
        DowngradeRhoW(old_sparse_index, old_sparse_rho_w, sparse_precision_,
                      target.sparse_precision());

    return target.EncodeParts(new_sparse_index, new_sparse_rho_w);
  }

  // Orders two sparse encodings as the reference does: one is less than
  // another when either of its precisions is lower. Two encodings can
  // therefore be unordered in both directions, which is what the
  // compatibility test below refuses.
  [[nodiscard]] bool IsLessThan(const Sparse& other) const {
    return normal_precision_ < other.normal_precision_ ||
           sparse_precision_ < other.sparse_precision_;
  }

  // Two sparse encodings can be merged only when one dominates the
  // other in both precisions. A pair where one precision rises and the
  // other falls has no common encoding to merge into, and the reference
  // refuses it in the words reproduced here.
  [[nodiscard]] std::expected<void, utils::Error> AssertCompatible(
      const Sparse& other) const {
    if ((normal_precision_ <= other.normal_precision_ &&
         sparse_precision_ <= other.sparse_precision_) ||
        (normal_precision_ >= other.normal_precision_ &&
         sparse_precision_ >= other.sparse_precision_)) {
      return {};
    }
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIncompatiblePrecision,
        .message = std::format(
            "Precisions (p={}, sp={}) are not compatible to (p={}, sp={})",
            normal_precision_, sparse_precision_, other.normal_precision_,
            other.sparse_precision_)});
  }

  [[nodiscard]] Normal normal() const { return normal_encoder_; }

  [[nodiscard]] int32_t normal_precision() const { return normal_precision_; }
  [[nodiscard]] int32_t sparse_precision() const { return sparse_precision_; }

 private:
  Sparse(int32_t normal_precision, int32_t sparse_precision,
         uint32_t rho_encoded_flag, Normal normal_encoder)
      : normal_precision_(normal_precision),
        sparse_precision_(sparse_precision),
        rho_encoded_flag_(rho_encoded_flag),
        normal_encoder_(normal_encoder) {}

  int32_t normal_precision_;
  int32_t sparse_precision_;
  uint32_t rho_encoded_flag_;
  Normal normal_encoder_;
};

}  // namespace zetasketch::hll::encoding

#endif  // ZETASKETCH_HLL_ENCODING_H_
