// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/math_utils.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <utility>
#include "zetasketch/bias_data.h"

namespace zetasketch::hll {
namespace {

struct WeightedBias {
  double bias;
  double distance;
};

constexpr int32_t kLinearThresholdDivisor = 2;
constexpr int32_t kLinearThresholdMultiplier = 5;
constexpr double kAlphaNumerator = 0.7213;
constexpr double kAlphaDenominatorOffset = 1.079;
// The three lowest precisions do not use the formula. The reference
// implementation takes these values verbatim from the Go and C++
// implementations, tabulated here from the lowest precision upwards.
constexpr int32_t kLowestTabulatedAlphaPrecision = 4;
constexpr std::array<double, 3> kTabulatedAlpha = {0.673101517, 0.697121585,
                                                   0.709209798};

}  // namespace

size_t ReferenceInsertionPoint(std::span<const double> values, double key) {
  int64_t low = 0;
  int64_t high = static_cast<int64_t>(values.size()) - 1;
  while (low <= high) {
    const int64_t middle = (low + high) / 2;
    const double middle_value = *std::next(values.begin(), middle);
    if (middle_value < key) {
      low = middle + 1;
    } else if (middle_value > key) {
      high = middle - 1;
    } else {
      // Neither less nor greater. The reference then compares bit
      // patterns, which distinguishes negative zero from zero and
      // orders a quiet NaN above every number.
      const auto middle_bits = std::bit_cast<int64_t>(middle_value);
      const auto key_bits = std::bit_cast<int64_t>(key);
      if (middle_bits == key_bits) {
        return static_cast<size_t>(middle);
      }
      if (middle_bits < key_bits) {
        low = middle + 1;
      } else {
        high = middle - 1;
      }
    }
  }
  return static_cast<size_t>(low);
}

namespace {

// Divides rounding towards negative infinity, which is what an
// arithmetic shift of a signed value does and what the built-in
// division, rounding towards zero, does not.
int64_t FloorDivide(int64_t numerator, int64_t denominator) {
  const int64_t quotient = numerator / denominator;
  const int64_t remainder = numerator % denominator;
  return (remainder != 0 && ((remainder < 0) != (denominator < 0)))
             ? quotient - 1
             : quotient;
}

}  // namespace

// Reproduces the rounding of the reference's language exactly,
// including the case that language leaves to its narrowing conversion.
// The decomposition is that language's own: it takes the significand,
// divides away the fractional bits, and adds a half before halving,
// which rounds a half towards positive infinity rather than away from
// zero. Its shifts are arithmetic, so the divisions here round towards
// negative infinity.
int64_t RoundAsTheReferenceDoes(double value) {
  constexpr int64_t kSignificandWidth = 53;
  constexpr int64_t kExponentBias = 1023;
  constexpr int64_t kShiftWidth = 64;
  constexpr uint64_t kExponentMask = 0x7FF0000000000000ULL;
  constexpr uint64_t kSignificandMask = 0x000FFFFFFFFFFFFFULL;

  const auto bits = std::bit_cast<int64_t>(value);
  const auto biased_exponent =
      static_cast<int64_t>((static_cast<uint64_t>(bits) & kExponentMask) >>
                           static_cast<uint64_t>(kSignificandWidth - 1));
  const int64_t shift =
      (kSignificandWidth - 2 + kExponentBias) - biased_exponent;

  if (shift >= 0 && shift < kShiftWidth) {
    auto significand =
        static_cast<int64_t>((static_cast<uint64_t>(bits) & kSignificandMask) |
                             (kSignificandMask + 1));
    if (bits < 0) {
      significand = -significand;
    }
    const auto divisor =
        static_cast<int64_t>(uint64_t{1} << static_cast<uint64_t>(shift));
    return FloorDivide(FloorDivide(significand, divisor) + 1, 2);
  }

  // The shift leaves that range only for a magnitude at or above two to
  // the fifty-second power, which is already integral, and for a value
  // that is not a number. The reference's narrowing conversion
  // saturates at the extremes and maps the latter to zero, where ours
  // would be undefined.
  if (std::isnan(value)) {
    return 0;
  }
  if (value >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
    return std::numeric_limits<int64_t>::min();
  }
  return static_cast<int64_t>(value);
}

double Alpha(int32_t precision) {
  const int32_t tabulated = precision - kLowestTabulatedAlphaPrecision;
  if (tabulated >= 0 && std::cmp_less(tabulated, kTabulatedAlpha.size())) {
    return kTabulatedAlpha.at(static_cast<size_t>(tabulated));
  }
  return kAlphaNumerator /
         (1.0 + kAlphaDenominatorOffset /
                    static_cast<double>(uint32_t{1}
                                        << static_cast<uint32_t>(precision)));
}

int32_t LinearCountingThreshold(int32_t precision) {
  if (precision >= internal::kMinimumPrecision &&
      precision <= internal::kMaximumPrecision) {
    auto idx = static_cast<size_t>(precision - internal::kMinimumPrecision);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return internal::kLinearCountingThreshold[idx];
  }
  return kLinearThresholdMultiplier *
         static_cast<int32_t>(uint32_t{1} << static_cast<uint32_t>(precision)) /
         kLinearThresholdDivisor;
}

double EstimateBias(double estimate, int32_t precision) {
  if (precision < internal::kMinimumPrecision ||
      precision > internal::kMaximumPrecision) {
    return 0.0;
  }

  auto index = static_cast<size_t>(precision - internal::kMinimumPrecision);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  const std::span<const double> biases = internal::kBiasData[index];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  const std::span<const double> means = internal::kMeanData[index];

  if (estimate < means.front() || estimate > means.back()) {
    return 0.0;
  }

  const size_t pos = ReferenceInsertionPoint(means, estimate);

  const size_t bottom = (pos > internal::kNumberOfNeighborsInKnn)
                            ? pos - internal::kNumberOfNeighborsInKnn
                            : 0;
  const size_t top =
      std::min(means.size(), pos + internal::kNumberOfNeighborsInKnn);

  std::array<WeightedBias, internal::kNumberOfNeighborsInKnn * 2> neighbors{};
  size_t count = 0;
  for (size_t i = bottom; i < top; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    const double diff = means[i] - estimate;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    neighbors.at(count++) = {.bias = biases[i], .distance = diff * diff};
  }

  if (count == 0) {
    return 0.0;
  }

  // Optimize for exact match (distance == 0)
  for (size_t i = 0; i < count; ++i) {
    if (neighbors.at(i).distance == 0.0) {
      return neighbors.at(i).bias;
    }
  }

  // The reference sorts the whole window and takes the closest six,
  // with a sort that is stable. Stability is not incidental: its own
  // tables hold means equidistant from an estimate, and which of two
  // equally distant neighbours is accumulated first changes the last
  // bit of the result. Insertion sort is stable and allocates
  // nothing, and the window holds at most twice the number of
  // neighbours.
  for (size_t i = 1; i < count; ++i) {
    const WeightedBias closer = neighbors.at(i);
    size_t j = i;
    while (j > 0 && neighbors.at(j - 1).distance > closer.distance) {
      neighbors.at(j) = neighbors.at(j - 1);
      --j;
    }
    neighbors.at(j) = closer;
  }

  const size_t k =
      std::min(count, static_cast<size_t>(internal::kNumberOfNeighborsInKnn));

  double sum = 0.0;
  double total_weight = 0.0;
  for (size_t i = 0; i < k; ++i) {
    total_weight += 1.0 / neighbors.at(i).distance;
    sum += neighbors.at(i).bias / neighbors.at(i).distance;
  }

  return sum / total_weight;
}

}  // namespace zetasketch::hll
