// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HYPERLOGLOGPLUSPLUS_H_
#define ZETASKETCH_HYPERLOGLOGPLUSPLUS_H_

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "zetasketch/hll/normal_representation.h"
#include "zetasketch/hll/representation.h"
#include "zetasketch/hll/sparse_representation.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/error.h"

namespace zetasketch {

class HyperLogLogPlusPlus {
 public:
  // The advertised precision limits are the limits the implementation
  // enforces, and match the public constants of the same names in the
  // reference implementation at revision d098c49. The published Maven
  // artifact is older and enforces a higher minimum normal precision;
  // the differential tests build the reference from source for that
  // reason.
  static constexpr int32_t kMinimumPrecision =
      hll::NormalRepresentation::kMinimumPrecision;
  static constexpr int32_t kMaximumPrecision =
      hll::NormalRepresentation::kMaximumPrecision;
  static constexpr int32_t kDefaultNormalPrecision = 15;
  static constexpr int32_t kMaximumSparsePrecision =
      hll::SparseRepresentation::kMaximumSparsePrecision;
  static constexpr int32_t kSparsePrecisionDisabled =
      hll::SparseRepresentation::kSparsePrecisionDisabled;
  // The reference's builder adds this to the normal precision when no
  // sparse precision is given, capping the result at the sparse
  // maximum, and so does Create when given only a normal precision.
  static constexpr int32_t kDefaultSparsePrecisionDelta = 5;
  static constexpr int32_t kEncodingVersion = 2;

  // Constructs a new sketch. The value type is fixed here, as the
  // reference fixes it when it builds an aggregator, and it decides
  // which additions the sketch will accept: a sketch built for text
  // refuses an integer and one built for integers refuses text, in
  // both libraries. The default builds for text; the reference has no
  // default, each of its builders naming a type, so text is a choice
  // made here. A sketch may also be built for 32-bit integers, which
  // this library reads, merges and writes but has no addition for: the
  // narrower addition arrives with the 32-bit mode itself.
  //
  // A call that names no sparse precision gets the one the reference's
  // builder would choose, the normal precision plus
  // kDefaultSparsePrecisionDelta capped at kMaximumSparsePrecision, so
  // a sketch built here from a normal precision alone is byte for byte
  // the sketch the reference builds from the same one. Sparse mode is
  // disabled only by naming kSparsePrecisionDisabled.
  // We use std::expected for allocation-free error handling.
  [[nodiscard]] static std::expected<HyperLogLogPlusPlus, utils::Error> Create(
      int32_t normal_precision, int32_t sparse_precision,
      hll::ValueType value_type = hll::ValueType::kBytesOrUtf8String);
  [[nodiscard]] static std::expected<HyperLogLogPlusPlus, utils::Error> Create(
      int32_t normal_precision,
      hll::ValueType value_type = hll::ValueType::kBytesOrUtf8String);
  [[nodiscard]] static std::expected<HyperLogLogPlusPlus, utils::Error>
  Create();

  // Deserializes a sketch from a BigTable/byte array representation.
  [[nodiscard]] static std::expected<HyperLogLogPlusPlus, utils::Error>
  FromBytes(std::span<const uint8_t> data);

  // Deserializes a sketch from a string_view representation.
  [[nodiscard]] static std::expected<HyperLogLogPlusPlus, utils::Error>
  FromBytes(std::string_view data);

  // Walks the contents of the stored state and reports the first defect
  // in them, or nothing when there is none. FromBytes checks what the
  // reference checks when it reads, which stops short of the registers'
  // values and the sparse stream's contents; both are read lazily, by
  // whichever later operation first needs them, and a defect in either
  // is reported then, from a sketch that may by that time have been
  // stored. This is the check to make at the point bytes arrive from a
  // caller that is not trusted, so that a defective sketch is refused
  // there instead.
  //
  // Refused: a register above the largest value a hash can produce at
  // the sketch's precision; and a sparse stream that does not decode,
  // does not strictly increase, repeats a sparse index, names an index
  // outside either precision, or holds a number of values other than
  // the recorded sparse size. Nothing the reference writes is refused.
  // A dense sketch's sparse fields are not read here, because neither
  // library reads them anywhere.
  //
  // Values this library has buffered and not yet written to the stream
  // are its own and are not walked. Nothing allocates unless a defect
  // is found, when its description does.
  [[nodiscard]] std::expected<void, utils::Error> Validate() const;

  // This class is designated as movable, but it explicitly prohibits copy
  // operations to ensure strict memory constraints.
  HyperLogLogPlusPlus(HyperLogLogPlusPlus&& other) noexcept = default;
  HyperLogLogPlusPlus& operator=(HyperLogLogPlusPlus&& other) noexcept =
      default;
  HyperLogLogPlusPlus(const HyperLogLogPlusPlus&) = delete;
  HyperLogLogPlusPlus& operator=(const HyperLogLogPlusPlus&) = delete;
  ~HyperLogLogPlusPlus() = default;

  // Adds a string value to the sketch. An addition whose flush merges
  // buffered values with a defective stored stream reports the defect;
  // an addition that only buffers succeeds without walking stored
  // bytes. After a failure on the flush route the stored stream and
  // committed counters are unchanged, the attempted value remains
  // buffered, and later operations repeat the error. After a failure
  // on the direct-normalization route (a stored stream already at the
  // promotion threshold) the sketch is not preserved, and later
  // serialization can succeed on truncated state.
  [[nodiscard]] std::expected<void, utils::Error> Add(std::string_view value);

  // Adds an integer value to the sketch. The value is hashed as the
  // reference hashes one: written as eight bytes, the least significant
  // first, and fingerprinted, so a sketch built from integers is byte
  // for byte the sketch the reference builds from the same integers.
  // Refused, without throwing, by a sketch whose value type admits
  // something else.
  [[nodiscard]] std::expected<void, utils::Error> Add(int64_t value);

  // This function adds a precomputed raw hash to the sketch, which is utilized
  // internally or by algorithmic extensions.
  [[nodiscard]] std::expected<void, utils::Error> AddHash(uint64_t hash);

  // Merges another sketch into this one.
  // Consumes `other` explicitly via std::move() to enforce zero-allocation
  // architectural constraints where possible.
  //
  // The kinds of addition the two sketches admit are intersected first,
  // as the reference intersects them, and a merge of two sketches that
  // admit nothing in common is refused with the receiver unchanged: a
  // sketch of integers and a sketch of text are not merged into one
  // sketch that misdescribes both. A merge the encodings then refuse
  // for their precisions leaves the intersection in place all the
  // same, because that is the order in which the reference does it.
  // The recorded value type is not written by a merge, in either
  // library, so a sketch that admits one kind only because of a merge
  // still serializes without one.
  [[nodiscard]] std::expected<void, utils::Error> Merge(
      HyperLogLogPlusPlus&& other);

  // Returns the estimated cardinality. Estimation flushes buffered
  // values first, so with anything buffered it walks the stored stream
  // and reports a defect found there; with nothing buffered the stored
  // bytes are not walked and are not validated here.
  [[nodiscard]] std::expected<int64_t, utils::Error> Result() const;

  // Serializes the sketch to a byte array. Serialization flushes
  // buffered values, so with anything buffered it walks and validates
  // the stored stream. With nothing buffered, stored bytes below the
  // promotion threshold are emitted verbatim, defective or not; a
  // stream at or past the threshold is normalized first, which walks
  // and validates it.
  // Serialization compacts the sketch, as the reference's does, so it
  // is not a constant operation: a sparse sketch that compaction
  // promotes is left promoted and estimated as a dense one afterwards.
  [[nodiscard]] std::expected<std::vector<uint8_t>, utils::Error> Serialize();

  // Serializes into a provided buffer to avoid allocation.
  [[nodiscard]] std::expected<void, utils::Error> Serialize(
      std::vector<uint8_t>& sink);

  // Serializes into a provided string buffer to avoid allocation.
  [[nodiscard]] std::expected<void, utils::Error> Serialize(std::string& sink);

 private:
  explicit HyperLogLogPlusPlus(hll::Representation representation);

  // Returns the state of whichever representation is current.
  [[nodiscard]] hll::State& MutableState();

  // Extracts and conditionally compacts the internal state for serialization.
  [[nodiscard]] std::expected<hll::State, utils::Error>
  GetStateForSerialization();

  hll::Representation representation_;

  // The kinds of addition this sketch admits, one bit per kind, in the
  // reference's own terms: it distinguishes a long, an integer, a
  // string and a byte array, and derives the set from the recorded
  // value type when a sketch is built or read. The set is kept beside
  // the sketch rather than in its bytes, and it only ever shrinks: to
  // one kind on the first addition, and to the intersection on a
  // merge. Two sketches can therefore serialize identically, refuse
  // different additions, and say so in different words.
  uint32_t admitted_kinds_ = 0;
};

}  // namespace zetasketch

#endif  // ZETASKETCH_HYPERLOGLOGPLUSPLUS_H_
