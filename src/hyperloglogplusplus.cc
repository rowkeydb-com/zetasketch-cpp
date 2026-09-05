// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hyperloglogplusplus.h"
#include <algorithm>
#include <array>
#include <bit>
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
#include "zetasketch/hll/encoding.h"
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/representation.h"
#include "zetasketch/hll/sparse_representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/error.h"
#include "zetasketch/utils/iterators.h"
#include "src/farmhash/fingerprint2011.h"

namespace zetasketch {
namespace {

// The encoding version the reference will read. It records 1 for a
// state that carries none, and refuses everything but 2.
constexpr int32_t kSupportedEncodingVersion = 2;

// The kinds of addition the reference distinguishes, one bit each, in
// the order its enumeration declares them, which is the order it prints
// a set of them in.
using KindSet = uint32_t;
constexpr KindSet kLongKind = 1U;
constexpr KindSet kIntegerKind = 2U;
constexpr KindSet kStringKind = 4U;
constexpr KindSet kBytesKind = 8U;
constexpr KindSet kEveryKind =
    kLongKind | kIntegerKind | kStringKind | kBytesKind;

struct KindName {
  KindSet kind;
  const char* name;
};

constexpr std::array<KindName, 4> kKindNames = {{
    {.kind = kLongKind, .name = "LONG"},
    {.kind = kIntegerKind, .name = "INTEGER"},
    {.kind = kStringKind, .name = "STRING"},
    {.kind = kBytesKind, .name = "BYTES"},
}};

// An addition's kind, as the reference names it, together with the
// value type it records for a sketch whose set it narrows.
struct AdditionType {
  const char* name;
  KindSet kind;
  hll::ValueType value_type;
};

constexpr AdditionType kStringAddition{
    .name = "STRING",
    .kind = kStringKind,
    .value_type = hll::ValueType::kBytesOrUtf8String};
constexpr AdditionType kLongAddition{
    .name = "LONG",
    .kind = kLongKind,
    .value_type = hll::ValueType::kUnsignedInt64};

// The kinds a recorded value type admits, as the reference derives them
// when it builds or reads a sketch: everything for none, and for the
// type text and byte arrays share, both of those.
KindSet AdmittedKinds(hll::ValueType value_type) {
  switch (value_type) {
    case hll::ValueType::kUnknown:
      return kEveryKind;
    case hll::ValueType::kUnsignedInt64:
      return kLongKind;
    case hll::ValueType::kUnsignedInt32:
      return kIntegerKind;
    case hll::ValueType::kBytesOrUtf8String:
      return kStringKind | kBytesKind;
  }
  return 0;
}

KindSet AdmittedKinds(const hll::Representation& representation) {
  return AdmittedKinds(
      std::visit([](const auto& current) { return current.state().value_type; },
                 representation));
}

// Names a set of kinds as the reference prints one: bracketed, in
// declaration order, separated by a comma and a space.
std::string DescribeKinds(KindSet kinds) {
  std::string described = "[";
  for (const KindName& named : kKindNames) {
    if ((kinds & named.kind) == 0) continue;
    if (described.size() > 1) described += ", ";
    described += named.name;
  }
  described += "]";
  return described;
}

// Reproduces the reference's check on an addition and its narrowing of
// the set. An addition outside the set is refused. One inside a set of
// more than one kind narrows it to that kind and records the kind's
// value type, which is why a sketch read without one does not stay
// without one. An addition to a set already of one kind records
// nothing: a sketch that admits one kind only because a merge narrowed
// it to that never writes a value type, and the reference does not
// either.
std::expected<void, utils::Error> RecordAddedType(
    hll::State& state, KindSet& admitted, const AdditionType& addition) {
  if ((admitted & addition.kind) == 0) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message = std::format("unable to add type {} to aggregator of type {}",
                               addition.name, DescribeKinds(admitted))});
  }
  if (std::popcount(admitted) > 1) {
    admitted = addition.kind;
    state.value_type = addition.value_type;
  }
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

// The checks the reference makes on a state before it chooses a
// representation, in its order and in its words: the aggregator type,
// then the encoding version, then the value type.
std::expected<void, utils::Error> CheckHeader(const hll::State& state) {
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
  if (AdmittedKinds(state.value_type) == 0) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message = std::format(
            "Unsupported value type {}",
            DescribeValueType(static_cast<int32_t>(state.value_type)))});
  }
  return {};
}

// Checks that no register is above the largest a hash can produce,
// which is the number of bits below the index plus one. The reference
// asserts the same bound on every register it reads, in builds that
// keep assertions. The array's length was checked when it was read.
std::expected<void, utils::Error> ValidateRegisters(const hll::State& state) {
  if (!state.data.has_value() || state.data->empty()) return {};
  const std::vector<uint8_t>& registers = *state.data;
  // The bound is at most 61, so it fits the register's own type.
  const auto largest =
      static_cast<uint8_t>(hll::encoding::kHashBits - state.precision + 1);
  const auto above = std::ranges::find_if(
      registers, [largest](uint8_t reg) { return reg > largest; });
  if (above != registers.end()) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kInvalidState,
        .message = std::format("register {} at index {} exceeds {}, the "
                               "largest a hash can produce at precision {}",
                               *above, above - registers.begin(), largest,
                               state.precision)});
  }
  return {};
}

// Decodes the whole of a sparse stream and checks each value against
// the encoding: it must increase on the one before it, name a sparse
// index the stream has not named, and lie within both precisions; and
// the stream must hold as many values as the recorded sparse size says.
// Every stream the reference writes satisfies all of this, since its
// encoder refuses a value that does not increase and its flush keeps
// one value per index.
std::expected<void, utils::Error> ValidateSparseStream(
    const hll::State& state, const hll::encoding::Sparse& encoding) {
  int32_t count = 0;
  if (state.sparse_data.has_value() && !state.sparse_data->empty()) {
    const auto sparse_precision = static_cast<uint32_t>(state.sparse_precision);
    const auto normal_precision = static_cast<uint32_t>(state.precision);
    const uint32_t sparse_buckets = 1U << sparse_precision;
    const uint32_t normal_buckets = 1U << normal_precision;
    const uint32_t flag_shift =
        std::max(sparse_precision,
                 normal_precision +
                     static_cast<uint32_t>(hll::encoding::Sparse::kRhoWBits));
    const uint32_t flag = 1U << flag_shift;
    const uint32_t widest = (flag << 1U) - 1U;

    utils::DifferenceDecoder decoder(*state.sparse_data);
    std::optional<uint32_t> previous;
    while (auto next = decoder.Next()) {
      const uint32_t value = *next;
      if (previous.has_value() && value <= *previous) {
        return std::unexpected(utils::Error{
            .code = utils::ErrorCode::kInvalidState,
            .message = std::format("sparse value {} at position {} does not "
                                   "increase on the value {} before it",
                                   value, count, *previous)});
      }
      if ((value & flag) == 0) {
        if (value >= sparse_buckets) {
          return std::unexpected(utils::Error{
              .code = utils::ErrorCode::kInvalidState,
              .message = std::format("sparse index {} at position {} is out "
                                     "of range for sparse precision {}",
                                     value, count, state.sparse_precision)});
        }
      } else {
        if (value > widest) {
          return std::unexpected(utils::Error{
              .code = utils::ErrorCode::kInvalidState,
              .message = std::format("sparse value {} at position {} has bits "
                                     "above the encoding for precisions "
                                     "({}, {})",
                                     value, count, state.precision,
                                     state.sparse_precision)});
        }
        if (encoding.DecodeNormalIndex(value) >= normal_buckets) {
          return std::unexpected(utils::Error{
              .code = utils::ErrorCode::kInvalidState,
              .message = std::format("normal index {} at position {} is out "
                                     "of range for normal precision {}",
                                     encoding.DecodeNormalIndex(value), count,
                                     state.precision)});
        }
      }
      if (previous.has_value() && encoding.DecodeSparseIndex(value) ==
                                      encoding.DecodeSparseIndex(*previous)) {
        return std::unexpected(utils::Error{
            .code = utils::ErrorCode::kInvalidState,
            .message =
                std::format("sparse value {} at position {} repeats "
                            "the sparse index {} of the value before it",
                            value, count, encoding.DecodeSparseIndex(value))});
      }
      previous = value;
      ++count;
    }
    if (decoder.error().has_value()) {
      return std::unexpected(*decoder.error());
    }
  }
  if (count != state.sparse_size) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kInvalidState,
        .message = std::format("sparse size {} is recorded but the sparse "
                               "stream holds {} values",
                               state.sparse_size, count)});
  }
  return {};
}

}  // namespace

std::expected<HyperLogLogPlusPlus, utils::Error> HyperLogLogPlusPlus::Create(
    int32_t normal_precision, int32_t sparse_precision,
    hll::ValueType value_type) {
  // The same value types the reference will read are the ones it will
  // build for, so a sketch cannot be constructed carrying one it would
  // refuse to parse back.
  if (value_type != hll::ValueType::kUnknown &&
      value_type != hll::ValueType::kUnsignedInt32 &&
      value_type != hll::ValueType::kUnsignedInt64 &&
      value_type != hll::ValueType::kBytesOrUtf8String) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message =
            std::format("Unsupported value type {}",
                        DescribeValueType(static_cast<int32_t>(value_type)))});
  }

  hll::State state;
  state.type = HYPERLOGLOG_PLUS_UNIQUE;
  state.encoding_version = 2;
  state.value_type = value_type;
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

std::expected<HyperLogLogPlusPlus, utils::Error> HyperLogLogPlusPlus::Create(
    int32_t normal_precision, hll::ValueType value_type) {
  // The sum is taken in a wider type so that a normal precision near the
  // top of its own type, which the check below refuses anyway, cannot
  // overflow on the way there.
  const auto default_sparse_precision = static_cast<int32_t>(std::min<int64_t>(
      int64_t{normal_precision} + kDefaultSparsePrecisionDelta,
      kMaximumSparsePrecision));
  return Create(normal_precision, default_sparse_precision, value_type);
}

std::expected<HyperLogLogPlusPlus, utils::Error> HyperLogLogPlusPlus::Create() {
  return Create(kDefaultNormalPrecision);
}

HyperLogLogPlusPlus::HyperLogLogPlusPlus(hll::Representation representation)
    : representation_(std::move(representation)),
      admitted_kinds_(AdmittedKinds(representation_)) {}

hll::State& HyperLogLogPlusPlus::MutableState() {
  return std::visit(
      [](auto& representation) -> hll::State& {
        return representation.state();
      },
      representation_);
}

std::expected<void, utils::Error> HyperLogLogPlusPlus::Add(
    std::string_view value) {
  auto admitted =
      RecordAddedType(MutableState(), admitted_kinds_, kStringAddition);
  if (!admitted.has_value()) return admitted;
  const uint64_t hash = Fingerprint2011(value.data(), value.size());
  return AddHash(hash);
}

std::expected<void, utils::Error> HyperLogLogPlusPlus::Add(int64_t value) {
  auto admitted =
      RecordAddedType(MutableState(), admitted_kinds_, kLongAddition);
  if (!admitted.has_value()) return admitted;

  // The reference hashes an integer by writing it as eight bytes, the
  // least significant first, and fingerprinting those bytes with the
  // same function it uses for text. Writing them in the other order,
  // or in fewer than eight, would hash a different value and every
  // sketch built from integers would differ from the reference's.
  //
  // One store puts them there on a host that is already little-endian,
  // and the bytes are reversed first on one that is not. Written a byte
  // at a time this was eight dependent stores that the eight-byte load
  // in the hash then had to wait on, which cost more than the hash
  // itself on a path that runs once per value added.
  static_assert(std::endian::native == std::endian::little ||
                    std::endian::native == std::endian::big,
                "the integer encoding is defined for little- and big-endian "
                "hosts only");
  auto bits = static_cast<uint64_t>(value);
  if constexpr (std::endian::native == std::endian::big) {
    bits = std::byteswap(bits);
  }
  const auto encoded = std::bit_cast<std::array<char, sizeof(bits)>>(bits);
  return AddHash(Fingerprint2011(encoded.data(), encoded.size()));
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
  // The reference intersects the two admitted sets before it touches
  // either representation, refuses an empty intersection, and keeps a
  // non-empty one whether or not the representations then merge. The
  // recorded value type is left alone: a merge never writes it.
  const KindSet common = admitted_kinds_ & other.admitted_kinds_;
  if (common == 0) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message = std::format(
            "Aggregator of type {} is incompatible with aggregator of type {}",
            DescribeKinds(admitted_kinds_),
            DescribeKinds(other.admitted_kinds_))});
  }
  admitted_kinds_ = common;

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

std::expected<void, utils::Error> HyperLogLogPlusPlus::Validate() const {
  // The header, the precisions and the register array's length were
  // checked when the sketch was built or read; nothing constructs one
  // that fails them. What remains unread until here is the contents: a
  // dense sketch's registers, or a sparse sketch's stream. A dense
  // sketch's sparse fields are carried but never read, by either
  // library, so they are not walked.
  if (const auto* sparse =
          std::get_if<hll::SparseRepresentation>(&representation_)) {
    return ValidateSparseStream(sparse->state(), sparse->encoding());
  }
  return ValidateRegisters(
      std::get<hll::NormalRepresentation>(representation_).state());
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
  auto header = CheckHeader(state);
  if (!header.has_value()) return std::unexpected(header.error());

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
