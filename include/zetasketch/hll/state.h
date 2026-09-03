// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_STATE_H_
#define ZETASKETCH_HLL_STATE_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "aggregator.pb.h"
#include "zetasketch/utils/error.h"

namespace zetasketch::hll {

// This enumeration represents the mathematical value types corresponding
// directly to the Java and Rust reference implementations.
// The value types the reference's aggregator admits. It records the
// type of the first value added and refuses any later value of another
// type; every other value type it refuses outright when reading.
enum class ValueType : int32_t {
  kUnknown = 0,
  kUnsignedInt32 = 7,
  kUnsignedInt64 = 8,
  kBytesOrUtf8String = 11,
};

struct State {
  // The aggregator type, absent when the number on the wire is one this
  // build does not recognise. The reference holds a nullable
  // enumeration here for the same reason and refuses such a value by
  // the name "null"; a field that is not present at all means this
  // library's own type, which is why the default is engaged.
  std::optional<zetasketch::AggregatorType> type =
      zetasketch::HYPERLOGLOG_PLUS_UNIQUE;
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

  // Serializes the internal state directly into a provided string buffer.
  [[nodiscard]] std::expected<void, utils::Error> ToByteArray(
      std::string* output) const;
};

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_STATE_H_
