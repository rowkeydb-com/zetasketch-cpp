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
#include <span>
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
      max_buffer_elements_(max_buffer_elements) {}

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

namespace {

// The stored stream of a state as a span, empty where there is none.
std::span<const uint8_t> StoredStream(const State& state) {
  if (!state.sparse_data.has_value()) return {};
  return std::span<const uint8_t>(*state.sparse_data);
}

// Yields a stored stream and a sorted buffer as one increasing
// sequence, the stream's value first on a tie, as the reference's
// sortedIterator does.
//
// The stream is decoded one value ahead, and that decoding happens at
// the moment the value before it is handed out, which is when the
// reference's merged iterator decodes it too. A value that does not
// decode therefore ends the sequence exactly where the reference
// throws: the value in hand is dropped, nothing after it is yielded, and
// error() holds the failure. What either library has written by then is
// the same, so the refusal each reports is the same kind.
class SortedValues {
 public:
  SortedValues(std::span<const uint8_t> stored,
               std::span<const uint32_t> buffered)
      : decoder_(stored), buffered_(buffered) {
    stream_next_ = decoder_.Next();
  }

  [[nodiscard]] std::optional<uint32_t> Next() {
    if (decoder_.error().has_value()) return std::nullopt;
    const bool buffer_remains = buffered_index_ < buffered_.size();
    if (stream_next_.has_value() &&
        (!buffer_remains ||
         stream_next_.value() <= buffered_[buffered_index_])) {
      const uint32_t value = stream_next_.value();
      stream_next_ = decoder_.Next();
      if (decoder_.error().has_value()) return std::nullopt;
      return value;
    }
    if (buffer_remains) return buffered_[buffered_index_++];
    return std::nullopt;
  }

  // Holds the stream's decode failure once the sequence has ended on one.
  [[nodiscard]] const std::optional<utils::Error>& error() const {
    return decoder_.error();
  }

 private:
  utils::DifferenceDecoder decoder_;
  std::span<const uint32_t> buffered_;
  size_t buffered_index_ = 0;
  std::optional<uint32_t> stream_next_;
};

// Yields two sorted sequences as one, the smaller head first and the
// first sequence's on a tie, as the reference's MergedIntIterator does
// when it merges two representations of one encoding. Each sequence is
// read one value ahead, as above, so a decode failure in either ends
// this sequence where the reference's outer iterator would have thrown:
// when the value before the one in hand was handed out.
class MergedSequences {
 public:
  MergedSequences(SortedValues* first, SortedValues* second)
      : first_(first), second_(second) {
    first_next_ = first_->Next();
    second_next_ = second_->Next();
  }

  [[nodiscard]] std::optional<uint32_t> Next() {
    if (error().has_value()) return std::nullopt;
    if (second_next_.has_value() &&
        (!first_next_.has_value() || second_next_ < first_next_)) {
      const uint32_t value = second_next_.value();
      second_next_ = second_->Next();
      if (second_->error().has_value()) return std::nullopt;
      return value;
    }
    if (first_next_.has_value()) {
      const uint32_t value = first_next_.value();
      first_next_ = first_->Next();
      if (first_->error().has_value()) return std::nullopt;
      return value;
    }
    return std::nullopt;
  }

  // Holds a decode failure of either stream once the sequence has ended
  // on one, the first sequence's ahead of the second's.
  [[nodiscard]] const std::optional<utils::Error>& error() const {
    return first_->error().has_value() ? first_->error() : second_->error();
  }

 private:
  SortedValues* first_;
  SortedValues* second_;
  std::optional<uint32_t> first_next_;
  std::optional<uint32_t> second_next_;
};

// Writes a sorted sequence of values deduplicated in one pass, as the
// reference's dedupe does, and returns how many were written. A value
// without an encoded rho is its own sparse index, and only an exact
// repeat of it is dropped. A value with one opens a run of every value
// after it with the same sparse index, and the last of the run is
// written. The two rules are not one rule: a value without an encoded
// rho never joins the run before it, whatever its index, so a stream can
// hold a value beside another of the same index. That happens only with
// values a downgrade carried across at their old precision, and the
// reference writes both, so this library must too. Nor is any part of
// the sequence deduplicated on its own first: a stored value can fall
// between two buffered values of one index, and the reference, which
// sees the merged sequence, keeps both of them.
//
// A stream that stops decoding ends the sequence at the value the
// reference would have been handing out when it met the bad bytes; that
// value is dropped and the failure is reported. The values written
// before it were written by the reference too, and one of those may have
// been refused by the encoder first, in both libraries alike.
template <typename Sequence>
std::expected<int32_t, utils::Error> WriteDeduplicated(
    const encoding::Sparse& encoding, Sequence& values,
    utils::DifferenceEncoder& encoder) {
  int32_t written = 0;
  std::optional<uint32_t> next = values.Next();
  while (next.has_value()) {
    uint32_t value = next.value();
    if (encoding.HasEncodedRhoW(value)) {
      const uint32_t index = encoding.DecodeSparseIndex(value);
      next = values.Next();
      while (next.has_value() &&
             encoding.DecodeSparseIndex(next.value()) == index) {
        value = next.value();
        next = values.Next();
      }
    } else {
      next = values.Next();
      while (next.has_value() && next.value() == value) {
        next = values.Next();
      }
    }
    if (!next.has_value() && values.error().has_value()) {
      return std::unexpected(values.error().value());
    }
    auto put_res = encoder.PutInt(static_cast<int32_t>(value));
    if (!put_res.has_value()) return std::unexpected(put_res.error());
    written++;
  }
  if (values.error().has_value()) {
    return std::unexpected(values.error().value());
  }
  return written;
}

}  // namespace

void SparseRepresentation::SetStream(std::vector<uint8_t> stream,
                                     int32_t size) {
  scratch_sparse_data_ = std::move(stream);
  if (state_.sparse_data.has_value()) {
    std::swap(state_.sparse_data.value(), scratch_sparse_data_);
    scratch_sparse_data_.clear();
  } else {
    state_.sparse_data = std::move(scratch_sparse_data_);
  }
  state_.sparse_size = size;
  buffer_.clear();
}

std::expected<void, utils::Error> SparseRepresentation::FlushBuffer() {
  if (buffer_.empty()) {
    return {};
  }
  SortedValues values(StoredStream(state_), buffer_.Sorted());
  utils::DifferenceEncoder encoder(std::move(scratch_sparse_data_));
  auto written = WriteDeduplicated(encoding_, values, encoder);
  if (!written.has_value()) return std::unexpected(written.error());
  SetStream(std::move(encoder).IntoVec(), written.value());
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

  std::expected<void, utils::Error> added;
  buffer_.ForEach([&normal_repr, &added, this](uint32_t value) {
    added = normal_repr.AddSparseValue(encoding_, value);
    return added.has_value();
  });
  if (!added.has_value()) return std::unexpected(added.error());

  return normal_repr;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::expected<Representation, utils::Error> SparseRepresentation::AddHash(
    uint64_t hash) && {
  buffer_.Insert(encoding_.Encode(hash));
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
  SparseBuffer buffered = std::exchange(buffer_, SparseBuffer());

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
  for (const uint32_t value : buffered.Sorted()) {
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

  buffer_.Insert(
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

  std::expected<void, utils::Error> added;
  buffer_.ForEach([&target, &added, this](uint32_t value) {
    added = target.AddSparseValue(encoding_, value);
    return added.has_value();
  });
  return added;
}

std::expected<Representation, utils::Error>
SparseRepresentation::MergeFromSparse(  // NOLINT(misc-no-recursion)
    SparseRepresentation& other) && {
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

  // Only now does the reference look at the operand, and one holding
  // nothing leaves this representation as the lowering left it: no
  // flush, no update. Flushing here would move values out of the buffer
  // that the reference still holds unflushed, and a later downgrade
  // would then carry different values across.
  if (StoredStream(other.state_).empty() && other.buffer_.empty()) {
    return representation;
  }

  // The operand's values arrive in increasing order, its stored stream
  // and its buffer merged, as the reference's sortedIterator yields
  // them: a stored stream in the order it decodes, not sorted again.
  SortedValues theirs(StoredStream(other.state_), other.buffer_.Sorted());

  // Where this representation is the lower one, each value is lowered
  // and added on its own, and may flush the buffer or promote the
  // representation, so the order the values arrive in is the order the
  // reference must see too.
  auto* sparse = std::get_if<SparseRepresentation>(&representation);
  if (sparse == nullptr || sparse->encoding_.IsLessThan(source)) {
    while (auto value = theirs.Next()) {
      auto added = AddSparseValueToRepresentation(std::move(representation),
                                                  source, value.value());
      if (!added.has_value()) return std::unexpected(added.error());
      representation = std::move(added.value());
    }
    if (theirs.error().has_value()) {
      return std::unexpected(theirs.error().value());
    }
    return representation;
  }

  // Where the encodings have become equal, the reference merges the two
  // sides' sorted sequences, writes the result deduplicated as one
  // stream, and updates the representation once rather than once per
  // value. The bytes are the same either way; the number of updates is
  // not, and an update is what promotes a sparse sketch to a dense one.
  // Adding value by value here would leave the sketch sparse where the
  // reference has already promoted it, and estimate it differently
  // until the next write. Nor may the operand's stream pass through the
  // buffer: a stream read from bytes need not increase, and the
  // reference refuses such a stream when it writes the merged sequence,
  // where a buffer would have sorted it into acceptance.
  SortedValues mine(StoredStream(sparse->state_), sparse->buffer_.Sorted());
  MergedSequences values(&mine, &theirs);
  utils::DifferenceEncoder encoder(std::move(sparse->scratch_sparse_data_));
  auto written = WriteDeduplicated(sparse->encoding_, values, encoder);
  if (!written.has_value()) return std::unexpected(written.error());
  sparse->SetStream(std::move(encoder).IntoVec(), written.value());
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
