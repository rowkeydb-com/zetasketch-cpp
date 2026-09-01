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
#include "zetasketch/utils/error.h"

namespace zetasketch::hll::encoding {

constexpr int32_t kHashBits = 64;
constexpr int32_t kIndexBits = 32;

[[nodiscard]] inline uint8_t ComputeRhoW(uint64_t value_suffix, int32_t bits) {
  const uint64_t w = value_suffix << static_cast<uint32_t>(kHashBits - bits);
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
  const uint32_t suffix =
      index << static_cast<uint32_t>(kIndexBits - source_p + target_p);
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
    return index >> static_cast<uint32_t>(precision_ - target_precision);
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
  static constexpr int32_t kMaxSparseNormalPrecision = 24;
  static constexpr int32_t kMaxSparsePrecision = 30;
  static constexpr int32_t kRhoWBits = 6;
  static constexpr uint32_t kRhoWMask =
      (1U << static_cast<uint32_t>(kRhoWBits)) - 1;

  [[nodiscard]] static std::expected<Sparse, utils::Error> Create(
      int32_t normal_precision, int32_t sparse_precision) {
    if (normal_precision < 1 || normal_precision > kMaxSparseNormalPrecision) {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kIllegalArgument,
          .message =
              std::format("Sparse mode: normal precision must be 1-24, got {}",
                          normal_precision)});
    }
    if (sparse_precision < 1 || sparse_precision > kMaxSparsePrecision) {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kIllegalArgument,
          .message =
              std::format("Sparse mode: sparse precision must be 1-30, got {}",
                          sparse_precision)});
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

    const uint32_t new_sparse_index =
        old_sparse_index >>
        static_cast<uint32_t>(sparse_precision_ - target.sparse_precision());
    const uint8_t new_sparse_rho_w =
        DowngradeRhoW(old_sparse_index, old_sparse_rho_w, sparse_precision_,
                      target.sparse_precision());

    return target.EncodeParts(new_sparse_index, new_sparse_rho_w);
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
