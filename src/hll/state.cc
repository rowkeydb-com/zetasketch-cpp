// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/state.h"
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>
#include "aggregator.pb.h"
#include "hllplusplus.pb.h"
#include "zetasketch/utils/error.h"

namespace zetasketch::hll {

std::expected<State, utils::Error> State::Parse(
    std::span<const uint8_t> input) {
  zetasketch::AggregatorStateProto proto;
  if (!proto.ParseFromArray(input.data(), static_cast<int>(input.size()))) {
    return std::unexpected(
        utils::Error{.code = utils::ErrorCode::kProtoDeserialization,
                     .message = "Failed to parse state"});
  }

  State state;
  state.type = proto.type();
  state.num_values = proto.num_values();
  state.encoding_version = proto.encoding_version();
  state.value_type = static_cast<ValueType>(proto.value_type());

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

  proto.set_type(state.type);
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

  if (state.sparse_data.has_value() && state.sparse_size > 0) {
    hll_proto.set_sparse_size(state.sparse_size);
  }

  if (state.data.has_value() && !state.data->empty()) {
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
