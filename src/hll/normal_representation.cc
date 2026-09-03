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
#include "zetasketch/utils/error.h"

namespace zetasketch::hll {
namespace {

// The reference shifts a signed long by a register value, and its
// language reduces a shift count modulo the width of that type.
constexpr uint32_t kRegisterShiftMask = 63U;

// Materialises a state's register array if it is absent or holds no
// bytes, as the reference's ensureData does. Its test is of the
// contents, so a data field that is present and empty is replaced
// rather than written into.
std::vector<uint8_t>& EnsureData(State& state) {
  if (!state.data.has_value() || state.data->empty()) {
    return state.data.emplace(size_t{1} << static_cast<size_t>(state.precision),
                              0);
  }
  return *state.data;
}

// Takes the maximum of a register and a candidate as the reference
// does. It holds registers in an array of its language's signed byte
// and compares them with a signed comparison, so a register at or
// above 0x80 counts as less than every other. Sketches the reference
// accepts can carry such a register, because it validates the length
// of the register array and never its contents.
void PutMax(uint8_t& target, uint8_t candidate) {
  if (static_cast<int8_t>(target) < static_cast<int8_t>(candidate)) {
    target = candidate;
  }
}

}  // namespace

std::expected<NormalRepresentation, utils::Error> NormalRepresentation::Create(
    State state) {
  auto check = CheckPrecision(state.precision);
  if (!check) return std::unexpected(check.error());

  auto encoding = encoding::Normal::Create(state.precision);
  if (!encoding) return std::unexpected(encoding.error());

  // The reference checks the length only when the data field holds a
  // byte, so a present but empty field passes construction unaltered.
  if (state.data.has_value() && !state.data->empty()) {
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

  // The reference discards the sparse fields in its normalization
  // routine alone, not on construction, and therefore preserves them
  // when it parses a sketch that carries dense and sparse data
  // together. See SparseRepresentation::Normalize.
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
  return EnsureData(state_);
}

void NormalRepresentation::EnsureRegisterArray() { EnsureDataMut(); }

std::expected<void, utils::Error> NormalRepresentation::BeginSparseValues(
    const encoding::Sparse& source_sparse_encoding) {
  auto downgraded = MaybeDowngrade(source_sparse_encoding.normal(),
                                   source_sparse_encoding.sparse_precision());
  if (!downgraded.has_value()) return downgraded;
  EnsureDataMut();
  return {};
}

std::expected<void, utils::Error> NormalRepresentation::AddHash(uint64_t hash) {
  const uint32_t idx = encoding_.Index(hash);
  const uint8_t rho_w = encoding_.RhoW(hash);

  auto& data = EnsureDataMut();
  if (idx < data.size()) {
    PutMax(data[idx], rho_w);
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
  // A present but empty register array carries no registers, so the
  // reference reports nothing from it rather than dividing by an empty
  // sum. Its guard tests the contents, not the presence of the field.
  if (!state_.data.has_value() || state_.data->empty()) {
    return 0;
  }
  const auto& data = *state_.data;

  const int32_t num_zeros =
      static_cast<int32_t>(std::ranges::count(data, uint8_t{0}));
  const double sum = std::transform_reduce(
      data.begin(), data.end(), 0.0, std::plus<>(), [](uint8_t v_byte) {
        // The reference shifts a signed long by the register, which its
        // language reduces modulo the width; a register at or above 64
        // therefore selects a shift of that value's low six bits, and a
        // shift of 63 yields the most negative long rather than a
        // positive power. Reproducing both keeps the sum identical for
        // register arrays the reference accepts but never writes.
        const auto power =
            static_cast<int64_t>(uint64_t{1} << (static_cast<uint32_t>(v_byte) &
                                                 kRegisterShiftMask));
        return 1.0 / static_cast<double>(power);
      });

  auto m = static_cast<double>(uint64_t{1}
                               << static_cast<uint32_t>(state_.precision));

  if (num_zeros > 0) {
    auto linear_count_threshold =
        static_cast<double>(LinearCountingThreshold(state_.precision));
    const double h = m * std::log(m / static_cast<double>(num_zeros));
    if (h <= linear_count_threshold) {
      return RoundAsTheReferenceDoes(h);
    }
  }

  const double raw_estimate = Alpha(state_.precision) * m * m / sum;
  const double bias_correction = EstimateBias(raw_estimate, state_.precision);

  return RoundAsTheReferenceDoes(raw_estimate - bias_correction);
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
    auto& data_slice = EnsureData(state);
    // The reference copies the source over the target from offset zero
    // and bounds only the source's length, so a source holding no bytes
    // merges nothing and leaves a newly materialised target behind.
    if (source_data.size() > data_slice.size()) {
      return std::unexpected(utils::Error{
          .code = utils::ErrorCode::kInvalidState,
          .message = "Mismatched data lengths in "
                     "MergeNormalDataMaybeDowngrading for same precision"});
    }
    for (size_t i = 0; i < source_data.size(); ++i) {
      PutMax(data_slice[i], source_data[i]);
    }
    return {};
  }

  auto& target_array = EnsureData(state);

  for (size_t old_index = 0; old_index < source_data.size(); ++old_index) {
    const uint8_t old_rho_w = source_data[old_index];
    auto new_index = static_cast<size_t>(source_encoding.DowngradeIndex(
        static_cast<uint32_t>(old_index), target_encoding.precision()));
    const uint8_t new_rho_w =
        source_encoding.DowngradeRhoW(static_cast<uint32_t>(old_index),
                                      old_rho_w, target_encoding.precision());

    if (new_index < target_array.size()) {
      PutMax(target_array[new_index], new_rho_w);
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
    PutMax(data[idx], rho_w);
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
