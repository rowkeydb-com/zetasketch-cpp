#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
// NOLINTNEXTLINE(misc-include-cleaner)
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/wait.h>

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

#include <gtest/gtest.h>
#include "zetasketch/hyperloglogplusplus.h"

namespace {

using zetasketch::HyperLogLogPlusPlus;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,fuchsia-statically-constructed-objects)
std::string g_java_cli;

std::string PrintHex(const std::vector<uint8_t>& data) {
  std::ostringstream oss;
  for (const uint8_t byte : data) {
    oss << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(byte);
  }
  return oss.str();
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

std::string RunJava(const std::string& mode, int np, int sp,
                    const std::string& input_data) {
  if (g_java_cli.empty()) {
    ADD_FAILURE() << "JAVA_CLI path is empty";
    return "";
  }

  std::array<int, 2> pipefd{};
  if (pipe(pipefd.data()) == -1) {
    ADD_FAILURE() << "pipe failed: " << errno;
    return "";
  }

  std::array<int, 2> outpipefd{};
  if (pipe(outpipefd.data()) == -1) {
    ADD_FAILURE() << "outpipe failed: " << errno;
    return "";
  }

  const pid_t pid = fork();
  if (pid == -1) {
    ADD_FAILURE() << "fork failed: " << errno;
    return "";
  }

  if (pid == 0) {
    // Child process
    close(pipefd[1]);
    if (dup2(pipefd[0], STDIN_FILENO) == -1) _exit(1);
    close(pipefd[0]);

    close(outpipefd[0]);
    if (dup2(outpipefd[1], STDOUT_FILENO) == -1) _exit(1);
    close(outpipefd[1]);

    const std::string np_str = std::to_string(np);
    const std::string sp_str = std::to_string(sp);

    std::vector<char*> args;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    args.push_back(const_cast<char*>(g_java_cli.c_str()));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    args.push_back(const_cast<char*>(mode.c_str()));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    args.push_back(const_cast<char*>(np_str.c_str()));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    args.push_back(const_cast<char*>(sp_str.c_str()));
    args.push_back(nullptr);

    execv(g_java_cli.c_str(), args.data());
    std::cerr << "execv failed: " << errno << "\n";
    _exit(1);
  }

  // Parent process
  close(pipefd[0]);
  close(outpipefd[1]);

  const ssize_t written =
      write(pipefd[1], input_data.data(), input_data.size());
  if (std::cmp_not_equal(written, input_data.size())) {
    ADD_FAILURE() << "Failed to write entire input to child process";
  }
  close(pipefd[1]);

  std::string result;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t bytes_read = read(outpipefd[0], buffer.data(), buffer.size());
    if (bytes_read > 0) {
      result.append(buffer.data(), bytes_read);
    } else if (bytes_read == 0) {
      break;
    } else {
      if (errno == EINTR) continue;
      ADD_FAILURE() << "read failed: " << errno;
      break;
    }
  }
  close(outpipefd[0]);

  int wstatus = 0;
  waitpid(pid, &wstatus, 0);

  // NOLINTNEXTLINE(misc-include-cleaner)
  if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
    ADD_FAILURE() << "Java process exited with error";
  }

  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }
  return result;
}

std::string CppCreate(int np, int sp, const std::vector<std::string>& items) {
  auto hll_res = HyperLogLogPlusPlus::Create(np, sp);
  EXPECT_TRUE(hll_res.has_value());
  auto hll = std::move(hll_res.value());
  for (const auto& item : items) {
    EXPECT_TRUE(hll.Add(item).has_value());
  }
  auto ser = hll.Serialize();
  EXPECT_TRUE(ser.has_value());
  return PrintHex(ser.value());
}

std::string CppMerge(int np, int sp,
                     const std::vector<std::string>& hex_sketches) {
  auto hll_res = HyperLogLogPlusPlus::Create(np, sp);
  EXPECT_TRUE(hll_res.has_value());
  auto hll = std::move(hll_res.value());

  for (const auto& line : hex_sketches) {
    std::vector<uint8_t> data;
    data.reserve(line.length() / 2);
    for (size_t i = 0; i < line.length(); i += 2) {
      data.push_back(ParseHexByte(std::string_view(line).substr(i, 2)));
    }
    auto other_res = HyperLogLogPlusPlus::FromBytes(data);
    EXPECT_TRUE(other_res.has_value());
    auto merge_res = hll.Merge(std::move(other_res.value()));
    EXPECT_TRUE(merge_res.has_value());
  }
  auto ser = hll.Serialize();
  EXPECT_TRUE(ser.has_value());
  return PrintHex(ser.value());
}

class DifferentialFuzzerTest
    : public ::testing::TestWithParam<std::pair<int, int>> {
 protected:
  static std::pair<std::vector<std::string>, std::string> GenerateTestItems(
      const std::string& prefix, int count) {
    std::vector<std::string> items;
    items.reserve(count);
    std::string input_data;
    for (int i = 0; i < count; ++i) {
      const std::string item = prefix + std::to_string(i);
      items.push_back(item);
      input_data += item + "\n";
    }
    return {items, input_data};
  }
};

TEST_P(DifferentialFuzzerTest, Create) {
  const int np = GetParam().first;
  const int sp = GetParam().second;

  const std::vector<int> counts = {10, 100, 1000, 5000};
  for (const int num : counts) {
    auto [items, input_data] = GenerateTestItems("item_", num);

    const std::string cpp_out = CppCreate(np, sp, items);
    const std::string java_out = RunJava("CREATE", np, sp, input_data);
    EXPECT_EQ(cpp_out, java_out)
        << "Mismatch at NP=" << np << " SP=" << sp << " Elements=" << num;
  }
}

TEST_P(DifferentialFuzzerTest, Merge) {
  const int np = GetParam().first;
  const int sp = GetParam().second;

  const std::vector<std::pair<int, int>> merge_configs = {
      {3, 100},   // sparse
      {3, 2000},  // normal
      {10, 200}   // mixed
  };

  for (const auto& config : merge_configs) {
    const int num_sketches = config.first;
    const int items_per_sketch = config.second;

    std::vector<std::string> cpp_hexes;
    std::string java_merge_in;

    for (int i = 0; i < num_sketches; ++i) {
      const std::string prefix = "test_" + std::to_string(i) + "_";
      auto [items, input_data] = GenerateTestItems(prefix, items_per_sketch);

      cpp_hexes.push_back(CppCreate(np, sp, items));
      java_merge_in += RunJava("CREATE", np, sp, input_data) + "\n";
    }

    const std::string cpp_merged = CppMerge(np, sp, cpp_hexes);
    const std::string java_merged = RunJava("MERGE", np, sp, java_merge_in);

    EXPECT_EQ(cpp_merged, java_merged)
        << "Mismatch at Merge NP=" << np << " SP=" << sp;
  }
}

INSTANTIATE_TEST_SUITE_P(Configs, DifferentialFuzzerTest,
                         ::testing::Values(std::make_pair(15, 20),
                                           std::make_pair(10, 15),
                                           std::make_pair(15, 0),
                                           std::make_pair(10, 0)));

}  // namespace

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  constexpr std::string_view kFlag = "--java_cli=";

  const std::span<char*> args_span(argv, argc);
  for (size_t i = 1; i < args_span.size(); ++i) {
    const std::string_view arg = args_span[i];
    if (arg.starts_with(kFlag)) {
      g_java_cli = arg.substr(kFlag.length());
    }
  }

  if (g_java_cli.empty()) {
    std::cerr << "Error: --java_cli= flag is required\n";
    return 1;
  }
  return RUN_ALL_TESTS();
}
