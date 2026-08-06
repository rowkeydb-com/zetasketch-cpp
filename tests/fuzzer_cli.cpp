#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "zetasketch/hyperloglogplusplus.h"

namespace {

using zetasketch::HyperLogLogPlusPlus;

// NOLINTNEXTLINE(readability-identifier-naming)
void print_hex(const std::vector<uint8_t>& data) {
  for (const uint8_t byte : data) {
    std::cout << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(byte);
  }
  std::cout << std::endl;
}

uint8_t ParseHexByte(std::string_view hex_byte) {
  uint32_t val = 0;
  constexpr uint32_t kHexAlphaOffset = 10;
  for (const char c : hex_byte) {
    val <<= 4U;
    if (c >= '0' && c <= '9') {
      val += static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      val += static_cast<uint32_t>(c - 'a') + kHexAlphaOffset;
    } else if (c >= 'A' && c <= 'F') {
      val += static_cast<uint32_t>(c - 'A') + kHexAlphaOffset;
    }
  }
  return static_cast<uint8_t>(val);
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
int main(int argc, char** argv) {
  if (argc < 2) return 1;
  const std::string mode = argv[1];

  if (mode == "CREATE") {
    // We expect at least normal_precision and sparse_precision
    if (argc < 4) return 1;
    const int normal_precision = std::stoi(argv[2]);
    const int sparse_precision = std::stoi(argv[3]);

    auto hll_res =
        HyperLogLogPlusPlus::Create(normal_precision, sparse_precision);
    if (!hll_res.has_value()) return 1;

    auto hll = std::move(hll_res.value());

    std::string line;
    while (std::getline(std::cin, line)) {
      if (!line.empty()) {
        hll.Add(line);
      }
    }

    auto ser = hll.Serialize();
    if (!ser.has_value()) {
      std::cerr << "Serialize failed: " << ser.error().message << std::endl;
      return 1;
    }

    print_hex(ser.value());
  } else if (mode == "MERGE") {
    // We expect at least normal_precision and sparse_precision
    if (argc < 4) return 1;
    const int normal_precision = std::stoi(argv[2]);
    const int sparse_precision = std::stoi(argv[3]);

    auto hll_res =
        HyperLogLogPlusPlus::Create(normal_precision, sparse_precision);
    if (!hll_res.has_value()) return 1;

    auto hll = std::move(hll_res.value());

    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;

      std::vector<uint8_t> data;
      data.reserve(line.length() / 2);

      for (size_t i = 0; i < line.length(); i += 2) {
        data.push_back(ParseHexByte(std::string_view(line).substr(i, 2)));
      }

      auto other_res = HyperLogLogPlusPlus::FromBytes(data);
      if (!other_res.has_value()) {
        std::cerr << "FromBytes failed" << std::endl;
        return 1;
      }

      auto merge_res = hll.Merge(std::move(other_res.value()));
      if (!merge_res.has_value()) {
        std::cerr << "Merge failed: " << merge_res.error().message << std::endl;
        return 1;
      }
    }

    auto ser = hll.Serialize();
    if (!ser.has_value()) return 1;

    print_hex(ser.value());
  }

  return 0;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
