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
#include "zetasketch/hll/math_utils.h"
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/error.h"
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

std::expected<void, utils::Error> SparseRepresentation::SortAndDedupBuffer() {
  if (buffer_.empty()) {
    return {};
  }

  std::ranges::sort(buffer_);

  std::vector<uint32_t> deduped;
  deduped.reserve(buffer_.size());
  std::optional<uint32_t> last_value;

  for (const uint32_t value : buffer_) {
    if (!last_value.has_value() || value != last_value.value()) {
      const uint32_t index = encoding_.DecodeSparseIndex(value);
      if (!deduped.empty() &&
          encoding_.DecodeSparseIndex(deduped.back()) == index) {
        deduped.back() = value;
      } else {
        deduped.push_back(value);
      }
      last_value = value;
    }
  }

  buffer_ = std::move(deduped);
  return {};
}

std::expected<void, utils::Error> SparseRepresentation::FlushBuffer() {
  if (buffer_.empty()) {
    return {};
  }

  auto sort_res = SortAndDedupBuffer();
  if (!sort_res.has_value()) return std::unexpected(sort_res.error());

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
    if (decoder.error().has_value()) {
      return std::unexpected(decoder.error().value());
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
  // The reference's normalization discards the sparse fields once
  // their values have been transferred into the register array.
  state_.sparse_data.reset();
  state_.sparse_size = 0;
  auto normal_res = NormalRepresentation::Create(std::move(state_));
  if (!normal_res.has_value()) {
    return std::unexpected(normal_res.error());
  }
  NormalRepresentation normal_repr = std::move(*normal_res);
  // The reference reaches its ensureData unconditionally here, through
  // addSparseValues, so a sparse sketch holding nothing still becomes a
  // normal sketch with a full register array rather than none.
  normal_repr.EnsureRegisterArray();

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
    if (decoder.error().has_value()) {
      return std::unexpected(decoder.error().value());
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

namespace {

// Adds one value to whichever representation it is given, as the
// reference's addUnsortedSparseValues does when the representation it
// is adding to has been promoted mid-way.
std::expected<Representation, utils::Error>
AddSparseValueToRepresentation(  // NOLINT(misc-no-recursion)
    Representation representation, const encoding::Sparse& source,
    uint32_t sparse_value) {
  if (auto* sparse = std::get_if<SparseRepresentation>(&representation)) {
    return std::move(*sparse).AddSparseValue(source, sparse_value);
  }
  auto added = std::get<NormalRepresentation>(representation)
                   .AddSparseValue(source, sparse_value);
  if (!added.has_value()) return std::unexpected(added.error());
  return representation;
}

}  // namespace

std::expected<Representation, utils::Error>
SparseRepresentation::Downgrade(  // NOLINT(misc-no-recursion)
    const encoding::Sparse& target) && {
  if (!target.IsLessThan(encoding_)) {
    return Representation(std::move(*this));
  }

  // The reference clears the stored stream and lowers both precisions
  // in place, leaving the sparse size as it found it for the flush that
  // follows to recompute, then re-adds the stream's values through the
  // target encoding.
  std::optional<std::vector<uint8_t>> stored;
  stored.swap(state_.sparse_data);
  state_.sparse_data.reset();
  state_.precision =
      std::min(encoding_.normal_precision(), target.normal_precision());
  state_.sparse_precision =
      std::min(encoding_.sparse_precision(), target.sparse_precision());

  const encoding::Sparse source_encoding = encoding_;
  std::vector<uint32_t> buffered;
  buffered.swap(buffer_);

  auto lowered = SparseRepresentation::Create(std::move(state_));
  if (!lowered.has_value()) return std::unexpected(lowered.error());
  Representation representation = std::move(lowered.value());

  if (stored.has_value() && !stored->empty()) {
    utils::DifferenceDecoder decoder(*stored);
    while (auto value = decoder.Next()) {
      auto added = AddSparseValueToRepresentation(
          std::move(representation), target,
          source_encoding.DowngradeSparseValue(value.value(), target));
      if (!added.has_value()) return std::unexpected(added.error());
      representation = std::move(added.value());
    }
    if (decoder.error().has_value()) {
      return std::unexpected(decoder.error().value());
    }
  }

  // The reference carries the buffer across through the target encoding
  // without lowering the values, its buffer iterator yielding them as
  // they stand, and in sorted order.
  std::ranges::sort(buffered);
  for (const uint32_t value : buffered) {
    auto added = AddSparseValueToRepresentation(std::move(representation),
                                                target, value);
    if (!added.has_value()) return std::unexpected(added.error());
    representation = std::move(added.value());
  }
  return representation;
}

std::expected<Representation, utils::Error>
SparseRepresentation::AddSparseValue(  // NOLINT(misc-no-recursion)
    const encoding::Sparse& source_sparse_encoding, uint32_t sparse_value) && {
  auto compatible = encoding_.AssertCompatible(source_sparse_encoding);
  if (!compatible.has_value()) {
    return std::unexpected(compatible.error());
  }

  // The reference lowers itself to the incoming encoding when that one
  // is lower, and lowers the incoming value when its own is lower.
  if (source_sparse_encoding.IsLessThan(encoding_)) {
    auto lowered = std::move(*this).Downgrade(source_sparse_encoding);
    if (!lowered.has_value()) return std::unexpected(lowered.error());
    return AddSparseValueToRepresentation(std::move(lowered.value()),
                                          source_sparse_encoding, sparse_value);
  }

  buffer_.push_back(
      encoding_.IsLessThan(source_sparse_encoding)
          ? source_sparse_encoding.DowngradeSparseValue(sparse_value, encoding_)
          : sparse_value);
  return std::move(*this).UpdateRepresentation();
}

std::expected<int64_t, utils::Error> SparseRepresentation::Estimate() const {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto* self = const_cast<SparseRepresentation*>(this);
  auto flush_res = self->FlushBuffer();
  if (!flush_res.has_value()) return std::unexpected(flush_res.error());

  // The reference counts buckets and empty buckets in 32-bit signed
  // arithmetic, and it validates the sparse size against nothing, so a
  // sparse size at or above the bucket count leaves no empty buckets
  // and one beyond it leaves a negative number of them. The first makes
  // the estimate infinite and the second makes it not a number; the
  // reference's rounding saturates and reports zero respectively, where
  // a cast would be undefined.
  const auto buckets = static_cast<int32_t>(
      1U << static_cast<uint32_t>(state_.sparse_precision));
  const auto num_zeros =
      static_cast<int32_t>(static_cast<uint32_t>(buckets) -
                           static_cast<uint32_t>(state_.sparse_size));
  const double estimate =
      static_cast<double>(buckets) *
      std::log(static_cast<double>(buckets) / static_cast<double>(num_zeros));
  return RoundAsTheReferenceDoes(estimate);
}

std::expected<void, utils::Error> SparseRepresentation::MergeInto(
    NormalRepresentation& target) const {
  auto begun = target.BeginSparseValues(encoding_);
  if (!begun.has_value()) return begun;

  if (state_.sparse_data.has_value() && !state_.sparse_data->empty()) {
    utils::DifferenceDecoder decoder(*state_.sparse_data);
    while (auto value = decoder.Next()) {
      auto added = target.AddSparseValue(encoding_, value.value());
      if (!added.has_value()) return added;
    }
    if (decoder.error().has_value()) {
      return std::unexpected(decoder.error().value());
    }
  }

  for (const uint32_t value : buffer_) {
    auto added = target.AddSparseValue(encoding_, value);
    if (!added.has_value()) return added;
  }
  return {};
}

std::expected<Representation, utils::Error>
SparseRepresentation::MergeFromSparse(  // NOLINT(misc-no-recursion)
    const SparseRepresentation& other) && {
  const encoding::Sparse& source = other.encoding();
  auto compatible = encoding_.AssertCompatible(source);
  if (!compatible.has_value()) {
    return std::unexpected(compatible.error());
  }

  // The reference lowers itself before it looks at whether the operand
  // has anything to contribute, so an operand of lower precision lowers
  // this representation even when it carries no values at all. Doing it
  // the other way round leaves the precisions of an empty merge
  // untouched, and every later value encoded against the wrong one.
  Representation representation = Representation(std::move(*this));
  if (source.IsLessThan(
          std::get<SparseRepresentation>(representation).encoding())) {
    auto lowered = std::move(std::get<SparseRepresentation>(representation))
                       .Downgrade(source);
    if (!lowered.has_value()) return std::unexpected(lowered.error());
    representation = std::move(lowered.value());
  }

  // Where the encodings have become equal, the reference merges the two
  // sorted streams, deduplicates the result, and updates the
  // representation once rather than once per value. The bytes are the
  // same either way; the number of updates is not, and an update is
  // what promotes a sparse sketch to a dense one. Adding value by value
  // here would leave the sketch sparse where the reference has already
  // promoted it, and estimate it differently until the next write.
  auto* sparse = std::get_if<SparseRepresentation>(&representation);
  const bool encodings_are_equal =
      sparse != nullptr && !sparse->encoding_.IsLessThan(source);

  if (other.state_.sparse_data.has_value() &&
      !other.state_.sparse_data->empty()) {
    utils::DifferenceDecoder decoder(*other.state_.sparse_data);
    while (auto value = decoder.Next()) {
      if (encodings_are_equal) {
        sparse->buffer_.push_back(value.value());
        continue;
      }
      auto added = AddSparseValueToRepresentation(std::move(representation),
                                                  source, value.value());
      if (!added.has_value()) return std::unexpected(added.error());
      representation = std::move(added.value());
    }
    if (decoder.error().has_value()) {
      return std::unexpected(decoder.error().value());
    }
  }

  for (const uint32_t value : other.buffer_) {
    if (encodings_are_equal) {
      sparse->buffer_.push_back(value);
      continue;
    }
    auto added = AddSparseValueToRepresentation(std::move(representation),
                                                source, value);
    if (!added.has_value()) return std::unexpected(added.error());
    representation = std::move(added.value());
  }

  if (!encodings_are_equal) {
    return representation;
  }
  auto flushed = sparse->FlushBuffer();
  if (!flushed.has_value()) return std::unexpected(flushed.error());
  return std::move(*sparse).UpdateRepresentation();
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
