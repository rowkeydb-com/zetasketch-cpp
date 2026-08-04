// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/normal_representation.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "zetasketch/hll/encoding.h"
#include "zetasketch/hll/math_utils.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/buffer_traits.h"

namespace zetasketch::hll {

std::expected<NormalRepresentation, utils::Error> NormalRepresentation::Create(
    State state) {
  auto check = CheckPrecision(state.precision);
  if (!check) return std::unexpected(check.error());

  auto encoding = encoding::Normal::Create(state.precision);
  if (!encoding) return std::unexpected(encoding.error());

  if (state.data.has_value()) {
    const size_t expected_size = size_t{1}
                                 << static_cast<size_t>(state.precision);
    if (state.data->size() != expected_size) {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kInvalidState,
          .message = std::format(
              "Expected normal data to consist of exactly {} bytes but got {}",
              expected_size, state.data->size())});
    }
  }

  state.sparse_data.reset();
  state.sparse_size = 0;
  return NormalRepresentation(std::move(state), *encoding);
}

std::expected<void, utils::Error> NormalRepresentation::CheckPrecision(
    int32_t precision) {
  if (precision < kMinimumPrecision || precision > kMaximumPrecision) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message = std::format(
            "Expected normal precision to be >= {} and <= {} but was {}",
            kMinimumPrecision, kMaximumPrecision, precision)});
  }
  return {};
}

NormalRepresentation::NormalRepresentation(State state,
                                           encoding::Normal encoding)
    : state_(std::move(state)), encoding_(std::move(encoding)) {}

std::vector<uint8_t>& NormalRepresentation::EnsureDataMut() {
  if (!state_.data.has_value()) {
    state_.data = std::vector<uint8_t>(
        size_t{1} << static_cast<size_t>(state_.precision), 0);
  }
  return *state_.data;
}

std::expected<void, utils::Error> NormalRepresentation::AddHash(uint64_t hash) {
  const uint32_t idx = encoding_.Index(hash);
  const uint8_t rho_w = encoding_.RhoW(hash);

  auto& data = EnsureDataMut();
  if (idx < data.size()) {
    data[idx] = std::max(data[idx], rho_w);
  } else {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kInvalidState,
        .message = std::format("Index {} out of bounds for data length {}", idx,
                               data.size())});
  }
  return {};
}

std::expected<void, utils::Error> NormalRepresentation::AddSparseValue(
    const encoding::Sparse& source_sparse_encoding, uint32_t sparse_value) {
  auto result = MaybeDowngrade(source_sparse_encoding.normal(),
                               source_sparse_encoding.sparse_precision());
  if (!result) return result;

  auto& data = EnsureDataMut();
  return AddSparseValueMaybeDowngrading(data, encoding_, sparse_value,
                                        source_sparse_encoding);
}

std::expected<int64_t, utils::Error> NormalRepresentation::Estimate() const {
  if (!state_.data.has_value()) {
    return 0;
  }
  const auto& data = *state_.data;

  const int32_t num_zeros =
      static_cast<int32_t>(std::ranges::count(data, uint8_t{0}));
  const double sum = std::transform_reduce(
      data.begin(), data.end(), 0.0, std::plus<>(), [](uint8_t v_byte) {
        return 1.0 / static_cast<double>(uint64_t{1}
                                         << static_cast<uint32_t>(v_byte));
      });

  auto m = static_cast<double>(uint64_t{1}
                               << static_cast<uint32_t>(state_.precision));

  if (num_zeros > 0) {
    auto linear_count_threshold =
        static_cast<double>(LinearCountingThreshold(state_.precision));
    const double h = m * std::log(m / static_cast<double>(num_zeros));
    if (h <= linear_count_threshold) {
      return static_cast<int64_t>(std::round(h));
    }
  }

  const double raw_estimate = Alpha(state_.precision) * m * m / sum;
  const double bias_correction = EstimateBias(raw_estimate, state_.precision);

  return static_cast<int64_t>(std::round(raw_estimate - bias_correction));
}

std::expected<void, utils::Error> NormalRepresentation::MergeFromNormal(
    NormalRepresentation other) {
  auto downgrade_res =
      MaybeDowngrade(other.encoding_, other.state_.sparse_precision);
  if (!downgrade_res) return downgrade_res;

  return MergeNormalDataMaybeDowngrading(
      state_, encoding_, std::move(other.state_.data), other.encoding_);
}

std::expected<void, utils::Error> NormalRepresentation::MaybeDowngrade(
    const encoding::Normal& encoding, int32_t sparse_precision) {
  if (state_.precision <= encoding.precision() &&
      state_.sparse_precision <= sparse_precision) {
    return {};
  }

  if (state_.precision > encoding.precision()) {
    std::optional<std::vector<uint8_t>> source_data_opt;
    source_data_opt.swap(state_.data);
    state_.data = std::vector<uint8_t>(
        size_t{1} << static_cast<size_t>(encoding.precision()), 0);
    state_.precision = encoding.precision();

    auto new_target_encoding = encoding::Normal::Create(state_.precision);
    if (!new_target_encoding)
      return std::unexpected(new_target_encoding.error());

    auto merge_res = MergeNormalDataMaybeDowngrading(
        state_, *new_target_encoding, std::move(source_data_opt), encoding_);
    if (!merge_res) return merge_res;
  }

  state_.sparse_precision = std::min(state_.sparse_precision, sparse_precision);
  if (encoding_.precision() != state_.precision) {
    auto new_encoding = encoding::Normal::Create(state_.precision);
    if (!new_encoding) return std::unexpected(new_encoding.error());
    encoding_ = std::move(*new_encoding);
  }
  return {};
}

std::expected<void, utils::Error>
NormalRepresentation::MergeNormalDataMaybeDowngrading(
    State& state, const encoding::Normal& target_encoding,
    std::optional<std::vector<uint8_t>> source_data_opt,
    const encoding::Normal& source_encoding) {
  if (!source_data_opt.has_value()) {
    return {};
  }
  const auto& source_data = *source_data_opt;

  if (target_encoding.precision() == source_encoding.precision()) {
    if (!state.data.has_value()) {
      state.data = std::vector<uint8_t>(
          size_t{1} << static_cast<size_t>(state.precision), 0);
    }
    auto& data_slice = *state.data;
    if (data_slice.size() == source_data.size()) {
      for (size_t i = 0; i < data_slice.size(); ++i) {
        data_slice[i] = std::max(data_slice[i], source_data[i]);
      }
    } else {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kInvalidState,
          .message = "Mismatched data lengths in "
                     "MergeNormalDataMaybeDowngrading for same precision"});
    }
    return {};
  }

  if (!state.data.has_value()) {
    state.data = std::vector<uint8_t>(
        size_t{1} << static_cast<size_t>(state.precision), 0);
  }
  auto& target_array = *state.data;

  for (size_t old_index = 0; old_index < source_data.size(); ++old_index) {
    const uint8_t old_rho_w = source_data[old_index];
    auto new_index = static_cast<size_t>(source_encoding.DowngradeIndex(
        static_cast<uint32_t>(old_index), target_encoding.precision()));
    const uint8_t new_rho_w =
        source_encoding.DowngradeRhoW(static_cast<uint32_t>(old_index),
                                      old_rho_w, target_encoding.precision());

    if (new_index < target_array.size()) {
      target_array[new_index] = std::max(target_array[new_index], new_rho_w);
    } else {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kInvalidState,
          .message = std::format(
              "Downgraded index {} out of bounds for target array length {}",
              new_index, target_array.size())});
    }
  }
  return {};
}

std::expected<void, utils::Error>
NormalRepresentation::AddSparseValueMaybeDowngrading(
    std::span<uint8_t> data, const encoding::Normal& target_normal_encoding,
    uint32_t sparse_value, const encoding::Sparse& source_sparse_encoding) {
  size_t idx = 0;
  uint8_t rho_w = 0;

  if (target_normal_encoding.precision() <
      source_sparse_encoding.normal().precision()) {
    idx = static_cast<size_t>(source_sparse_encoding.normal().DowngradeIndex(
        source_sparse_encoding.DecodeNormalIndex(sparse_value),
        target_normal_encoding.precision()));
    rho_w = source_sparse_encoding.normal().DowngradeRhoW(
        source_sparse_encoding.DecodeNormalIndex(sparse_value),
        source_sparse_encoding.DecodeNormalRhoW(sparse_value),
        target_normal_encoding.precision());
  } else {
    idx = static_cast<size_t>(
        source_sparse_encoding.DecodeNormalIndex(sparse_value));
    rho_w = source_sparse_encoding.DecodeNormalRhoW(sparse_value);
  }

  if (idx < data.size()) {
    data[idx] = std::max(data[idx], rho_w);
  } else {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kInvalidState,
        .message = std::format(
            "Decoded index {} out of bounds for data length {} "
            "(sparse_value={}, source_precision={}, target_precision={})",
            idx, data.size(), sparse_value,
            source_sparse_encoding.sparse_precision(),
            target_normal_encoding.precision())});
  }
  return {};
}

}  // namespace zetasketch::hll
