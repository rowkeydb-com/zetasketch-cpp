// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/state.h"
#include <cstdint>
#include <expected>
#include <span>
#include <vector>
#include "aggregator.pb.h"
#include "hllplusplus.pb.h"
#include "zetasketch/utils/buffer_traits.h"

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

std::expected<std::vector<uint8_t>, utils::Error> State::ToByteArray() const {
  zetasketch::AggregatorStateProto proto;

  proto.set_type(type);
  proto.set_num_values(num_values);

  if (encoding_version != 1) {  // 1 is default
    proto.set_encoding_version(encoding_version);
  }

  if (value_type != ValueType::kUnknown) {
    proto.set_value_type(static_cast<int32_t>(value_type));
  }

  zetasketch::HyperLogLogPlusUniqueStateProto hll_proto;

  // Enforce Proto2 explicit-set semantics:
  // We must set these unconditionally, because the parser on the other
  // end might expect them if they are part of the active algorithm state.
  hll_proto.set_sparse_size(sparse_size);
  hll_proto.set_precision_or_num_buckets(precision);
  hll_proto.set_sparse_precision_or_num_buckets(sparse_precision);

  if (data.has_value() && !data->empty()) {
    hll_proto.set_data(absl::string_view(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const char*>(data->data()), data->size()));
  }

  if (sparse_data.has_value() && !sparse_data->empty()) {
    hll_proto.set_sparse_data(absl::string_view(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const char*>(sparse_data->data()),
        sparse_data->size()));
  }

  proto.MutableExtension(zetasketch::hyperloglogplus_unique_state)
      ->CopyFrom(hll_proto);

  std::vector<uint8_t> output;
  output.resize(proto.ByteSizeLong());
  if (!proto.SerializeToArray(output.data(), static_cast<int>(output.size()))) {
    return std::unexpected(
        utils::Error{.code = utils::ErrorCode::kProtoSerialization,
                     .message = "Failed to serialize state"});
  }

  return output;
}

}  // namespace zetasketch::hll
