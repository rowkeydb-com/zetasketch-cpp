// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_MATH_UTILS_H_
#define ZETASKETCH_HLL_MATH_UTILS_H_

#include <cstdint>

namespace zetasketch::hll {

// Returns the value of alpha_m (where m = 2^precision).
[[nodiscard]] double Alpha(int32_t precision);

// Returns the estimate threshold below which LinearCounting is preferred.
[[nodiscard]] int32_t LinearCountingThreshold(int32_t precision);

// Returns the bias correction for the given estimate and precision.
[[nodiscard]] double EstimateBias(double estimate, int32_t precision);

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_MATH_UTILS_H_
