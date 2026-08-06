#include <iostream>
// NOLINTBEGIN
#include <iomanip>
#include <string>
#include <vector>
#include "zetasketch/hyperloglogplusplus.h"

using zetasketch::HyperLogLogPlusPlus;

void print_hex(const std::vector<uint8_t>& data) {
  for (uint8_t b : data) {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)b;
  }
  std::cout << std::endl;
}

int main(int argc, char** argv) {
  if (argc < 2) return 1;
  std::string mode = argv[1];

  if (mode == "CREATE") {
    int normal_precision = std::stoi(argv[2]);
    int sparse_precision = std::stoi(argv[3]);
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
    int normal_precision = std::stoi(argv[2]);
    int sparse_precision = std::stoi(argv[3]);
    auto hll_res =
        HyperLogLogPlusPlus::Create(normal_precision, sparse_precision);
    if (!hll_res.has_value()) return 1;
    auto hll = std::move(hll_res.value());

    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      std::vector<uint8_t> data;
      for (size_t i = 0; i < line.length(); i += 2) {
        std::string byteString = line.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byteString.c_str(), NULL, 16);
        data.push_back(byte);
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
// NOLINTEND
