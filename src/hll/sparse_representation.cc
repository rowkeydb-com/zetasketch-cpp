// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/sparse_representation.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <utility>
#include <variant>
#include <vector>
#include "zetasketch/hll/encoding.h"
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/buffer_traits.h"
#include "zetasketch/utils/iterators.h"

namespace zetasketch::hll {

SparseRepresentation::SparseRepresentation(State state,
                                           encoding::Sparse encoding,
                                           size_t max_sparse_data_bytes,
                                           size_t max_buffer_elements)
    : state_(std::move(state)),
      encoding_(std::move(encoding)),
      max_sparse_data_bytes_(max_sparse_data_bytes),
      max_buffer_elements_(max_buffer_elements) {
  buffer_.reserve(max_buffer_elements_ + 1);
}

std::expected<SparseRepresentation, utils::Error> SparseRepresentation::Create(
    State state) {
  auto check = CheckPrecision(state.precision, state.sparse_precision);
  if (!check.has_value()) {
    return std::unexpected(check.error());
  }

  auto enc = encoding::Sparse::Create(state.precision, state.sparse_precision);
  if (!enc.has_value()) {
    return std::unexpected(enc.error());
  }

  const auto m_normal_bytes =
      static_cast<size_t>(1ULL << static_cast<uint32_t>(state.precision));
  const auto max_sparse_data_bytes = static_cast<size_t>(
      static_cast<float>(m_normal_bytes) * kMaximumSparseDataFraction);
  const auto max_buffer_elements = static_cast<size_t>(
      static_cast<float>(m_normal_bytes) * kMaximumBufferElementsFraction);

  if (max_sparse_data_bytes == 0 || max_buffer_elements == 0) {
    return std::unexpected(
        utils::Error{.code = utils::ErrorCode::kIllegalArgument,
                     .message = "Calculated max sparse data bytes or buffer "
                                "elements is zero, precision too low?"});
  }

  return SparseRepresentation(std::move(state), *std::move(enc),
                              max_sparse_data_bytes, max_buffer_elements);
}

std::expected<void, utils::Error> SparseRepresentation::CheckPrecision(
    int32_t normal_precision, int32_t sparse_precision) {
  auto normal_check = NormalRepresentation::CheckPrecision(normal_precision);
  if (!normal_check.has_value()) {
    return std::unexpected(normal_check.error());
  }
  if (sparse_precision < normal_precision ||
      sparse_precision > kMaximumSparsePrecision) {
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIllegalArgument,
        .message = std::format("Expected sparse precision to be >= normal "
                               "precision ({}) and <= {} but was {}.",
                               normal_precision, kMaximumSparsePrecision,
                               sparse_precision)});
  }
  return {};
}

void SparseRepresentation::SortAndDedupBuffer() {
  std::ranges::sort(buffer_, [this](uint32_t a, uint32_t b) {
    const uint32_t idx_a = encoding_.DecodeSparseIndex(a);
    const uint32_t idx_b = encoding_.DecodeSparseIndex(b);
    if (idx_a != idx_b) {
      return idx_a < idx_b;
    }
    return a > b;
  });

  auto unique_range =
      std::ranges::unique(buffer_, [this](uint32_t a, uint32_t b) {
        return encoding_.DecodeSparseIndex(a) == encoding_.DecodeSparseIndex(b);
      });
  buffer_.erase(unique_range.begin(), buffer_.end());
}

std::expected<void, utils::Error> SparseRepresentation::FlushBuffer() {
  if (buffer_.empty()) {
    return {};
  }

  SortAndDedupBuffer();

  utils::DifferenceEncoder encoder(std::move(scratch_sparse_data_));
  int32_t new_sparse_size = 0;
  if (!state_.sparse_data.has_value() || state_.sparse_data->empty()) {
    for (const uint32_t val : buffer_) {
      auto put_res = encoder.PutInt(static_cast<int32_t>(val));
      if (!put_res.has_value()) return std::unexpected(put_res.error());
      new_sparse_size++;
    }
  } else {
    utils::DifferenceDecoder decoder(*state_.sparse_data);
    const utils::DifferenceDecoderIterator dec_iter(&decoder);
    const utils::DifferenceDecoderIterator dec_end;

    utils::MergedIntIterator<utils::DifferenceDecoderIterator,
                             std::vector<uint32_t>::const_iterator>
        merged_iter(dec_iter, dec_end, buffer_.begin(), buffer_.end());

    std::optional<uint32_t> last_index = std::nullopt;
    std::optional<uint32_t> last_val = std::nullopt;
    while (auto val = merged_iter.Next()) {
      const uint32_t idx = encoding_.DecodeSparseIndex(val.value());
      if (last_index.has_value() && last_index.value() == idx) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        last_val = std::max(last_val.value(), val.value());
      } else {
        if (last_val.has_value()) {
          auto put_res = encoder.PutInt(static_cast<int32_t>(last_val.value()));
          if (!put_res.has_value()) return std::unexpected(put_res.error());
          new_sparse_size++;
        }
        last_index = idx;
        last_val = val;
      }
    }
    if (last_val.has_value()) {
      auto put_res = encoder.PutInt(static_cast<int32_t>(last_val.value()));
      if (!put_res.has_value()) return std::unexpected(put_res.error());
      new_sparse_size++;
    }
  }

  scratch_sparse_data_ = std::move(encoder).IntoVec();
  if (state_.sparse_data.has_value()) {
    std::swap(state_.sparse_data.value(), scratch_sparse_data_);
    scratch_sparse_data_.clear();
  } else {
    state_.sparse_data = std::move(scratch_sparse_data_);
  }
  state_.sparse_size = new_sparse_size;
  buffer_.clear();
  return {};
}

std::expected<Representation, utils::Error>
SparseRepresentation::UpdateRepresentation() && {
  if (buffer_.size() > max_buffer_elements_) {
    auto res = FlushBuffer();
    if (!res.has_value()) {
      return std::unexpected(res.error());
    }
  }

  bool should_normalize = false;
  if (state_.sparse_data.has_value()) {
    should_normalize =
        state_.sparse_data.value().size() >= max_sparse_data_bytes_;
  }

  if (should_normalize) {
    return std::move(*this).Normalize();
  }
  return std::move(*this);
}

std::expected<Representation, utils::Error>
SparseRepresentation::Normalize() && {
  std::optional<std::vector<uint8_t>> extracted_sparse_data = std::nullopt;
  if (state_.sparse_data.has_value()) {
    extracted_sparse_data = std::move(state_.sparse_data.value());
  }
  auto normal_res = NormalRepresentation::Create(std::move(state_));
  if (!normal_res.has_value()) {
    return std::unexpected(normal_res.error());
  }
  NormalRepresentation normal_repr = std::move(*normal_res);

  if (extracted_sparse_data.has_value() &&
      !extracted_sparse_data.value().empty()) {
    utils::DifferenceDecoder decoder(extracted_sparse_data.value());
    while (true) {
      auto val_opt = decoder.Next();
      if (!val_opt.has_value()) {
        break;
      }
      auto add_res = normal_repr.AddSparseValue(encoding_, *val_opt);
      if (!add_res.has_value()) return std::unexpected(add_res.error());
    }
  }

  for (const uint32_t val : buffer_) {
    auto add_res = normal_repr.AddSparseValue(encoding_, val);
    if (!add_res.has_value()) return std::unexpected(add_res.error());
  }

  return normal_repr;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::expected<Representation, utils::Error> SparseRepresentation::AddHash(
    uint64_t hash) && {
  const uint32_t encoded_val = encoding_.Encode(hash);
  buffer_.push_back(encoded_val);
  return std::move(*this).UpdateRepresentation();
}

std::expected<Representation, utils::Error>
SparseRepresentation::AddSparseValue(  // NOLINT(misc-no-recursion)
    const encoding::Sparse& source_sparse_encoding, uint32_t sparse_value) && {
  if (source_sparse_encoding.normal_precision() !=
          encoding_.normal_precision() ||
      source_sparse_encoding.sparse_precision() !=
          encoding_.sparse_precision()) {
    // Sketch precision transitions are not currently supported.
    return std::unexpected(utils::Error{
        .code = utils::ErrorCode::kIncompatiblePrecision,
        .message = "Precision transitions are not currently supported"});
  }

  buffer_.push_back(sparse_value);
  return std::move(*this).UpdateRepresentation();
}

std::expected<int64_t, utils::Error> SparseRepresentation::Estimate() const {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto* self = const_cast<SparseRepresentation*>(this);
  auto flush_res = self->FlushBuffer();
  if (!flush_res.has_value()) return std::unexpected(flush_res.error());

  const auto buckets = static_cast<int64_t>(
      1ULL << static_cast<uint32_t>(state_.sparse_precision));

  const int64_t num_zeros = buckets - state_.sparse_size;
  const double estimate =
      static_cast<double>(buckets) *
      std::log(static_cast<double>(buckets) / static_cast<double>(num_zeros));
  return static_cast<int64_t>(std::round(estimate));
}

std::expected<Representation, utils::Error>
SparseRepresentation::MergeFromSparse(const SparseRepresentation& other) && {
  Representation repr = std::move(*this);

  auto process_val = [&](uint32_t val) -> std::expected<void, utils::Error> {
    if (auto* s = std::get_if<SparseRepresentation>(&repr)) {
      auto add_res = std::move(*s).AddSparseValue(other.encoding(), val);
      if (!add_res.has_value()) return std::unexpected(add_res.error());
      repr = std::move(add_res.value());
    } else if (auto* n = std::get_if<NormalRepresentation>(&repr)) {
      auto add_res = n->AddSparseValue(other.encoding(), val);
      if (!add_res.has_value()) return std::unexpected(add_res.error());
    }
    return {};
  };

  if (other.state_.sparse_data.has_value()) {
    utils::DifferenceDecoder decoder(*other.state_.sparse_data);
    while (auto val = decoder.Next()) {
      auto res = process_val(val.value());
      if (!res.has_value()) return std::unexpected(res.error());
    }
  }

  for (const uint32_t val : other.buffer_) {
    auto res = process_val(val);
    if (!res.has_value()) return std::unexpected(res.error());
  }

  return repr;
}

std::expected<Representation, utils::Error> SparseRepresentation::Compact() && {
  auto res = FlushBuffer();
  if (!res.has_value()) {
    return std::unexpected(res.error());
  }
  if (!state_.sparse_data.has_value()) {
    state_.sparse_data = std::vector<uint8_t>();
  }
  return std::move(*this).UpdateRepresentation();
}

}  // namespace zetasketch::hll
