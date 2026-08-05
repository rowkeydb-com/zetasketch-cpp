// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_REPRESENTATION_H_
#define ZETASKETCH_HLL_REPRESENTATION_H_

#include <variant>
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/sparse_representation.h"

namespace zetasketch::hll {

// Use std::variant to encapsulate Sparse and Dense states,
// completely avoiding heap-allocated virtual dispatch.
using Representation = std::variant<SparseRepresentation, NormalRepresentation>;

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_REPRESENTATION_H_
