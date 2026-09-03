// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/state.h"
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>
#include "aggregator.pb.h"
#include "hllplusplus.pb.h"
#include "zetasketch/utils/error.h"

namespace zetasketch::hll {
namespace {

// The aggregator type's field number in the enclosing message.
constexpr int kAggregatorTypeFieldNumber = 1;

// Returns the number carried by the last aggregator type field in the
// message, ignoring a field of that number carrying anything but a
// variable-length integer, and returning nothing when the message
// carries no such field.
std::optional<int32_t> LastAggregatorTypeNumber(
    std::span<const uint8_t> input) {
  google::protobuf::io::CodedInputStream stream(input.data(),
                                                static_cast<int>(input.size()));
  std::optional<int32_t> last;
  for (uint32_t tag = stream.ReadTag(); tag != 0; tag = stream.ReadTag()) {
    using google::protobuf::internal::WireFormatLite;
    if (WireFormatLite::GetTagFieldNumber(tag) == kAggregatorTypeFieldNumber &&
        WireFormatLite::GetTagWireType(tag) ==
            WireFormatLite::WIRETYPE_VARINT) {
      uint64_t value = 0;
      if (!stream.ReadVarint64(&value)) {
        return last;
      }
      // The reference reads this field as a 32-bit enumeration, so a
      // wider number is truncated rather than rejected.
      last = static_cast<int32_t>(static_cast<uint32_t>(value));
      continue;
    }
    if (!WireFormatLite::SkipField(&stream, tag)) {
      return last;
    }
  }
  return last;
}

}  // namespace

std::expected<State, utils::Error> State::Parse(
    std::span<const uint8_t> input) {
  zetasketch::AggregatorStateProto proto;
  // The reference reads this message with a parser it wrote by hand,
  // which enforces no required field: it accepts a sketch that carries
  // neither an aggregator type nor a value count, and supplies its own
  // defaults for both. Parsing partially reproduces that, and the
  // defaults below are the reference's own.
  if (!proto.ParsePartialFromArray(input.data(),
                                   static_cast<int>(input.size()))) {
    return std::unexpected(
        utils::Error{.code = utils::ErrorCode::kProtoDeserialization,
                     .message = "Failed to parse state"});
  }

  State state;
  // The aggregator type is read from the wire rather than from the
  // parsed message. A generated parser discards a number it does not
  // recognise into the unknown fields, which loses both the number and
  // its position, where the reference keeps the last field it reads,
  // recognised or not. A field of any other wire type is skipped, as
  // the reference skips it.
  if (const auto number = LastAggregatorTypeNumber(input)) {
    state.type = zetasketch::AggregatorType_IsValid(*number)
                     ? std::optional<zetasketch::AggregatorType>(
                           static_cast<zetasketch::AggregatorType>(*number))
                     : std::nullopt;
  }
  if (proto.has_num_values()) {
    state.num_values = proto.num_values();
  }
  if (proto.has_encoding_version()) {
    state.encoding_version = proto.encoding_version();
  }
  if (proto.has_value_type()) {
    state.value_type = static_cast<ValueType>(proto.value_type());
  }

  if (proto.HasExtension(zetasketch::hyperloglogplus_unique_state)) {
    const auto& hll_proto =
        proto.GetExtension(zetasketch::hyperloglogplus_unique_state);

    if (hll_proto.has_sparse_size()) {
      state.sparse_size = hll_proto.sparse_size();
    }

    if (hll_proto.has_precision_or_num_buckets()) {
      state.precision = hll_proto.precision_or_num_buckets();
    }

    if (hll_proto.has_sparse_precision_or_num_buckets()) {
      state.sparse_precision = hll_proto.sparse_precision_or_num_buckets();
    }

    if (hll_proto.has_data()) {
      state.data = std::vector<uint8_t>();
      for (auto chunk : hll_proto.data().Chunks()) {
        state.data->insert(state.data->end(), chunk.begin(), chunk.end());
      }
    }

    if (hll_proto.has_sparse_data()) {
      state.sparse_data = std::vector<uint8_t>();
      for (auto chunk : hll_proto.sparse_data().Chunks()) {
        state.sparse_data->insert(state.sparse_data->end(), chunk.begin(),
                                  chunk.end());
      }
    }
  }

  return state;
}

namespace {
zetasketch::AggregatorStateProto BuildProto(const State& state) {
  zetasketch::AggregatorStateProto proto;

  proto.set_type(state.type.value_or(zetasketch::HYPERLOGLOG_PLUS_UNIQUE));
  proto.set_num_values(state.num_values);

  if (state.encoding_version != 1) {  // 1 is default
    proto.set_encoding_version(state.encoding_version);
  }

  if (state.value_type != ValueType::kUnknown) {
    proto.set_value_type(static_cast<int32_t>(state.value_type));
  }

  zetasketch::HyperLogLogPlusUniqueStateProto hll_proto;

  // Enforce Proto2 explicit-set semantics for precisions:
  hll_proto.set_precision_or_num_buckets(state.precision);
  if (state.sparse_precision != 0) {
    hll_proto.set_sparse_precision_or_num_buckets(state.sparse_precision);
  }

  // The reference emits the sparse size whenever it departs from its
  // default, and emits each data field whenever the field is present,
  // including when the field holds no bytes. Its read predicates
  // require a byte, but its write predicates test only for presence,
  // so an empty field parsed from the input reappears in the output.
  if (state.sparse_size != 0) {
    hll_proto.set_sparse_size(state.sparse_size);
  }

  if (state.data.has_value()) {
    hll_proto.set_data(absl::string_view(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const char*>(state.data->data()), state.data->size()));
  }

  if (state.sparse_data.has_value()) {
    hll_proto.set_sparse_data(absl::string_view(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const char*>(state.sparse_data->data()),
        state.sparse_data->size()));
  }

  proto.MutableExtension(zetasketch::hyperloglogplus_unique_state)
      ->CopyFrom(hll_proto);

  return proto;
}
}  // namespace

std::expected<std::vector<uint8_t>, utils::Error> State::ToByteArray() const {
  auto proto = BuildProto(*this);
  std::vector<uint8_t> output;
  output.resize(proto.ByteSizeLong());
  if (!proto.SerializeToArray(output.data(), static_cast<int>(output.size()))) {
    return std::unexpected(
        utils::Error{.code = utils::ErrorCode::kProtoSerialization,
                     .message = "Failed to serialize state"});
  }

  return output;
}

std::expected<void, utils::Error> State::ToByteArray(
    std::string* output) const {
  auto proto = BuildProto(*this);
  if (!proto.SerializeToString(output)) {
    return std::unexpected(
        utils::Error{.code = utils::ErrorCode::kProtoSerialization,
                     .message = "Failed to serialize state"});
  }

  return {};
}

}  // namespace zetasketch::hll
