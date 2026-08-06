// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hyperloglogplusplus.h"
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "aggregator.pb.h"
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/sparse_representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/buffer_traits.h"

namespace zetasketch {

std::expected<HyperLogLogPlusPlus, utils::Error> HyperLogLogPlusPlus::Create(
    int32_t normal_precision, int32_t sparse_precision) {
  hll::State state;
  state.type = HYPERLOGLOG_PLUS_UNIQUE;
  state.precision = normal_precision;
  state.sparse_precision = sparse_precision;

  if (sparse_precision == kSparsePrecisionDisabled) {
    auto rep_res = hll::NormalRepresentation::Create(std::move(state));
    if (!rep_res.has_value()) return std::unexpected(rep_res.error());
    return HyperLogLogPlusPlus(std::move(rep_res.value()));
  }

  auto rep_res = hll::SparseRepresentation::Create(std::move(state));
  if (!rep_res.has_value()) return std::unexpected(rep_res.error());
  return HyperLogLogPlusPlus(std::move(rep_res.value()));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void HyperLogLogPlusPlus::Add(std::string_view value) {
  (void)value;
  // Stub for now. Hashing is usually done via FarmHash.
  // Not strictly needed to test serialization round-tripping if we use
  // Add(int64_t).
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void HyperLogLogPlusPlus::Add(int64_t value) {
  // Simple stub/dummy hash for testing serialization round-trip without
  // farmhash dep here Real implementation will use farmhash and handle types
  // properly.
  constexpr uint64_t kDummyHashMultiplier = 0x9E3779B97F4A7C15ULL;
  auto res = AddHash(static_cast<uint64_t>(value) * kDummyHashMultiplier);
  if (!res.has_value()) {
    // Ignore in stub
  }
}

std::expected<void, utils::Error> HyperLogLogPlusPlus::AddHash(uint64_t hash) {
  if (std::holds_alternative<hll::NormalRepresentation>(representation_)) {
    auto res =
        std::get<hll::NormalRepresentation>(representation_).AddHash(hash);
    if (!res.has_value()) return std::unexpected(res.error());
    std::get<hll::NormalRepresentation>(representation_).state().num_values++;
  } else {
    auto res = std::move(std::get<hll::SparseRepresentation>(representation_))
                   .AddHash(hash);
    if (!res.has_value()) return std::unexpected(res.error());
    representation_ = std::move(res.value());

    if (std::holds_alternative<hll::NormalRepresentation>(representation_)) {
      std::get<hll::NormalRepresentation>(representation_).state().num_values++;
    } else {
      std::get<hll::SparseRepresentation>(representation_).state().num_values++;
    }
  }
  return {};
}

std::expected<void, utils::Error> HyperLogLogPlusPlus::Merge(
    HyperLogLogPlusPlus&& other) {
  int32_t self_precision = 0;
  if (std::holds_alternative<hll::NormalRepresentation>(representation_)) {
    self_precision =
        std::get<hll::NormalRepresentation>(representation_).state().precision;
  } else {
    self_precision =
        std::get<hll::SparseRepresentation>(representation_).state().precision;
  }

  int32_t other_precision = 0;
  if (std::holds_alternative<hll::NormalRepresentation>(
          other.representation_)) {
    other_precision = std::get<hll::NormalRepresentation>(other.representation_)
                          .state()
                          .precision;
  } else {
    other_precision = std::get<hll::SparseRepresentation>(other.representation_)
                          .state()
                          .precision;
  }

  if (self_precision != other_precision) {
    return std::unexpected(
        utils::Error{.code = utils::ErrorCode::kIncompatiblePrecision,
                     .message = "Precision mismatch"});
  }

  int64_t other_count = 0;
  if (std::holds_alternative<hll::NormalRepresentation>(
          other.representation_)) {
    other_count = std::get<hll::NormalRepresentation>(other.representation_)
                      .state()
                      .num_values;
  } else {
    other_count = std::get<hll::SparseRepresentation>(other.representation_)
                      .state()
                      .num_values;
  }

  auto result = std::visit(
      [](auto& self_rep_ref, auto& other_rep_ref)
          -> std::expected<std::optional<hll::Representation>, utils::Error> {
        using SelfType = std::decay_t<decltype(self_rep_ref)>;
        using OtherType = std::decay_t<decltype(other_rep_ref)>;

        if constexpr (std::is_same_v<SelfType, hll::NormalRepresentation> &&
                      std::is_same_v<OtherType, hll::NormalRepresentation>) {
          auto res = self_rep_ref.MergeFromNormal(std::move(other_rep_ref));
          if (!res.has_value()) return std::unexpected(res.error());
          return std::nullopt;
        } else if constexpr (std::is_same_v<SelfType,
                                            hll::SparseRepresentation> &&
                             std::is_same_v<OtherType,
                                            hll::SparseRepresentation>) {
          auto res = std::move(self_rep_ref).MergeFromSparse(other_rep_ref);
          if (!res.has_value()) return std::unexpected(res.error());
          return std::move(res.value());
        } else if constexpr (std::is_same_v<SelfType,
                                            hll::NormalRepresentation> &&
                             std::is_same_v<OtherType,
                                            hll::SparseRepresentation>) {
          auto norm_other_res = std::move(other_rep_ref).Normalize();
          if (!norm_other_res.has_value()) {
            return std::unexpected(norm_other_res.error());
          }
          auto& norm_other = norm_other_res.value();
          if (!std::holds_alternative<hll::NormalRepresentation>(norm_other)) {
            return std::unexpected(
                utils::Error{.code = utils::ErrorCode::kInvalidState,
                             .message = "Sparse normalization did not return "
                                        "NormalRepresentation"});
          }
          auto res = self_rep_ref.MergeFromNormal(
              std::get<hll::NormalRepresentation>(std::move(norm_other)));
          if (!res.has_value()) return std::unexpected(res.error());
          return std::nullopt;
        } else if constexpr (std::is_same_v<SelfType,
                                            hll::SparseRepresentation> &&
                             std::is_same_v<OtherType,
                                            hll::NormalRepresentation>) {
          auto self_norm_res = std::move(self_rep_ref).Normalize();
          if (!self_norm_res.has_value()) {
            return std::unexpected(self_norm_res.error());
          }
          auto& self_norm = self_norm_res.value();
          if (std::holds_alternative<hll::NormalRepresentation>(self_norm)) {
            auto res =
                std::get<hll::NormalRepresentation>(self_norm).MergeFromNormal(
                    std::move(other_rep_ref));
            if (!res.has_value()) return std::unexpected(res.error());
            return std::move(self_norm);
          }
          return std::unexpected(utils::Error{
              .code = utils::ErrorCode::kInvalidState,
              .message =
                  "Sparse normalization did not return NormalRepresentation"});
        }
      },
      representation_, other.representation_);

  if (!result.has_value()) {
    return std::unexpected(result.error());
  }

  if (result.value().has_value()) {
    representation_ = std::move(result.value().value());
  }
  if (std::holds_alternative<hll::NormalRepresentation>(representation_)) {
    std::get<hll::NormalRepresentation>(representation_).state().num_values +=
        other_count;
  } else {
    std::get<hll::SparseRepresentation>(representation_).state().num_values +=
        other_count;
  }

  (void)std::move(other);
  return {};
}

int64_t HyperLogLogPlusPlus::Result() const {
  if (std::holds_alternative<hll::NormalRepresentation>(representation_)) {
    auto res = std::get<hll::NormalRepresentation>(representation_).Estimate();
    return res.value_or(0);
  }

  auto res = std::get<hll::SparseRepresentation>(representation_).Estimate();
  return res.value_or(0);
}

std::expected<HyperLogLogPlusPlus, utils::Error> HyperLogLogPlusPlus::FromBytes(
    std::span<const uint8_t> data) {
  auto state_result = hll::State::Parse(data);
  if (!state_result.has_value()) {
    return std::unexpected(state_result.error());
  }
  const auto& state = state_result.value();

  if (state.sparse_data.has_value()) {
    auto rep_result = hll::SparseRepresentation::Create(state);
    if (!rep_result.has_value()) {
      return std::unexpected(rep_result.error());
    }
    return HyperLogLogPlusPlus(std::move(rep_result.value()));
  }

  auto rep_result = hll::NormalRepresentation::Create(state);
  if (!rep_result.has_value()) {
    return std::unexpected(rep_result.error());
  }
  return HyperLogLogPlusPlus(std::move(rep_result.value()));
}

std::expected<std::vector<uint8_t>, utils::Error>
HyperLogLogPlusPlus::Serialize() const {
  hll::State state;

  if (std::holds_alternative<hll::NormalRepresentation>(representation_)) {
    state = std::get<hll::NormalRepresentation>(representation_).state();
  } else {
    hll::SparseRepresentation copy =
        std::get<hll::SparseRepresentation>(representation_);
    auto compact_res = std::move(copy).Compact();
    if (!compact_res.has_value()) {
      return std::unexpected(compact_res.error());
    }

    if (std::holds_alternative<hll::NormalRepresentation>(
            compact_res.value())) {
      state = std::get<hll::NormalRepresentation>(compact_res.value()).state();
    } else {
      state = std::get<hll::SparseRepresentation>(compact_res.value()).state();
    }
  }

  state.type = zetasketch::HYPERLOGLOG_PLUS_UNIQUE;
  return state.ToByteArray();
}

std::expected<void, utils::Error> HyperLogLogPlusPlus::Serialize(
    std::vector<uint8_t>& sink) const {
  auto bytes_result = Serialize();
  if (!bytes_result.has_value()) {
    return std::unexpected(bytes_result.error());
  }

  sink = std::move(bytes_result.value());
  return {};
}

}  // namespace zetasketch
