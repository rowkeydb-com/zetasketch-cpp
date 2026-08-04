// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/normal_representation.h"
#include <cstdint>
#include <utility>
#include <gtest/gtest.h>
#include "zetasketch/hll/encoding.h"
#include "zetasketch/hll/state.h"

namespace zetasketch::hll {
namespace {

constexpr int32_t kP14 = 14;
constexpr int32_t kP10 = 10;
constexpr int32_t kP11 = 11;
constexpr int32_t kSp15 = 15;
constexpr int32_t kSp13 = 13;
constexpr uint32_t kSparseValue = 0b0000000000001;
constexpr uint64_t kTestHash = 0x1234567890ABCDEF;

TEST(NormalRepresentationTest, BasicEstimate) {
  State state;
  state.precision = kP14;
  state.sparse_precision = kP14;

  auto repr = NormalRepresentation::Create(std::move(state));
  ASSERT_TRUE(repr.has_value());

  auto est = repr->Estimate();
  ASSERT_TRUE(est.has_value());
  EXPECT_EQ(*est, 0);

  auto add_res = repr->AddHash(kTestHash);
  ASSERT_TRUE(add_res.has_value());

  est = repr->Estimate();
  ASSERT_TRUE(est.has_value());
  EXPECT_GT(*est, 0);
}

TEST(NormalRepresentationTest, AddSparseValueDowngradesSparsePrecision) {
  State state;
  state.precision = kP10;
  state.sparse_precision = kSp15;
  auto repr = NormalRepresentation::Create(std::move(state));
  ASSERT_TRUE(repr.has_value());

  auto sparse_encoding = encoding::Sparse::Create(kP10, kSp13);
  ASSERT_TRUE(sparse_encoding.has_value());

  auto add_res = repr->AddSparseValue(*sparse_encoding, kSparseValue);
  ASSERT_TRUE(add_res.has_value());

  EXPECT_EQ(repr->state().sparse_precision, kSp13);
}

TEST(NormalRepresentationTest, AddSparseValueHigherPrecision) {
  State state;
  state.precision = kP10;
  state.sparse_precision = kSp15;
  auto repr = NormalRepresentation::Create(std::move(state));
  ASSERT_TRUE(repr.has_value());

  auto source_sparse_encoding = encoding::Sparse::Create(kP11, kSp13);
  ASSERT_TRUE(source_sparse_encoding.has_value());

  const uint32_t sparse_value = kSparseValue;
  auto add_res = repr->AddSparseValue(*source_sparse_encoding, sparse_value);
  ASSERT_TRUE(add_res.has_value());

  auto target_normal_encoding = encoding::Normal::Create(kP10);
  ASSERT_TRUE(target_normal_encoding.has_value());

  const uint32_t new_index = source_sparse_encoding->normal().DowngradeIndex(
      source_sparse_encoding->DecodeNormalIndex(sparse_value),
      target_normal_encoding->precision());
  const uint8_t new_rho_w = source_sparse_encoding->normal().DowngradeRhoW(
      source_sparse_encoding->DecodeNormalIndex(sparse_value),
      source_sparse_encoding->DecodeNormalRhoW(sparse_value),
      target_normal_encoding->precision());

  ASSERT_TRUE(repr->state().data.has_value());
  const auto& data =
      repr->state().data.value();  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(data[new_index], new_rho_w);

  EXPECT_EQ(repr->state().precision, kP10);
  EXPECT_EQ(repr->state().sparse_precision, kSp13);
}

TEST(NormalRepresentationTest, AddSparseValueLowerPrecision) {
  State state;
  state.precision = kP11;
  state.sparse_precision = kSp15;
  auto repr = NormalRepresentation::Create(std::move(state));
  ASSERT_TRUE(repr.has_value());

  auto source_sparse_encoding = encoding::Sparse::Create(kP10, kSp13);
  ASSERT_TRUE(source_sparse_encoding.has_value());

  const uint32_t sparse_value = kSparseValue;
  auto add_res = repr->AddSparseValue(*source_sparse_encoding, sparse_value);
  ASSERT_TRUE(add_res.has_value());

  EXPECT_EQ(repr->state().precision, kP10);
  EXPECT_EQ(repr->state().sparse_precision, kSp13);
}

}  // namespace
}  // namespace zetasketch::hll
