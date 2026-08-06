#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "zetasketch/hyperloglogplusplus.h"

namespace zetasketch {
namespace {

constexpr size_t kTypeIndex = 0;
constexpr size_t kNameIndex = 1;
constexpr size_t kSketchB64Index = 2;
constexpr size_t kSketchExpectedCardinalityIndex = 3;

constexpr size_t kMergeB641Index = 2;
constexpr size_t kMergeB642Index = 3;
constexpr size_t kMergeB64MergedIndex = 4;
constexpr size_t kMergeExpectedCardinalityIndex = 5;
constexpr size_t kMergeTokensCount = 6;

std::string Base64Decode(const std::string& encoded_string) {
  // NOLINTBEGIN(readability-magic-numbers,
  // cppcoreguidelines-avoid-magic-numbers, hicpp-signed-bitwise,
  // misc-const-correctness)
  static const std::string kBase64Chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789+/";

  std::vector<int> char_lookup(256, -1);
  for (int i = 0; i < 64; i++) char_lookup[kBase64Chars[i]] = i;

  std::string decoded;
  int val = 0;
  int valb = -8;
  for (unsigned char c : encoded_string) {
    if (char_lookup[c] == -1) break;
    val = (val << 6) + char_lookup[c];
    valb += 6;
    if (valb >= 0) {
      decoded.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return decoded;
  // NOLINTEND(readability-magic-numbers, cppcoreguidelines-avoid-magic-numbers,
  // hicpp-signed-bitwise, misc-const-correctness)
}

std::string Base64Encode(const std::string& str) {
  // NOLINTBEGIN(readability-magic-numbers,
  // cppcoreguidelines-avoid-magic-numbers, hicpp-signed-bitwise,
  // misc-const-correctness)
  static const std::string kBase64Chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789+/";

  std::string ret;
  int val = 0;
  int valb = -6;
  for (unsigned char c : str) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      ret.push_back(kBase64Chars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) ret.push_back(kBase64Chars[((val << 8) >> (valb + 8)) & 0x3F]);
  while (ret.size() % 4) ret.push_back('=');
  return ret;
  // NOLINTEND(readability-magic-numbers, cppcoreguidelines-avoid-magic-numbers,
  // hicpp-signed-bitwise, misc-const-correctness)
}

std::vector<std::string> SplitString(const std::string& str, char delimiter) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream token_stream(str);
  while (std::getline(token_stream, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

TEST(GoldenCorpusTest, VerifyParity) {
  std::ifstream file("tests/golden_corpus.tsv");
  ASSERT_TRUE(file.is_open()) << "Failed to open golden_corpus.tsv. Make sure "
                                 "to run the test from the workspace root.";

  std::string line;
  int line_num = 0;
  while (std::getline(file, line)) {
    line_num++;
    if (line.empty()) continue;

    std::vector<std::string> tokens = SplitString(line, '\t');
    ASSERT_GE(tokens.size(), 4) << "Malformed line at " << line_num;

    const std::string& type = tokens[kTypeIndex];
    const std::string& name = tokens[kNameIndex];

    if (type == "SKETCH") {
      const std::string& b64 = tokens[kSketchB64Index];
      const int64_t expected_cardinality =
          std::stoll(tokens[kSketchExpectedCardinalityIndex]);

      const std::string bytes = Base64Decode(b64);
      // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
      auto sketch_or = HyperLogLogPlusPlus::FromBytes(std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      ASSERT_TRUE(sketch_or.has_value())
          << "Failed to deserialize SKETCH " << name;
      auto sketch = std::move(sketch_or.value());

      EXPECT_EQ(sketch.Result(), expected_cardinality)
          << "Cardinality mismatch for " << name;

      auto serialized_or = sketch.Serialize();
      ASSERT_TRUE(serialized_or.has_value())
          << "Failed to serialize SKETCH " << name;
      const std::string serialized_str(serialized_or.value().begin(),
                                       serialized_or.value().end());
      const std::string reserialized_b64 = Base64Encode(serialized_str);

      EXPECT_EQ(reserialized_b64, b64)
          << "Bit-exact serialization parity failed for SKETCH " << name;
    } else if (type == "MERGE") {
      ASSERT_EQ(tokens.size(), kMergeTokensCount)
          << "Malformed MERGE line at " << line_num;
      const std::string& b64_1 = tokens[kMergeB641Index];
      const std::string& b64_2 = tokens[kMergeB642Index];
      const std::string& b64_merged = tokens[kMergeB64MergedIndex];
      const int64_t expected_cardinality =
          std::stoll(tokens[kMergeExpectedCardinalityIndex]);

      const std::string bytes1 = Base64Decode(b64_1);
      // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
      auto sketch1_or = HyperLogLogPlusPlus::FromBytes(std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(bytes1.data()), bytes1.size()));
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      ASSERT_TRUE(sketch1_or.has_value())
          << "Failed to deserialize sketch1 for " << name;
      auto sketch1 = std::move(sketch1_or.value());

      const std::string bytes2 = Base64Decode(b64_2);
      // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
      auto sketch2_or = HyperLogLogPlusPlus::FromBytes(std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(bytes2.data()), bytes2.size()));
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      ASSERT_TRUE(sketch2_or.has_value())
          << "Failed to deserialize sketch2 for " << name;
      auto sketch2 = std::move(sketch2_or.value());

      auto merge_res = sketch1.Merge(std::move(sketch2));
      ASSERT_TRUE(merge_res.has_value()) << "Failed to merge " << name;

      EXPECT_EQ(sketch1.Result(), expected_cardinality)
          << "Merged cardinality mismatch for " << name;

      auto serialized_or = sketch1.Serialize();
      ASSERT_TRUE(serialized_or.has_value())
          << "Failed to serialize merged sketch for " << name;
      const std::string serialized_str(serialized_or.value().begin(),
                                       serialized_or.value().end());
      const std::string reserialized_b64 = Base64Encode(serialized_str);

      EXPECT_EQ(reserialized_b64, b64_merged)
          << "Bit-exact serialization parity failed for MERGE " << name;
    } else {
      FAIL() << "Unknown type " << type << " at line " << line_num;
    }
  }
}

}  // namespace
}  // namespace zetasketch
