// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_NORMAL_REPRESENTATION_H_
#define ZETASKETCH_HLL_NORMAL_REPRESENTATION_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>
#include "zetasketch/hll/encoding.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/error.h"

namespace zetasketch::hll {

class SparseRepresentation;

class NormalRepresentation {
 public:
  static constexpr int32_t kMinimumPrecision = 4;
  static constexpr int32_t kMaximumPrecision = 24;

  [[nodiscard]] static std::expected<NormalRepresentation, utils::Error> Create(
      State state);

  [[nodiscard]] static std::expected<void, utils::Error> CheckPrecision(
      int32_t precision);

  // Adds a single hash value to the dense representation.
  [[nodiscard]] std::expected<void, utils::Error> AddHash(uint64_t hash);

  // Adds a single sparse value using the source encoding.
  [[nodiscard]] std::expected<void, utils::Error> AddSparseValue(
      const encoding::Sparse& source_sparse_encoding, uint32_t sparse_value);

  // Estimates the cardinality using the dense representation.
  [[nodiscard]] std::expected<int64_t, utils::Error> Estimate() const;

  // Performs what the reference's addSparseValues does before it reads
  // any value: downgrades this representation to the source encoding
  // and materialises the register array. It runs even for an empty
  // sequence, which is why it is separate from AddSparseValue.
  [[nodiscard]] std::expected<void, utils::Error> BeginSparseValues(
      const encoding::Sparse& source_sparse_encoding);

  // Materialises the register array if it is absent or holds no bytes.
  // The reference reaches its equivalent unconditionally when a sparse
  // representation normalises, through addSparseValues, so a sketch
  // that normalises with nothing stored still acquires a full array.
  void EnsureRegisterArray();

  // Merges another normal representation into this one.
  [[nodiscard]] std::expected<void, utils::Error> MergeFromNormal(
      NormalRepresentation other);

  [[nodiscard]] State& state() { return state_; }
  [[nodiscard]] const State& state() const { return state_; }

 private:
  NormalRepresentation(State state, encoding::Normal encoding);

  // Ensures data array is initialized and returns a mutable reference.
  std::vector<uint8_t>& EnsureDataMut();

  // Downgrades precision if necessary to match the target.
  [[nodiscard]] std::expected<void, utils::Error> MaybeDowngrade(
      const encoding::Normal& encoding, int32_t sparse_precision);

  // Merges a data array from a source representation into the target state.
  [[nodiscard]] static std::expected<void, utils::Error>
  MergeNormalDataMaybeDowngrading(
      State& state, const encoding::Normal& target_encoding,
      std::optional<std::vector<uint8_t>> source_data_opt,
      const encoding::Normal& source_encoding);

  // Adds a sparse value into a dense data array, applying downgrades.
  [[nodiscard]] static std::expected<void, utils::Error>
  AddSparseValueMaybeDowngrading(
      std::span<uint8_t> data, const encoding::Normal& target_normal_encoding,
      uint32_t sparse_value, const encoding::Sparse& source_sparse_encoding);

  State state_;
  encoding::Normal encoding_;
};

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_NORMAL_REPRESENTATION_H_
