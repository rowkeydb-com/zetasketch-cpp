// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_REPRESENTATION_H_
#define ZETASKETCH_HLL_REPRESENTATION_H_

#include <cstdint>
#include <variant>
#include <vector>

namespace zetasketch::hll {

// These are forward declarations for the internal structural states.
// Their full implementations will be developed in Commits 6 and 7.
class SparseRepresentation {
 public:
  SparseRepresentation(int32_t normal_precision, int32_t sparse_precision)
      : normal_precision_(normal_precision),
        sparse_precision_(sparse_precision) {}

  // We explicitly manage our sparse representation in a vector.
  // We will initialize it with reserve() to avoid allocations.
  std::vector<uint32_t> data_;
  int32_t normal_precision_;
  int32_t sparse_precision_;
};

class NormalRepresentation {
 public:
  explicit NormalRepresentation(int32_t precision) : precision_(precision) {}

  std::vector<uint8_t> data_;
  int32_t precision_;
};

// Use std::variant to encapsulate Sparse and Dense states,
// completely avoiding heap-allocated virtual dispatch.
using Representation = std::variant<SparseRepresentation, NormalRepresentation>;

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_REPRESENTATION_H_
