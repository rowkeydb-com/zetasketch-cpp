// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_SPARSE_REPRESENTATION_H_
#define ZETASKETCH_HLL_SPARSE_REPRESENTATION_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <variant>
#include <vector>
#include "zetasketch/hll/encoding.h"
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/error.h"

namespace zetasketch::hll {

class SparseRepresentation;
class NormalRepresentation;
using Representation = std::variant<SparseRepresentation, NormalRepresentation>;

class SparseRepresentation {
 public:
  static constexpr int32_t kMaximumSparsePrecision = 25;
  static constexpr int32_t kSparsePrecisionDisabled = 0;
  static constexpr float kMaximumSparseDataFraction = 0.75F;
  static constexpr float kMaximumBufferElementsFraction =
      1.0F - kMaximumSparseDataFraction;

  [[nodiscard]] static std::expected<SparseRepresentation, utils::Error> Create(
      State state);

  [[nodiscard]] static std::expected<void, utils::Error> CheckPrecision(
      int32_t normal_precision, int32_t sparse_precision);

  // Adds a single hash value to the sparse representation.
  [[nodiscard]] std::expected<Representation, utils::Error> AddHash(
      uint64_t hash) &&;

  // Adds a single sparse value using the source encoding.
  [[nodiscard]] std::expected<Representation, utils::Error> AddSparseValue(
      const encoding::Sparse& source_sparse_encoding, uint32_t sparse_value) &&;

  // Estimates the cardinality using the sparse representation.
  [[nodiscard]] std::expected<int64_t, utils::Error> Estimate() const;

  // Merges another sparse representation into this one.
  [[nodiscard]] std::expected<Representation, utils::Error> MergeFromSparse(
      const SparseRepresentation& other) &&;

  // Compacts the sparse representation.
  [[nodiscard]] std::expected<Representation, utils::Error> Compact() &&;

  // Converts this sparse representation to a normal representation.
  [[nodiscard]] std::expected<Representation, utils::Error> Normalize() &&;

  [[nodiscard]] State& state() { return state_; }
  [[nodiscard]] const State& state() const { return state_; }
  [[nodiscard]] const encoding::Sparse& encoding() const { return encoding_; }

 private:
  SparseRepresentation(State state, encoding::Sparse encoding,
                       size_t max_sparse_data_bytes,
                       size_t max_buffer_elements);

  [[nodiscard]] std::expected<void, utils::Error> FlushBuffer();
  std::expected<void, utils::Error> SortAndDedupBuffer();

  [[nodiscard]] std::expected<Representation, utils::Error>
  UpdateRepresentation() &&;

  State state_;
  encoding::Sparse encoding_;
  size_t max_sparse_data_bytes_;
  size_t max_buffer_elements_;
  std::vector<uint32_t> buffer_;
  std::vector<uint8_t> scratch_sparse_data_;
};

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_SPARSE_REPRESENTATION_H_
