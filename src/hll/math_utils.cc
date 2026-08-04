// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/math_utils.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
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

}  // namespace

double Alpha(int32_t precision) {
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

  auto it = std::ranges::lower_bound(means, estimate);
  auto pos = static_cast<size_t>(std::distance(means.begin(), it));

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

  const size_t k =
      std::min(count, static_cast<size_t>(internal::kNumberOfNeighborsInKnn));

  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::ranges::partial_sort(
      neighbors.begin(),
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      neighbors.begin() + k,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      neighbors.begin() + count,
      [](const WeightedBias& a, const WeightedBias& b) {
        return a.distance < b.distance;
      });

  double sum = 0.0;
  double total_weight = 0.0;
  for (size_t i = 0; i < k; ++i) {
    const double weight = 1.0 / neighbors.at(i).distance;
    total_weight += weight;
    sum += neighbors.at(i).bias * weight;
  }

  return sum / total_weight;
}

}  // namespace zetasketch::hll
