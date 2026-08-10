// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HYPERLOGLOGPLUSPLUS_H_
#define ZETASKETCH_HYPERLOGLOGPLUSPLUS_H_

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "zetasketch/hll/representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/buffer_traits.h"

namespace zetasketch {

class HyperLogLogPlusPlus {
 public:
  static constexpr int32_t kMinimumPrecision = 10;
  static constexpr int32_t kMaximumPrecision = 24;
  static constexpr int32_t kDefaultNormalPrecision = 15;
  static constexpr int32_t kMaximumSparsePrecision = 30;
  static constexpr int32_t kSparsePrecisionDisabled = 0;
  static constexpr int32_t kDefaultSparsePrecisionDelta = 5;
  static constexpr int32_t kEncodingVersion = 2;

  // Constructs a new sketch.
  // We use std::expected for allocation-free error handling.
  static std::expected<HyperLogLogPlusPlus, utils::Error> Create(
      int32_t normal_precision = kDefaultNormalPrecision,
      int32_t sparse_precision = kSparsePrecisionDisabled);

  // Deserializes a sketch from a BigTable/byte array representation.
  static std::expected<HyperLogLogPlusPlus, utils::Error> FromBytes(
      std::span<const uint8_t> data);

  // Deserializes a sketch from a string_view representation.
  static std::expected<HyperLogLogPlusPlus, utils::Error> FromBytes(
      std::string_view data);

  // This class is designated as movable, but it explicitly prohibits copy
  // operations to ensure strict memory constraints.
  HyperLogLogPlusPlus(HyperLogLogPlusPlus&& other) noexcept = default;
  HyperLogLogPlusPlus& operator=(HyperLogLogPlusPlus&& other) noexcept =
      default;
  HyperLogLogPlusPlus(const HyperLogLogPlusPlus&) = delete;
  HyperLogLogPlusPlus& operator=(const HyperLogLogPlusPlus&) = delete;
  ~HyperLogLogPlusPlus() = default;

  // Adds a string value to the sketch.
  void Add(std::string_view value);

  // Adds an integer value to the sketch.
  void Add(int64_t value);

  // This function adds a precomputed raw hash to the sketch, which is utilized
  // internally or by algorithmic extensions.
  std::expected<void, utils::Error> AddHash(uint64_t hash);

  // Merges another sketch into this one.
  // Consumes `other` explicitly via std::move() to enforce zero-allocation
  // architectural constraints where possible.
  std::expected<void, utils::Error> Merge(HyperLogLogPlusPlus&& other);

  // Returns the estimated cardinality.
  [[nodiscard]] int64_t Result() const;

  // Serializes the sketch to a byte array.
  [[nodiscard]] std::expected<std::vector<uint8_t>, utils::Error> Serialize()
      const;

  // Serializes into a provided buffer to avoid allocation.
  std::expected<void, utils::Error> Serialize(std::vector<uint8_t>& sink) const;

  // Serializes into a provided string buffer to avoid allocation.
  std::expected<void, utils::Error> Serialize(std::string& sink) const;

 private:
  explicit HyperLogLogPlusPlus(hll::Representation representation)
      : representation_(std::move(representation)) {}

  // Extracts and conditionally compacts the internal state for serialization.
  [[nodiscard]] std::expected<hll::State, utils::Error>
  GetStateForSerialization() const;

  hll::Representation representation_;
};

}  // namespace zetasketch

#endif  // ZETASKETCH_HYPERLOGLOGPLUSPLUS_H_
