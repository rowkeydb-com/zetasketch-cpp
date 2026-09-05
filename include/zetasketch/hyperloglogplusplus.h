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
#include <utility>
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
  // maximum. Create below does not: its default disables sparse mode,
  // so a caller who supplies neither precision gets a different sketch
  // from the reference's default. Nothing in the library reads this
  // constant yet.
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
  // We use std::expected for allocation-free error handling.
  [[nodiscard]] static std::expected<HyperLogLogPlusPlus, utils::Error> Create(
      int32_t normal_precision = kDefaultNormalPrecision,
      int32_t sparse_precision = kSparsePrecisionDisabled,
      hll::ValueType value_type = hll::ValueType::kBytesOrUtf8String);

  // Deserializes a sketch from a BigTable/byte array representation.
  [[nodiscard]] static std::expected<HyperLogLogPlusPlus, utils::Error>
  FromBytes(std::span<const uint8_t> data);

  // Deserializes a sketch from a string_view representation.
  [[nodiscard]] static std::expected<HyperLogLogPlusPlus, utils::Error>
  FromBytes(std::string_view data);

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
  explicit HyperLogLogPlusPlus(hll::Representation representation)
      : representation_(std::move(representation)) {}

  // Returns the state of whichever representation is current.
  [[nodiscard]] hll::State& MutableState();

  // Extracts and conditionally compacts the internal state for serialization.
  [[nodiscard]] std::expected<hll::State, utils::Error>
  GetStateForSerialization();

  hll::Representation representation_;

  // Whether an addition has already narrowed the kinds this sketch
  // accepts. The reference keeps that set beside the sketch rather than
  // in its bytes and narrows it to a single kind on the first addition,
  // so a sketch recording the value type shared by text and byte arrays
  // admits both until something is added and only text afterwards. Two
  // sketches can therefore serialize identically, refuse different
  // additions, and say so in different words.
  bool additions_narrowed_ = false;
};

}  // namespace zetasketch

#endif  // ZETASKETCH_HYPERLOGLOGPLUSPLUS_H_
