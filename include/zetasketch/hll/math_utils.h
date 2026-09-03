// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_MATH_UTILS_H_
#define ZETASKETCH_HLL_MATH_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace zetasketch::hll {

// Returns the position the reference implementation's search reports
// for a value in a table of means: the index of an equal element where
// one is found, and otherwise the position at which the value would be
// inserted. This reproduces the probing sequence of that search rather
// than that of a lower bound. The two agree on sorted input and need
// not agree otherwise, and two of the reference's own mean tables are
// not sorted, so the bias correction uses this and not a lower bound.
// The two choose a different position at isolated tabulated values of
// one row and across a short interval of the other, and produce the
// same correction at every estimate tried, because the six nearest
// neighbours lie well inside either window. Sweeping both rows
// uniformly at two million points each, the positions differ at 690 of
// them, all within one interval of the precision-6 row, and the
// correction differs at none; no estimate is known at which the choice
// is observable through EstimateBias. This function is therefore exported so
// that its agreement with the reference can be asserted directly. That
// assertion guards the function; it does not guard the call site
// below, and substituting a lower bound there would fail no test.
//
// The position is the reference's for every input free of NaN,
// including duplicates, negative zero and both infinities. It is not
// the reference's for NaN, because the reference compares canonicalised
// bit patterns and this compares the pattern it is given. No estimate
// reaching this function can be NaN, the raw estimate being a positive
// quotient, and both implementations return NaN from a NaN estimate
// whatever position they choose.
[[nodiscard]] size_t ReferenceInsertionPoint(std::span<const double> values,
                                             double key);

// Rounds as the reference's language does, which is not what casting
// the result of std::round produces. The reference rounds a half
// towards positive infinity, maps a value that is not a number to
// zero, and saturates at the extremes of the result type, where the
// cast leaves all three undefined. A sketch the reference accepts can
// drive an estimate to infinity or to a value that is not a number,
// because it validates neither the length of a register array's
// contents nor the sparse size against the sparse precision, so the
// difference is reachable rather than theoretical.
[[nodiscard]] int64_t RoundAsTheReferenceDoes(double value);

// Returns the value of alpha_m (where m = 2^precision).
[[nodiscard]] double Alpha(int32_t precision);

// Returns the estimate threshold below which LinearCounting is preferred.
[[nodiscard]] int32_t LinearCountingThreshold(int32_t precision);

// Returns the bias correction for the given estimate and precision.
[[nodiscard]] double EstimateBias(double estimate, int32_t precision);

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_MATH_UTILS_H_
