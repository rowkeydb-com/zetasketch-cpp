// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hyperloglogplusplus.h"
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "aggregator.pb.h"
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/sparse_representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/error.h"
#include "src/farmhash/fingerprint2011.h"

namespace zetasketch {
namespace {

// The encoding version the reference will read. It records 1 for a
// state that carries none, and refuses everything but 2.
constexpr int32_t kSupportedEncodingVersion = 2;

// An addition's type, as the reference names it, together with the
// value type it records for a sketch that has recorded none.
struct AdditionType {
  const char* name;
  hll::ValueType value_type;
};

constexpr AdditionType kStringAddition{
    .name = "STRING", .value_type = hll::ValueType::kBytesOrUtf8String};
constexpr AdditionType kLongAddition{
    .name = "LONG", .value_type = hll::ValueType::kUnsignedInt64};
// Names the set of additions a recorded value type admits, in the
// reference's own notation. A sketch that has recorded none admits any
// of them; one that has recorded the type used for text admits both
// the string and the byte array that share it.
const char* DescribeAdmittedTypes(hll::ValueType recorded) {
  switch (recorded) {
    case hll::ValueType::kUnsignedInt32:
      return "[INTEGER]";
    case hll::ValueType::kUnsignedInt64:
      return "[LONG]";
    case hll::ValueType::kBytesOrUtf8String:
      return "[STRING, BYTES]";
    case hll::ValueType::kUnknown:
      return "[LONG, INTEGER, STRING, BYTES]";
  }
  return "[]";
}

// Reproduces the reference's narrowing of the additions a sketch will
// accept. A sketch that has recorded no value type takes the type of
// its first addition and writes it out thereafter, which is why a
// sketch parsed without one does not stay without one.
std::expected<void, utils::Error> RecordAddedType(
    hll::State& state, const AdditionType& addition) {
  if (state.value_type != hll::ValueType::kUnknown &&
      state.value_type != addition.value_type) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message = std::format("unable to add type {} to aggregator of type {}",
                               addition.name,
                               DescribeAdmittedTypes(state.value_type))});
  }
  state.value_type = addition.value_type;
  return {};
}

// A value type the reference's own enumeration names, and the name it
// prints when it refuses one. Only the types it refuses appear here;
// the four it accepts are never named in a refusal.
struct NamedValueType {
  int32_t number;
  const char* name;
};

constexpr std::array<NamedValueType, 8> kNamedValueTypes = {{
    {.number = 1, .name = "DefaultOpsType.Id.INT8"},
    {.number = 2, .name = "DefaultOpsType.Id.INT16"},
    {.number = 3, .name = "DefaultOpsType.Id.INT32"},
    {.number = 4, .name = "DefaultOpsType.Id.INT64"},
    {.number = 5, .name = "DefaultOpsType.Id.UINT8"},
    {.number = 6, .name = "DefaultOpsType.Id.UINT16"},
    {.number = 9, .name = "DefaultOpsType.Id.FLOAT"},
    {.number = 10, .name = "DefaultOpsType.Id.DOUBLE"},
}};

// Names a value type the reference refuses, as it names it. Anything
// outside its enumeration is reported as a custom type carrying its
// number.
std::string DescribeValueType(int32_t value_type) {
  for (const NamedValueType& named : kNamedValueTypes) {
    if (named.number == value_type) {
      return named.name;
    }
  }
  return std::format("<unnamed custom value type {}>", value_type);
}

}  // namespace

std::expected<HyperLogLogPlusPlus, utils::Error> HyperLogLogPlusPlus::Create(
    int32_t normal_precision, int32_t sparse_precision) {
  hll::State state;
  state.type = HYPERLOGLOG_PLUS_UNIQUE;
  state.encoding_version = 2;
  state.value_type = hll::ValueType::kBytesOrUtf8String;
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

hll::State& HyperLogLogPlusPlus::MutableState() {
  return std::visit(
      [](auto& representation) -> hll::State& {
        return representation.state();
      },
      representation_);
}

std::expected<void, utils::Error> HyperLogLogPlusPlus::Add(
    std::string_view value) {
  auto admitted = RecordAddedType(MutableState(), kStringAddition);
  if (!admitted.has_value()) return admitted;
  const uint64_t hash = Fingerprint2011(value.data(), value.size());
  return AddHash(hash);
}

std::expected<void, utils::Error> HyperLogLogPlusPlus::Add(int64_t value) {
  // Which additions a sketch admits, and the value type it records for
  // one, are decided by the state alone and need no hash, so they are
  // reproduced here. The reference distinguishes a 32-bit integer from
  // a 64-bit one and records a different value type for each; this
  // library offers only the wider addition, and the narrower one
  // arrives with the integer hash below, which is still a placeholder.
  auto admitted = RecordAddedType(MutableState(), kLongAddition);
  if (!admitted.has_value()) return admitted;
  // Placeholder hash; the reference integer hash replaces it when the
  // integer path is implemented against the Java library.
  constexpr uint64_t kDummyHashMultiplier = 0x9E3779B97F4A7C15ULL;
  return AddHash(static_cast<uint64_t>(value) * kDummyHashMultiplier);
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
  // The reference imposes no precision check here. A merge lowers
  // whichever side is higher, the normal representation through its own
  // downgrade and the sparse one through the encoding's, and only a
  // pair of encodings that is unordered in both precisions is refused,
  // by the encoding itself.
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
          auto res = other_rep_ref.MergeInto(self_rep_ref);
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

std::expected<int64_t, utils::Error> HyperLogLogPlusPlus::Result() const {
  if (std::holds_alternative<hll::NormalRepresentation>(representation_)) {
    return std::get<hll::NormalRepresentation>(representation_).Estimate();
  }
  return std::get<hll::SparseRepresentation>(representation_).Estimate();
}

std::expected<HyperLogLogPlusPlus, utils::Error> HyperLogLogPlusPlus::FromBytes(
    std::string_view data) {
  // We employ reinterpret_cast to bridge the boundary between the internal
  // unsigned 8-bit integer type and the external character type. The standard
  // prohibits static_cast between these pointer types. Direct conversion
  // satisfies the constraint against temporary allocations on this execution
  // path. The [basic.lval] clause of the C++ standard exempts character types
  // from strict aliasing constraints, rendering this cast safe.
  auto span = std::span<const uint8_t>(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<const uint8_t*>(data.data()), data.size());
  return FromBytes(span);
}

std::expected<HyperLogLogPlusPlus, utils::Error> HyperLogLogPlusPlus::FromBytes(
    std::span<const uint8_t> data) {
  auto state_result = hll::State::Parse(data);
  if (!state_result.has_value()) {
    return std::unexpected(state_result.error());
  }
  const auto& state = state_result.value();

  // The reference chooses the representation before it validates, and
  // the choice decides which precisions are inspected: a sketch
  // carrying dense data, or none at all with sparse mode disabled,
  // becomes a normal representation, whose construction never looks at
  // the sparse precision. Choosing differently would refuse sketches
  // the reference accepts, or accept ones it refuses.
  // The reference checks the aggregator type and the encoding version
  // before it looks at anything else, then the value type, and only
  // then chooses a representation. A state of another aggregator's
  // type, or of the encoding version that preceded this one, is
  // refused rather than reinterpreted under this one's rules.
  if (state.type != HYPERLOGLOG_PLUS_UNIQUE) {
    const std::string type_name =
        state.type.has_value() ? std::string(AggregatorType_Name(*state.type))
                               : std::string("null");
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message = std::format(
            "Expected proto to be of type HYPERLOGLOG_PLUS_UNIQUE but was {}",
            type_name)});
  }

  if (state.encoding_version != kSupportedEncodingVersion) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message =
            std::format("Expected encoding version to be {} but was {}",
                        kSupportedEncodingVersion, state.encoding_version)});
  }

  if (state.value_type != hll::ValueType::kUnknown &&
      state.value_type != hll::ValueType::kUnsignedInt32 &&
      state.value_type != hll::ValueType::kUnsignedInt64 &&
      state.value_type != hll::ValueType::kBytesOrUtf8String) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message = std::format(
            "Unsupported value type {}",
            DescribeValueType(static_cast<int32_t>(state.value_type)))});
  }

  // The reference distinguishes a field that is present from a field
  // that holds bytes. Both of its selection predicates require at
  // least one byte, so a sketch whose data field is present but empty
  // is selected as a sparse sketch, and an empty sparse data field
  // beside no sparse precision is accepted rather than refused.
  const bool has_data = state.data.has_value() && !state.data->empty();
  const bool has_sparse_data =
      state.sparse_data.has_value() && !state.sparse_data->empty();

  if (has_sparse_data && state.sparse_precision == kSparsePrecisionDisabled) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message = "Must have a sparse precision when sparse data is set"});
  }

  if (!has_data && state.sparse_precision != kSparsePrecisionDisabled) {
    auto sparse_result = hll::SparseRepresentation::Create(state);
    if (!sparse_result.has_value()) {
      return std::unexpected(sparse_result.error());
    }
    return HyperLogLogPlusPlus(std::move(sparse_result.value()));
  }

  auto rep_result = hll::NormalRepresentation::Create(state);
  if (!rep_result.has_value()) {
    return std::unexpected(rep_result.error());
  }
  return HyperLogLogPlusPlus(std::move(rep_result.value()));
}

std::expected<hll::State, utils::Error>
HyperLogLogPlusPlus::GetStateForSerialization() {
  // The reference assigns the compacted representation back to itself
  // before it writes, so a sparse sketch that compaction promotes stays
  // promoted and a later estimate uses the dense estimator. Compacting
  // a copy would leave the sketch sparse and estimate it differently
  // afterwards, while writing identical bytes.
  if (auto* sparse = std::get_if<hll::SparseRepresentation>(&representation_)) {
    auto compact_res = std::move(*sparse).Compact();
    if (!compact_res.has_value()) {
      return std::unexpected(compact_res.error());
    }
    representation_ = std::move(compact_res.value());
  }

  hll::State state = std::visit(
      [](const auto& representation) { return representation.state(); },
      representation_);
  state.type = zetasketch::HYPERLOGLOG_PLUS_UNIQUE;
  return state;
}

std::expected<std::vector<uint8_t>, utils::Error>
HyperLogLogPlusPlus::Serialize() {
  auto state_res = GetStateForSerialization();
  if (!state_res.has_value()) return std::unexpected(state_res.error());
  return state_res.value().ToByteArray();
}

std::expected<void, utils::Error> HyperLogLogPlusPlus::Serialize(
    std::vector<uint8_t>& sink) {
  auto bytes_result = Serialize();
  if (!bytes_result.has_value()) {
    return std::unexpected(bytes_result.error());
  }

  sink = std::move(bytes_result.value());
  return {};
}

std::expected<void, utils::Error> HyperLogLogPlusPlus::Serialize(
    std::string& sink) {
  auto state_res = GetStateForSerialization();
  if (!state_res.has_value()) return std::unexpected(state_res.error());
  return state_res.value().ToByteArray(&sink);
}

}  // namespace zetasketch
