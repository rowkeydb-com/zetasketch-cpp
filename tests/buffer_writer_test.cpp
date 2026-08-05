#include <gtest/gtest.h>
#include "zetasketch/utils/buffer_traits.h"

TEST(BufferWriterTest, Compilation) {
  // Just a compilation check
  const zetasketch::utils::BufferWriter writer;
  EXPECT_EQ(writer.size(), 0);
}
