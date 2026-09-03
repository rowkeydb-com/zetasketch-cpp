// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/encoding.h"
#include <cstdint>
#include <limits>
#include <gtest/gtest.h>
#include "zetasketch/utils/error.h"

namespace zetasketch::hll::encoding {
namespace {

constexpr int32_t kMaxNormal = Sparse::kMaxEncodableNormalPrecision;
constexpr int32_t kMaxSparse = Sparse::kMaxEncodableSparsePrecision;
constexpr uint32_t kFullRhoW = Sparse::kRhoWMask;
constexpr uint32_t kRhoWBits = static_cast<uint32_t>(Sparse::kRhoWBits);

// A configuration in which the two precisions are equal, so the mask
// is empty and no value can take the plain form.
constexpr int32_t kEqualPrecision = 3;
// A configuration in which the normal precision plus the rho width
// exceeds the sparse precision, and one in which it does not.
constexpr int32_t kNormalDominatesNormalPrecision = 4;
constexpr int32_t kNormalDominatesSparsePrecision = 7;
constexpr int32_t kSparseDominatesNormalPrecision = 2;
constexpr int32_t kSparseDominatesSparsePrecision = 9;
// An ordinary configuration, and values that fall either side of its
// mask.
constexpr int32_t kOrdinaryNormalPrecision = 10;
constexpr int32_t kOrdinarySparsePrecision = 15;
// The configurations of the reference implementation's own encoding
// cases: a sparse precision just above the normal one; the widest
// normal precision against a sparse precision below the encoding
// maximum, where the flag position is decided by the normal term; the
// narrowest normal precision the encoding serves; and the pair of
// downgrades, which move a value to fewer normal bits and then to
// fewer sparse bits.
constexpr int32_t kPlainCaseSparsePrecision = 7;
constexpr int32_t kAsymmetricSparsePrecision = 26;
constexpr int32_t kNarrowestNormalPrecision = 1;
constexpr int32_t kNarrowestCaseSparsePrecision = 5;
constexpr int32_t kDowngradeNormalPrecision = 3;
constexpr int32_t kDowngradeSparsePrecision = 5;
constexpr int32_t kDowngradeFewerNormalBits = 2;
constexpr int32_t kDowngradeFewerSparseBits = 4;

constexpr uint32_t Bit(int32_t position) {
  return uint32_t{1} << static_cast<uint32_t>(position);
}

// The encoding packs a value into a non-negative int32, which is what
// the difference encoder writes. At the encoding's own maxima the
// widest packed value is exactly the largest positive int32; one
// precision higher on either axis would set the sign bit.
TEST(SparseEncodingTest, WidestEncodingIsTheLargestPositiveInt32) {
  auto enc = Sparse::Create(kMaxNormal, kMaxSparse);
  ASSERT_TRUE(enc.has_value());

  // The widest value has a full normal index and a full rho field, so
  // its sparse index has every mask bit clear.
  const uint32_t widest_index = Bit(kMaxSparse) - Bit(Sparse::kRhoWBits);

  const uint32_t widest = enc->EncodeParts(widest_index, kFullRhoW);
  EXPECT_EQ(widest, Sparse::kWidestEncoding);
  EXPECT_EQ(static_cast<int32_t>(widest), std::numeric_limits<int32_t>::max());
}

// A sparse index with any mask bit set is stored as itself, without
// the flag and without a rho field.
TEST(SparseEncodingTest, PlainFormStoresTheSparseIndexItself) {
  auto enc = Sparse::Create(kNormalDominatesNormalPrecision, kMaxSparse);
  ASSERT_TRUE(enc.has_value());

  const uint32_t index = Bit(kMaxSparse) - 1;
  const uint32_t plain = enc->EncodeParts(index, kFullRhoW);
  EXPECT_EQ(plain, index);
  EXPECT_EQ(enc->DecodeSparseIndex(plain), index);
  EXPECT_EQ(enc->DecodeSparseRhoWIfPresent(plain), 0);
}

// When the two precisions are equal the mask is empty, so every value
// carries the flag. This is the branch most easily left wrong.
TEST(SparseEncodingTest, EqualPrecisionsAlwaysTakeTheFlagForm) {
  auto enc = Sparse::Create(kEqualPrecision, kEqualPrecision);
  ASSERT_TRUE(enc.has_value());

  const uint32_t index = kEqualPrecision;
  const uint32_t rho_w = kFullRhoW - 1;
  const uint32_t encoded = enc->EncodeParts(index, rho_w);
  const uint32_t flag = Bit(kEqualPrecision + Sparse::kRhoWBits);
  EXPECT_EQ(encoded, flag | (index << kRhoWBits) | rho_w);
  EXPECT_EQ(enc->DecodeSparseIndex(encoded), index);
  EXPECT_EQ(enc->DecodeNormalIndex(encoded), index);
  EXPECT_EQ(enc->DecodeSparseRhoWIfPresent(encoded), rho_w);

  // The reference's own case for this configuration, driven from a
  // hash: the flag sits above the normal index and the rank.
  EXPECT_EQ(enc->Encode(0b10111ULL << 59U), (1U << 9U) | (0b101U << 6U) | 1U);
}

// The flag sits at the greater of the sparse precision and the normal
// precision plus the rho width, and either term can dominate.
TEST(SparseEncodingTest, FlagPositionTakesWhicheverTermDominates) {
  auto normal_dominates = Sparse::Create(kNormalDominatesNormalPrecision,
                                         kNormalDominatesSparsePrecision);
  ASSERT_TRUE(normal_dominates.has_value());
  EXPECT_EQ(normal_dominates->EncodeParts(0, 1),
            Bit(kNormalDominatesNormalPrecision + Sparse::kRhoWBits) | 1U);

  auto sparse_dominates = Sparse::Create(kSparseDominatesNormalPrecision,
                                         kSparseDominatesSparsePrecision);
  ASSERT_TRUE(sparse_dominates.has_value());
  EXPECT_EQ(sparse_dominates->EncodeParts(0, 1),
            Bit(kSparseDominatesSparsePrecision) | 1U);

  // The reference's own two cases for the same two configurations,
  // driven from a hash and asserting its literal packing.
  EXPECT_EQ(normal_dominates->Encode(0b101100001ULL << 55U),
            (1U << 10U) | (0b1011U << 6U) | 2U);
  EXPECT_EQ(sparse_dominates->Encode(0b110000000001ULL << 52U),
            (1U << 9U) | (0b11U << 6U) | 3U);
}

TEST(SparseEncodingTest, DecodeRecoversWhatEncodeStored) {
  auto enc = Sparse::Create(kOrdinaryNormalPrecision, kOrdinarySparsePrecision);
  ASSERT_TRUE(enc.has_value());

  const auto mask_width = static_cast<uint32_t>(kOrdinarySparsePrecision -
                                                kOrdinaryNormalPrecision);
  const uint32_t rho_w = kFullRhoW - 1;

  // An index with every mask bit clear takes the flag form and stores
  // the rho field.
  const uint32_t flagged_index = Bit(static_cast<int32_t>(mask_width) + 1);
  const uint32_t flagged = enc->EncodeParts(flagged_index, rho_w);
  EXPECT_EQ(enc->DecodeSparseIndex(flagged), flagged_index);
  EXPECT_EQ(enc->DecodeNormalIndex(flagged), flagged_index >> mask_width);
  EXPECT_EQ(enc->DecodeSparseRhoWIfPresent(flagged), rho_w);

  // An index with a mask bit set takes the plain form.
  const uint32_t plain_index = flagged_index + 1;
  const uint32_t plain = enc->EncodeParts(plain_index, rho_w);
  EXPECT_EQ(plain, plain_index);
  EXPECT_EQ(enc->DecodeSparseIndex(plain), plain_index);
  EXPECT_EQ(enc->DecodeNormalIndex(plain), plain_index >> mask_width);
}

// The encoding accepts precisions a sketch may not use: its limits
// bound what the packing can represent, and the compatibility limit is
// enforced above it.
TEST(SparseEncodingTest, CreateBoundsAreTheRepresentableOnes) {
  EXPECT_TRUE(Sparse::Create(kMaxNormal, kMaxSparse).has_value());

  auto too_wide = Sparse::Create(kMaxNormal, kMaxSparse + 1);
  ASSERT_FALSE(too_wide.has_value());
  EXPECT_EQ(too_wide.error().code, utils::ErrorCode::kIllegalArgument);

  auto too_deep = Sparse::Create(kMaxNormal + 1, kMaxSparse);
  ASSERT_FALSE(too_deep.has_value());
  EXPECT_EQ(too_deep.error().code, utils::ErrorCode::kIllegalArgument);

  auto inverted =
      Sparse::Create(kOrdinaryNormalPrecision, kOrdinaryNormalPrecision - 1);
  ASSERT_FALSE(inverted.has_value());
  EXPECT_EQ(inverted.error().code, utils::ErrorCode::kIllegalArgument);
}

// The cases below are the reference implementation's own, ported from
// its EncodingSparseTest, and they drive Encode from a hash rather
// than assembling the parts, which is how the library is used.
TEST(SparseEncodingTest, EncodeFromHashMatchesTheReference) {
  // A sparse index whose low bits are set: stored plainly.
  auto without_rho = Sparse::Create(kNormalDominatesNormalPrecision,
                                    kPlainCaseSparsePrecision);
  ASSERT_TRUE(without_rho.has_value());
  EXPECT_EQ(without_rho->Encode(0b101100101ULL << 55U), 0b1011001U);

  // The same hash at the widest sparse precision the encoding allows.
  auto widest = Sparse::Create(kNormalDominatesNormalPrecision, kMaxSparse);
  ASSERT_TRUE(widest.has_value());
  EXPECT_EQ(widest->Encode(0b101100101ULL << 55U), 0b101100101U << 21U);

  // At the widest normal precision the flag sits above the normal
  // index and the count of zero bits that follow the sparse index.
  auto widest_normal = Sparse::Create(kMaxNormal, kAsymmetricSparsePrecision);
  ASSERT_TRUE(widest_normal.has_value());
  EXPECT_EQ(widest_normal->Encode(0b101ULL << 61U),
            (1U << 30U) | (0b101U << 27U) | 39U);

  // At the narrowest normal precision the encoding serves.
  auto narrowest_normal =
      Sparse::Create(kNarrowestNormalPrecision, kNarrowestCaseSparsePrecision);
  ASSERT_TRUE(narrowest_normal.has_value());
  EXPECT_EQ(narrowest_normal->Encode(1ULL << 63U),
            (1U << 7U) | (1U << 6U) | 60U);
}

// Downgrading moves a value between encodings, in both directions
// across the flag. These are the reference's own two cases.
TEST(SparseEncodingTest, DowngradeMatchesTheReferenceInBothDirections) {
  auto source =
      Sparse::Create(kDowngradeNormalPrecision, kDowngradeSparsePrecision);
  ASSERT_TRUE(source.has_value());

  auto fewer_normal_bits =
      Sparse::Create(kDowngradeFewerNormalBits, kDowngradeSparsePrecision);
  ASSERT_TRUE(fewer_normal_bits.has_value());
  EXPECT_EQ(source->DowngradeSparseValue((1U << 9U) | (0b111U << 6U) | 2U,
                                         *fewer_normal_bits),
            0b11100U);

  auto fewer_sparse_bits =
      Sparse::Create(kDowngradeNormalPrecision, kDowngradeFewerSparseBits);
  ASSERT_TRUE(fewer_sparse_bits.has_value());
  EXPECT_EQ(source->DowngradeSparseValue(0b11101U, *fewer_sparse_bits),
            (1U << 9U) | (0b111U << 6U) | 1U);
}

// A plain value at equal precisions recomputes its normal rho from
// the index, shifting by the full width of the value. The reference's
// language reduces that shift modulo the width; C++ does not, so the
// encoder reduces it explicitly. Deserialised bytes can present this
// value, so it must not be left undefined.
TEST(SparseEncodingTest, NormalRhoWAtEqualPrecisionsMatchesTheReference) {
  auto enc = Sparse::Create(kEqualPrecision, kEqualPrecision);
  ASSERT_TRUE(enc.has_value());

  // The flag is at bit nine, so this value carries none and is plain.
  // Its rho is the count of leading zeros of the value itself, plus
  // one: 61 + 1.
  EXPECT_EQ(enc->DecodeNormalRhoW(5), 62);

  // A flagged value stores its rho, to which the mask width is added.
  auto wider =
      Sparse::Create(kOrdinaryNormalPrecision, kOrdinarySparsePrecision);
  ASSERT_TRUE(wider.has_value());
  const uint32_t flagged = wider->EncodeParts(Bit(6), 9);
  EXPECT_EQ(wider->DecodeNormalRhoW(flagged),
            9 + kOrdinarySparsePrecision - kOrdinaryNormalPrecision);
}

// The normal encoding splits a hash into an index and a rank, and
// downgrades both to a lower precision.
TEST(NormalEncodingTest, SplitsAndDowngradesAHash) {
  auto enc = Normal::Create(kNormalDominatesNormalPrecision);
  ASSERT_TRUE(enc.has_value());

  // The index is the top four bits; the remainder is all zero, so the
  // rank is the width of that remainder plus one.
  EXPECT_EQ(enc->Index(1ULL << 63U), 0b1000U);
  EXPECT_EQ(enc->RhoW(1ULL << 63U), 61);

  // One bit set immediately below the index gives a rank of one.
  EXPECT_EQ(enc->RhoW((1ULL << 63U) | (1ULL << 59U)), 1);

  auto lower = Normal::Create(kNormalDominatesNormalPrecision - 1);
  ASSERT_TRUE(lower.has_value());
  EXPECT_EQ(enc->DowngradeIndex(0b1011U, lower->precision()), 0b101U);
  // The bit the index loses becomes the first bit of the remainder.
  // Where that bit is set the rank becomes one; where it is clear the
  // rank grows by the number of bits dropped.
  EXPECT_EQ(enc->DowngradeRhoW(0b1011U, 3, lower->precision()), 1);
  EXPECT_EQ(enc->DowngradeRhoW(0b1010U, 3, lower->precision()), 4);
}

}  // namespace
}  // namespace zetasketch::hll::encoding
