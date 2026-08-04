// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_STATE_H_
#define ZETASKETCH_HLL_STATE_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>
#include "aggregator.pb.h"
#include "zetasketch/utils/buffer_traits.h"

namespace zetasketch::hll {

// This enumeration represents the mathematical value types corresponding
// directly to the Java and Rust reference implementations.
enum class ValueType : int32_t {
  kUnknown = 0,
  // Maps to DefaultOpsTypeId / CustomValueTypeId depending on the id
};

struct State {
  zetasketch::AggregatorType type = zetasketch::HYPERLOGLOG_PLUS_UNIQUE;
  int64_t num_values = 0;
  int32_t encoding_version = 1;
  ValueType value_type = ValueType::kUnknown;
  int32_t sparse_size = 0;
  int32_t precision = 0;
  int32_t sparse_precision = 0;
  std::optional<std::vector<uint8_t>> data;
  std::optional<std::vector<uint8_t>> sparse_data;

  // This function parses the internal state from a serialized protocol buffer
  // byte array.
  [[nodiscard]] static std::expected<State, utils::Error> Parse(
      std::span<const uint8_t> input);

  // This function serializes the internal state into a protocol buffer byte
  // array.
  [[nodiscard]] std::expected<std::vector<uint8_t>, utils::Error> ToByteArray()
      const;
};

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_STATE_H_
