#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "src/farmhash/farmhash.h"

namespace {

TEST(FarmhashTest, Fingerprint64Test) {
    // These strings represent the known test cases.
    // The exact expected hashes will be verified from the output.
    const std::string_view test_string1 = "Hello, world!";
    const std::string_view test_string2 = "ZetaSketch";
    const std::string_view test_string3;

    const uint64_t hash1 = util::Fingerprint64(test_string1.data(), test_string1.size());
    const uint64_t hash2 = util::Fingerprint64(test_string2.data(), test_string2.size());
    const uint64_t hash3 = util::Fingerprint64(test_string3.data(), test_string3.size());

    EXPECT_EQ(hash1, 3493709964939663943ULL) << "Hash for 'Hello, world!' is incorrect.";
    EXPECT_EQ(hash2, 6842862662275296781ULL) << "Hash for 'ZetaSketch' is incorrect.";
    EXPECT_EQ(hash3, 11160318154034397263ULL) << "Hash for '' is incorrect.";
}

} // namespace
