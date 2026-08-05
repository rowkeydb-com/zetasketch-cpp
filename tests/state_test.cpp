// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#include "zetasketch/hll/state.h"
#include <gtest/gtest.h>
#include "hllplusplus.pb.h"
#include "aggregator.pb.h"

namespace zetasketch::hll {
namespace {

TEST(StateTest, ExplicitSetSemanticsForDefaults) {
  State state;
  state.type = zetasketch::HYPERLOGLOG_PLUS_UNIQUE;
  state.num_values = 0;
  state.encoding_version = 2;
  state.value_type = ValueType::kUnknown;

  // Set these to 0 explicitly.
  state.sparse_size = 0;
  state.precision = 0;
  state.sparse_precision = 0;

  auto bytes_result = state.ToByteArray();
  ASSERT_TRUE(bytes_result.has_value());

  auto parsed_result = State::Parse(bytes_result.value());
  ASSERT_TRUE(parsed_result.has_value());

  const State& parsed = parsed_result.value();
  EXPECT_EQ(parsed.sparse_size, 0);
  EXPECT_EQ(parsed.precision, 0);
  EXPECT_EQ(parsed.sparse_precision, 0);
  EXPECT_EQ(parsed.encoding_version, 2);
  EXPECT_EQ(parsed.type, zetasketch::HYPERLOGLOG_PLUS_UNIQUE);

  // Verify that the proto fields are actually present in the serialized output.
  zetasketch::AggregatorStateProto raw_proto;
  ASSERT_TRUE(
      raw_proto.ParseFromArray(bytes_result->data(), bytes_result->size()));
  ASSERT_TRUE(raw_proto.HasExtension(zetasketch::hyperloglogplus_unique_state));
  const auto& hll_ext =
      raw_proto.GetExtension(zetasketch::hyperloglogplus_unique_state);

  EXPECT_TRUE(hll_ext.has_sparse_size());
  EXPECT_TRUE(hll_ext.has_precision_or_num_buckets());
  EXPECT_TRUE(hll_ext.has_sparse_precision_or_num_buckets());
}

}  // namespace
}  // namespace zetasketch::hll
