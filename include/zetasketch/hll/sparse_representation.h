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
#include "zetasketch/hll/sparse_buffer.h"
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

  // Adds this representation's values to a normal one, as the
  // reference's mergeInto does. It adds the stored stream and the
  // buffer through the normal representation's sparse-value path, which
  // touches only the registers those values name. Normalising this
  // representation into a register array and taking a maximum over the
  // whole of it would instead lower every register the values do not
  // name, that maximum being over signed bytes.
  [[nodiscard]] std::expected<void, utils::Error> MergeInto(
      NormalRepresentation& target) const;

  // Lowers this representation to the given encoding, as the
  // reference's downgrade does, re-encoding the stored stream through
  // the target encoding and carrying the buffer across unchanged, in
  // increasing order. Which values are still in the buffer is decided
  // by the flushes before, and those fall where the reference's do
  // because the buffer counts distinct values as the reference's does.
  // It returns this representation unaltered when the target is not
  // lower.
  [[nodiscard]] std::expected<Representation, utils::Error> Downgrade(
      const encoding::Sparse& target) &&;

  // Merges another sparse representation into this one, taking its
  // values in increasing order as the reference does. The operand's
  // buffer is arranged for that, which is why it is not const.
  [[nodiscard]] std::expected<Representation, utils::Error> MergeFromSparse(
      SparseRepresentation& other) &&;

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

  // Writes the stored stream and the buffer as one deduplicated stream,
  // as the reference's flushBuffer does, and empties the buffer.
  [[nodiscard]] std::expected<void, utils::Error> FlushBuffer();

  // Replaces the stored stream with one just written, records how many
  // values it holds, and empties the buffer, as the reference's set does.
  void SetStream(std::vector<uint8_t> stream, int32_t size);

  [[nodiscard]] std::expected<Representation, utils::Error>
  UpdateRepresentation() &&;

  State state_;
  encoding::Sparse encoding_;
  size_t max_sparse_data_bytes_;
  size_t max_buffer_elements_;
  SparseBuffer buffer_;
  std::vector<uint8_t> scratch_sparse_data_;
};

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_SPARSE_REPRESENTATION_H_
