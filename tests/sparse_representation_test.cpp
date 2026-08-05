// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/sparse_representation.h"
#include <cstdint>
#include <expected>
#include <utility>
#include <variant>
#include <gtest/gtest.h>
#include "zetasketch/hll/encoding.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/utils/buffer_traits.h"

namespace zetasketch::hll {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers,misc-redundant-expression,bugprone-unchecked-optional-access)

std::expected<SparseRepresentation, utils::Error> CreateSparse(
    int32_t normal_precision, int32_t sparse_precision) {
  State state;
  state.precision = normal_precision;
  state.sparse_precision = sparse_precision;
  state.sparse_size = 0;
  return SparseRepresentation::Create(std::move(state));
}

TEST(SparseRepresentationTest, AddHash) {
  auto repr_res = CreateSparse(10, 13);
  ASSERT_TRUE(repr_res.has_value());
  auto repr = std::move(repr_res.value());

  auto hash_res = std::move(repr).AddHash(0x123456789ABCDEF0);
  ASSERT_TRUE(hash_res.has_value());
}

TEST(SparseRepresentationTest, AddSparseValueSamePrecision) {
  auto repr_res = CreateSparse(10, 13);
  ASSERT_TRUE(repr_res.has_value());
  auto repr = std::move(repr_res.value());

  auto enc_res = encoding::Sparse::Create(10, 13);
  ASSERT_TRUE(enc_res.has_value());
  auto source_encoding = enc_res.value();

  const uint32_t sparse_value = 0b000000000011111;

  auto union_res =
      std::move(repr).AddSparseValue(source_encoding, sparse_value);
  ASSERT_TRUE(union_res.has_value());
  auto repr_union = std::move(union_res.value());

  ASSERT_TRUE(std::holds_alternative<SparseRepresentation>(repr_union));
  auto& s_repr = std::get<SparseRepresentation>(repr_union);

  auto compact_res = std::move(s_repr).Compact();
  ASSERT_TRUE(compact_res.has_value());
  repr_union = std::move(compact_res.value());

  ASSERT_TRUE(std::holds_alternative<SparseRepresentation>(repr_union));
  auto& final_repr = std::get<SparseRepresentation>(repr_union);

  EXPECT_EQ(final_repr.state().precision, 10);
  EXPECT_EQ(final_repr.state().sparse_precision, 13);

  ASSERT_TRUE(final_repr.state().sparse_data.has_value());
}

TEST(SparseRepresentationTest, Estimate) {
  auto repr_res = CreateSparse(10, 13);
  ASSERT_TRUE(repr_res.has_value());
  auto repr = std::move(repr_res.value());

  auto hash_res = std::move(repr).AddHash(0x123456789ABCDEF0);
  ASSERT_TRUE(hash_res.has_value());
  Representation repr_union = std::move(hash_res.value());

  ASSERT_TRUE(std::holds_alternative<SparseRepresentation>(repr_union));
  auto& s_repr = std::get<SparseRepresentation>(repr_union);
  auto est = s_repr.Estimate();
  ASSERT_TRUE(est.has_value());
  EXPECT_GT(est.value(), 0);
}

}  // namespace
// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers,misc-redundant-expression,bugprone-unchecked-optional-access)
}  // namespace zetasketch::hll