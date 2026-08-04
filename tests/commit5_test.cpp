#include "zetasketch/utils/buffer_traits.h"
#include <gtest/gtest.h>

TEST(Commit5Test, Compilation) {
  // Just a compilation check
  const zetasketch::utils::BufferWriter writer;
  EXPECT_EQ(writer.size(), 0);
}
