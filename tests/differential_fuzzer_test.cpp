#include <stdlib.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <system_error>
#include <sys/types.h>
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
#include "zetasketch/bias_data.h"
#include "zetasketch/hll/math_utils.h"
#include "zetasketch/hll/state.h"
#include "zetasketch/hyperloglogplusplus.h"
#include "zetasketch/utils/error.h"

namespace {

using zetasketch::HyperLogLogPlusPlus;

// The path of the reference harness, given as --java_cli= on the
// command line.
std::string& JavaCliFlag() {
  static std::string path;
  return path;
}

// The child reads its input from a file created from this template.
// The path is fixed rather than taken from the environment, reading
// which is not thread safe; the file is unlinked on every path out of
// the function, so nothing is left behind.
constexpr std::string_view kInputTemplate = "/tmp/zetasketch_cli_input_XXXXXX";

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

// Runs the reference harness with the given arguments, feeding it the
// text supplied and returning everything it writes to its output. The
// arguments vary by mode, so they are passed as a list rather than as
// a fixed mode, precision and sparse precision.
std::string RunJavaWithArguments(const std::vector<std::string>& arguments,
                                 const std::string& input_data) {
  if (JavaCliFlag().empty()) {
    ADD_FAILURE() << "JAVA_CLI path is empty";
    return "";
  }

  const std::string path_text(kInputTemplate);
  std::vector<char> input_path(path_text.begin(), path_text.end());
  input_path.push_back('\0');
  const int input_fd = mkstemp(input_path.data());
  if (input_fd == -1) {
    ADD_FAILURE() << "mkstemp failed: " << errno;
    return "";
  }
  if (!input_data.empty()) {
    const ssize_t written =
        write(input_fd, input_data.data(), input_data.size());
    if (std::cmp_not_equal(written, input_data.size())) {
      ADD_FAILURE() << "failed to write the child's input";
    }
  }
  if (lseek(input_fd, 0, SEEK_SET) == -1) {
    ADD_FAILURE() << "lseek failed: " << errno;
  }

  // Releases the input file however the function leaves.
  const auto release_input = [&input_fd, &input_path]() {
    close(input_fd);
    unlink(input_path.data());
  };

  // Built before the fork so that the child does nothing but exec. The
  // strings own the storage the pointers refer to.
  std::vector<std::string> owned_arguments;
  owned_arguments.reserve(arguments.size() + 1);
  owned_arguments.push_back(JavaCliFlag());
  for (const std::string& argument : arguments) {
    owned_arguments.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(owned_arguments.size() + 1);
  for (std::string& argument : owned_arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  std::array<int, 2> outpipefd{};
  if (pipe(outpipefd.data()) == -1) {
    ADD_FAILURE() << "outpipe failed: " << errno;
    release_input();
    return "";
  }

  const pid_t pid = fork();
  if (pid == -1) {
    ADD_FAILURE() << "fork failed: " << errno;
    release_input();
    close(outpipefd[0]);
    close(outpipefd[1]);
    return "";
  }

  if (pid == 0) {
    // Child process
    if (dup2(input_fd, STDIN_FILENO) == -1) _exit(1);
    close(input_fd);

    close(outpipefd[0]);
    if (dup2(outpipefd[1], STDOUT_FILENO) == -1) _exit(1);
    close(outpipefd[1]);

    execv(JavaCliFlag().c_str(), argv.data());
    std::cerr << "execv failed: " << errno << "\n";
    _exit(1);
  }

  // Parent process
  release_input();
  close(outpipefd[1]);

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

  std::string invocation;
  for (const std::string& argument : arguments) {
    invocation += invocation.empty() ? argument : " " + argument;
  }

  int wstatus = 0;
  if (waitpid(pid, &wstatus, 0) == -1) {
    ADD_FAILURE() << "waiting for the reference harness failed for '"
                  << invocation << "': " << errno;
    // NOLINTNEXTLINE(misc-include-cleaner)
  } else if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
    ADD_FAILURE() << "the reference harness failed for '" << invocation
                  << "', wait status " << wstatus;
  }

  return result;
}

std::string RunJava(const std::string& mode, int np, int sp,
                    const std::string& input_data) {
  std::string result = RunJavaWithArguments(
      {mode, std::to_string(np), std::to_string(sp)}, input_data);
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }
  return result;
}

// Encodes a value the way the scripted harness expects its arguments,
// so that a value carrying a space or a newline survives the line-based
// command format.
std::string EncodeBase64(std::string_view value) {
  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((value.size() + 2) / 3) * 4);
  size_t index = 0;
  while (index + 2 < value.size()) {
    const uint32_t triple =
        (static_cast<uint32_t>(static_cast<uint8_t>(value[index])) << 16U) |
        (static_cast<uint32_t>(static_cast<uint8_t>(value[index + 1])) << 8U) |
        static_cast<uint32_t>(static_cast<uint8_t>(value[index + 2]));
    encoded.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
    encoded.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
    encoded.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
    encoded.push_back(kAlphabet[triple & 0x3FU]);
    index += 3;
  }
  const size_t remaining = value.size() - index;
  if (remaining > 0) {
    uint32_t triple = static_cast<uint32_t>(static_cast<uint8_t>(value[index]))
                      << 16U;
    if (remaining == 2) {
      triple |= static_cast<uint32_t>(static_cast<uint8_t>(value[index + 1]))
                << 8U;
    }
    encoded.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
    encoded.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
    encoded.push_back(remaining == 2 ? kAlphabet[(triple >> 6U) & 0x3FU] : '=');
    encoded.push_back('=');
  }
  return encoded;
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

// The receiver is built for the value type its operands carry. The
// reference builds its receiver by reading the first operand, so it
// inherits that type, and a receiver of another type would write a
// different value type into the merged sketch even where every
// register agreed.
std::string CppMerge(int np, int sp,
                     const std::vector<std::string>& hex_sketches,
                     zetasketch::hll::ValueType value_type =
                         zetasketch::hll::ValueType::kBytesOrUtf8String) {
  auto hll_res = HyperLogLogPlusPlus::Create(np, sp, value_type);
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

std::pair<std::vector<std::string>, std::string> MakeItems(
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

int64_t CppEstimate(int np, int sp, const std::vector<std::string>& items) {
  auto hll_res = HyperLogLogPlusPlus::Create(np, sp);
  EXPECT_TRUE(hll_res.has_value());
  auto hll = std::move(hll_res.value());
  for (const auto& item : items) {
    EXPECT_TRUE(hll.Add(item).has_value());
  }
  auto estimate = hll.Result();
  EXPECT_TRUE(estimate.has_value());
  return estimate.value_or(-1);
}

// The published Maven artifact refuses a normal precision below 10;
// the source this suite builds against accepts 4. If the harness is
// ever pointed back at the artifact, this fails immediately rather
// than silently certifying compatibility with a library three years
// out of date.
TEST(ReferenceLibraryTest, AcceptsTheMinimumNormalPrecisionOfFour) {
  const std::string java_out = RunJava("CREATE", 4, 0, "");
  EXPECT_FALSE(java_out.empty());
  EXPECT_EQ(CppCreate(4, 0, {}), java_out);
}

// Splits one line of the transition harness's report, whose fields are
// the verdict, the cardinality and the bytes, separated by tabs.
// Decodes a sketch the harness reported as hexadecimal, as strictly as
// the harness itself reads one: an odd length or a character outside
// the hexadecimal digits is a failure rather than a silent truncation,
// so that the two halves of the apparatus agree on what is readable.
std::vector<uint8_t> ParseHexString(std::string_view hex) {
  std::vector<uint8_t> bytes;
  if (hex.size() % 2 != 0) {
    ADD_FAILURE() << "hexadecimal of odd length " << hex.size();
    return bytes;
  }
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    if (std::isxdigit(static_cast<unsigned char>(hex[i])) == 0 ||
        std::isxdigit(static_cast<unsigned char>(hex[i + 1])) == 0) {
      ADD_FAILURE() << "not hexadecimal at offset " << i << ": " << hex;
      return bytes;
    }
    bytes.push_back(ParseHexByte(hex.substr(i, 2)));
  }
  return bytes;
}

std::vector<std::string> SplitOnTabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

// Splits into lines, keeping an empty one. The scripted harness reports
// one line per command that produces output and its caller pairs those
// lines with commands by position, so discarding an empty line would
// shift every line after it and compare the wrong pair.
std::vector<std::string> SplitLinesKeepingEmpty(const std::string& text) {
  std::vector<std::string> lines;
  if (text.empty()) {
    return lines;
  }
  size_t start = 0;
  while (true) {
    const size_t newline = text.find('\n', start);
    if (newline == std::string::npos) {
      lines.push_back(text.substr(start));
      return lines;
    }
    lines.push_back(text.substr(start, newline - start));
    start = newline + 1;
  }
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

// Runs a script against the reference harness. The type selects which
// of the reference's builders makes the sketch, and the commands are
// applied in order; the lines returned are what the script's
// checkpoints and estimates printed, plus a marker for any command the
// reference refused.
std::vector<std::string> RunScript(const std::string& type, int np, int sp,
                                   const std::vector<std::string>& commands) {
  std::string input_data;
  for (const std::string& command : commands) {
    input_data += command + "\n";
  }
  std::string output = RunJavaWithArguments(
      {"SCRIPT", type, std::to_string(np), std::to_string(sp)}, input_data);
  // Only the newline that terminates the final line is removed, so that
  // a final line which is empty is kept like any other.
  if (!output.empty() && output.back() == '\n') {
    output.pop_back();
  }
  return SplitLinesKeepingEmpty(output);
}

// Runs a script that names its own receivers with RECEIVER commands and
// separates its parts with MARK commands, so that one start of the
// reference can carry many independent scripts.
std::vector<std::string> RunScriptWithReceivers(const std::string& script) {
  std::string output = RunJavaWithArguments({"SCRIPT", "none"}, script);
  if (!output.empty() && output.back() == '\n') {
    output.pop_back();
  }
  return SplitLinesKeepingEmpty(output);
}

// Splits the harness's output at its MARK lines into named blocks, in
// the order the marks were printed.
std::vector<std::pair<std::string, std::vector<std::string>>> SplitAtMarks(
    const std::vector<std::string>& lines) {
  constexpr std::string_view kMark = "MARK ";
  std::vector<std::pair<std::string, std::vector<std::string>>> blocks;
  for (const std::string& line : lines) {
    if (line.starts_with(kMark)) {
      blocks.emplace_back(line.substr(kMark.size()),
                          std::vector<std::string>{});
      continue;
    }
    if (blocks.empty()) {
      ADD_FAILURE() << "output before the first mark: " << line;
      continue;
    }
    blocks.back().second.push_back(line);
  }
  return blocks;
}

// Every byte string the reference writes must be read back and pass the
// full walk of its contents.
void ExpectValidates(const std::string& hex, const std::string& context) {
  auto sketch = HyperLogLogPlusPlus::FromBytes(ParseHexString(hex));
  ASSERT_TRUE(sketch.has_value()) << context;
  auto valid = sketch->Validate();
  EXPECT_TRUE(valid.has_value())
      << context << (valid.has_value() ? "" : ": " + valid.error().message);
}

// Which estimator is used is decided by the linear counting estimate
// against the tabulated threshold, not by the number of items added,
// so no choice of population reaches a given branch. Comparing the
// estimate at every population from one upward crosses the branch
// wherever it lies. Measured against the reference by computing the
// linear counting estimate from the register array it emits, the
// branch changes at populations 9, 19, 41, 90, 220 and 396 for
// precisions 4 to 9, so each sweep below spans it with both branches
// either side.
TEST(ReferenceLibraryTest, EstimatesAgreeAtEveryPopulationAcrossTheBranch) {
  const std::vector<std::pair<int, int>> sweeps = {
      {4, 40}, {5, 80}, {6, 120}, {7, 200}, {8, 500}, {9, 900}};
  for (const auto& [np, upper] : sweeps) {
    auto [items, input_data] = MakeItems("s_", upper);
    const std::vector<std::string> java_estimates =
        SplitLines(RunJava("ESTIMATE_SWEEP", np, 0, input_data));
    ASSERT_EQ(java_estimates.size(), items.size()) << "normal precision " << np;

    auto hll_res = HyperLogLogPlusPlus::Create(np, 0);
    ASSERT_TRUE(hll_res.has_value());
    auto hll = std::move(hll_res.value());
    for (size_t i = 0; i < items.size(); ++i) {
      ASSERT_TRUE(hll.Add(items.at(i)).has_value());
      auto estimate = hll.Result();
      ASSERT_TRUE(estimate.has_value());
      EXPECT_EQ(std::to_string(estimate.value()), java_estimates.at(i))
          << "normal precision " << np << ", population " << (i + 1);
    }
  }
}

// Returns the bit pattern of a double, with negative zero normalised,
// so that two results can be compared exactly rather than within a
// tolerance. A tolerance would not detect a difference in the last
// place, which is what distinguishes dividing by a distance from
// multiplying by its reciprocal.
uint64_t NormalisedBits(double value) {
  return std::bit_cast<uint64_t>(value + 0.0);
}

// The bias correction must equal the reference's exactly, at every
// tabulated precision and at both the tabulated means and the points
// between them. The means bracket the interpolation, and the points
// between them drive it; the descending pairs the reference's own
// tables contain at precisions 5 and 6 are covered along with the
// rest.
TEST(ReferenceLibraryTest, BiasCorrectionsAreBitIdenticalToTheReference) {
  for (int32_t precision = zetasketch::internal::kMinimumPrecision;
       precision <= zetasketch::internal::kMaximumPrecision; ++precision) {
    const auto row = static_cast<size_t>(
        precision - zetasketch::internal::kMinimumPrecision);
    const std::span<const double> means =
        zetasketch::internal::kMeanData.at(row);

    std::vector<double> estimates;
    std::string input_data;
    for (size_t i = 0; i + 1 < means.size(); ++i) {
      const auto offset = static_cast<std::ptrdiff_t>(i);
      const double lower = *std::next(means.begin(), offset);
      const double upper = *std::next(means.begin(), offset + 1);
      for (const double estimate : {lower, std::midpoint(lower, upper)}) {
        estimates.push_back(estimate);
        input_data += std::format("{:.17g} {}\n", estimate, precision);
      }

      // Midway between two means six or seven apart. Where those two
      // are exactly equidistant from the midpoint, the tie falls at
      // the sixth and seventh neighbours, which is where a selection
      // that ordered ties differently would take a different set of
      // neighbours rather than the same set in a different order. Of
      // the 5,493 such samples the loop draws, 1,322 are corrections
      // that a reversed tie order would change, by between thirteen
      // thousand and a quadrillion units in the last place. The
      // midpoints of adjacent means, sampled above, catch the other
      // case: exactly one of them changes, and by a single unit.
      for (const size_t separation : {size_t{6}, size_t{7}}) {
        if (i + separation >= means.size()) {
          continue;
        }
        const double distant = *std::next(
            means.begin(), offset + static_cast<std::ptrdiff_t>(separation));
        const double tie = std::midpoint(lower, distant);
        estimates.push_back(tie);
        input_data += std::format("{:.17g} {}\n", tie, precision);
      }
    }

    // The loop above stops one short, so the last mean of the row is
    // added here; every tabulated mean is then sampled.
    estimates.push_back(means.back());
    input_data += std::format("{:.17g} {}\n", means.back(), precision);

    const std::vector<std::string> java_biases =
        SplitLines(RunJava("ESTIMATE_BIAS", precision, 0, input_data));
    ASSERT_EQ(java_biases.size(), estimates.size())
        << "precision " << precision;
    for (size_t i = 0; i < estimates.size(); ++i) {
      const double ours =
          zetasketch::hll::EstimateBias(estimates.at(i), precision);
      const double theirs = std::stod(java_biases.at(i));
      EXPECT_EQ(NormalisedBits(ours), NormalisedBits(theirs))
          << "precision " << precision << ", estimate " << estimates.at(i)
          << ", ours " << ours << ", reference " << theirs;
    }
  }
}

// Above the thresholds the estimate is governed by alpha, which the
// reference replaces with a constant at precisions 4, 5 and 6.
TEST(ReferenceLibraryTest, EstimatesAgreeWhereAlphaGoverns) {
  auto [items, input_data] = MakeItems("a_", 2000);
  for (const int np : {4, 5, 6, 7}) {
    const int64_t cpp_estimate = CppEstimate(np, 0, items);
    const std::string java_estimate = RunJava("ESTIMATE", np, 0, input_data);
    EXPECT_EQ(std::to_string(cpp_estimate), java_estimate)
        << "normal precision " << np;
  }
}

// Builds the parse-shape product the host-side comparison also pins:
// five normal precisions, at and around both limits, by five sparse
// precisions each, taken with nothing stored, with sparse data, with an
// empty data field and with an empty sparse data field, and at the two
// smallest normal precisions also with a register array alone and with
// a register array beside sparse data. A register array is sixteen
// megabytes at precision 24, which is why the last two are confined to
// the small precisions.
std::vector<std::pair<std::string, std::vector<uint8_t>>> BuildParseShapes() {
  enum class Content {
    kNothing,
    kSparse,
    kEmptyDense,
    kEmptySparse,
    kDense,
    kDenseAndSparse
  };
  constexpr std::array<Content, 6> kContents = {
      Content::kNothing,     Content::kSparse, Content::kEmptyDense,
      Content::kEmptySparse, Content::kDense,  Content::kDenseAndSparse};
  constexpr int32_t kLargestPrecisionWithADenseShape = 4;

  std::vector<std::pair<std::string, std::vector<uint8_t>>> shapes;
  for (const int32_t precision : {3, 4, 15, 24, 25}) {
    // None, one below the normal precision, the normal precision
    // itself, the maximum accepted, and one above it. At precision 25
    // the third and fourth coincide, so the widest encodable sparse
    // precision stands in for one of them.
    const std::array<int32_t, 5> sparse_precisions =
        precision == 25
            ? std::array<int32_t, 5>{0, 24, 25, 26, 30}
            : std::array<int32_t, 5>{0, precision - 1, precision, 25, 26};
    for (const int32_t sparse_precision : sparse_precisions) {
      for (const Content content : kContents) {
        const bool dense =
            content == Content::kDense || content == Content::kDenseAndSparse;
        if (dense && precision > kLargestPrecisionWithADenseShape) {
          continue;
        }
        zetasketch::hll::State state;
        state.encoding_version = 2;
        state.precision = precision;
        state.sparse_precision = sparse_precision;
        if (dense) {
          state.data = std::vector<uint8_t>(
              size_t{1} << static_cast<size_t>(precision), 0);
        }
        if (content == Content::kEmptyDense) {
          state.data = std::vector<uint8_t>();
        }
        if (content == Content::kSparse ||
            content == Content::kDenseAndSparse) {
          state.sparse_size = 1;
          state.sparse_data = std::vector<uint8_t>{0x05};
        }
        if (content == Content::kEmptySparse) {
          state.sparse_data = std::vector<uint8_t>();
        }

        auto bytes = state.ToByteArray();
        if (!bytes.has_value()) {
          continue;
        }
        shapes.emplace_back(
            std::format("normal precision {}, sparse precision {}, content {}",
                        precision, sparse_precision, static_cast<int>(content)),
            std::move(bytes.value()));
      }
    }
  }
  return shapes;
}

// Every shape the reference is willing to read must be read the same
// way here, and what it writes back out must be reproduced byte for
// byte. Both verdicts and bytes are taken from the reference as this
// test runs, so the table the host-side comparison carries cannot
// drift away from the library without this failing.
TEST(ReferenceLibraryTest, ParseShapesAndTheirBytesAgreeWithTheReference) {
  const auto shapes = BuildParseShapes();
  std::string input_data;
  for (const auto& shape : shapes) {
    input_data += PrintHex(shape.second) + "\n";
  }

  const std::vector<std::string> verdicts =
      SplitLines(RunJava("ROUNDTRIP", 4, 0, input_data));
  ASSERT_EQ(verdicts.size(), shapes.size());

  for (size_t i = 0; i < shapes.size(); ++i) {
    const std::string& verdict = verdicts.at(i);
    const bool reference_accepts = verdict.starts_with("ACCEPT ");
    auto sketch =
        zetasketch::HyperLogLogPlusPlus::FromBytes(shapes.at(i).second);
    ASSERT_EQ(sketch.has_value(), reference_accepts) << shapes.at(i).first;
    if (!reference_accepts) {
      continue;
    }
    auto written = sketch.value().Serialize();
    ASSERT_TRUE(written.has_value()) << shapes.at(i).first;
    EXPECT_EQ(PrintHex(written.value()), verdict.substr(std::strlen("ACCEPT ")))
        << shapes.at(i).first;
  }
}

// Reading a sketch and writing it straight back out never reaches an
// operation. Each shape the reference reads is therefore carried one
// step further here, through an addition, through a merge with a
// second copy of itself, and through an estimate, and the cardinality
// and the bytes are compared with the reference's at every step. The
// states an operation reaches from a crafted sketch are where a
// register array's contents, a sparse size larger than its precision
// admits, and an unrecorded value type first become observable.
TEST(ReferenceLibraryTest, OperationsOnParseShapesAgreeWithTheReference) {
  const auto shapes = BuildParseShapes();
  const std::array<std::string_view, 4> operations = {"RESULT", "ADD", "MERGE",
                                                      "ADD_AND_WRITE"};

  for (const std::string_view operation : operations) {
    std::string input_data;
    for (const auto& shape : shapes) {
      const std::string hex = PrintHex(shape.second);
      std::string argument;
      if (operation == "ADD" || operation == "ADD_AND_WRITE") {
        // Enough additions to carry a sparse sketch past the promotion
        // that a write performs, so the two orders can disagree.
        argument = operation == "ADD" ? "1:q" : "400:q";
      } else if (operation == "MERGE") {
        argument = hex;
      }
      input_data += std::format("{}\t{}\t{}\n", operation, hex, argument);
    }

    const std::vector<std::string> outcomes =
        SplitLines(RunJava("TRANSITION", 4, 0, input_data));
    ASSERT_EQ(outcomes.size(), shapes.size()) << operation;

    for (size_t i = 0; i < shapes.size(); ++i) {
      const std::string& outcome = outcomes.at(i);
      const std::string context =
          std::format("{} on {}", operation, shapes.at(i).first);

      auto sketch =
          zetasketch::HyperLogLogPlusPlus::FromBytes(shapes.at(i).second);
      const bool reference_accepts = outcome.starts_with("ACCEPT\t");
      if (!sketch.has_value()) {
        EXPECT_FALSE(reference_accepts) << context;
        continue;
      }

      bool ours_succeeds = true;
      if (operation == "ADD") {
        ours_succeeds = sketch.value().Add(std::string("q0")).has_value();
      } else if (operation == "ADD_AND_WRITE") {
        for (int i = 0; i < 400 && ours_succeeds; ++i) {
          ours_succeeds =
              sketch.value().Add("q" + std::to_string(i)).has_value();
        }
        if (ours_succeeds) {
          ours_succeeds = sketch.value().Serialize().has_value();
        }
      } else if (operation == "MERGE") {
        auto operand =
            zetasketch::HyperLogLogPlusPlus::FromBytes(shapes.at(i).second);
        ASSERT_TRUE(operand.has_value()) << context;
        ours_succeeds =
            sketch.value().Merge(std::move(operand.value())).has_value();
      }

      // The reference's harness reports the cardinality before the
      // bytes, and reporting one flushes a sparse sketch's buffer, so
      // the same order is followed here.
      std::optional<int64_t> our_result;
      if (ours_succeeds) {
        auto result = sketch.value().Result();
        if (result.has_value()) {
          our_result = result.value();
        }
      }
      ASSERT_EQ(our_result.has_value(), reference_accepts) << context;
      if (!our_result.has_value()) {
        continue;
      }

      const std::vector<std::string> fields = SplitOnTabs(outcome);
      ASSERT_GE(fields.size(), 3U) << context;
      EXPECT_EQ(std::to_string(*our_result), fields.at(1)) << context;

      auto written = sketch.value().Serialize();
      ASSERT_TRUE(written.has_value()) << context;
      EXPECT_EQ(PrintHex(written.value()), fields.at(2)) << context;
    }
  }
}

// The reference checks the aggregator's own fields before it looks at
// the sketch, and reads them with a parser that requires none of them.
// The product below is every combination of an aggregator type, an
// encoding version, a value type and a value count, each including the
// case where the field is absent: 1,260 shapes, every verdict and every
// byte taken from the reference as this test runs.
TEST(ReferenceLibraryTest, AggregatorFieldsAgreeWithTheReference) {
  struct Field {
    bool present;
    int32_t value;
  };
  const std::array<Field, 7> types = {Field{.present = false, .value = 0},
                                      Field{.present = true, .value = 0},
                                      Field{.present = true, .value = 1},
                                      Field{.present = true, .value = 100},
                                      Field{.present = true, .value = 112},
                                      Field{.present = true, .value = 113},
                                      Field{.present = true, .value = 200}};
  const std::array<Field, 5> versions = {
      Field{.present = false, .value = 0}, Field{.present = true, .value = 0},
      Field{.present = true, .value = 1}, Field{.present = true, .value = 2},
      Field{.present = true, .value = 3}};
  const std::array<Field, 12> value_types = {
      Field{.present = false, .value = 0},
      Field{.present = true, .value = 0},
      Field{.present = true, .value = 1},
      Field{.present = true, .value = 4},
      Field{.present = true, .value = 6},
      Field{.present = true, .value = 7},
      Field{.present = true, .value = 8},
      Field{.present = true, .value = 9},
      Field{.present = true, .value = 10},
      Field{.present = true, .value = 11},
      Field{.present = true, .value = 12},
      Field{.present = true, .value = 1000}};
  const std::array<Field, 3> counts = {Field{.present = false, .value = 0},
                                       Field{.present = true, .value = 0},
                                       Field{.present = true, .value = 5}};

  std::vector<std::pair<std::string, std::vector<uint8_t>>> shapes;
  for (const Field& type : types) {
    for (const Field& version : versions) {
      for (const Field& value_type : value_types) {
        for (const Field& count : counts) {
          std::vector<uint8_t> bytes;
          const auto put = [&bytes](uint8_t tag, int64_t value) {
            bytes.push_back(tag);
            auto remaining = static_cast<uint64_t>(value);
            while (true) {
              const auto byte = static_cast<uint8_t>(remaining & 0x7FU);
              remaining >>= 7U;
              bytes.push_back(
                  remaining != 0 ? static_cast<uint8_t>(byte | 0x80U) : byte);
              if (remaining == 0) {
                break;
              }
            }
          };
          if (type.present) put(0x08, type.value);
          if (count.present) put(0x10, count.value);
          if (version.present) put(0x18, version.value);
          if (value_type.present) put(0x20, value_type.value);
          bytes.push_back(0x82);
          bytes.push_back(0x07);
          bytes.push_back(0x02);
          bytes.push_back(0x18);
          bytes.push_back(0x04);
          shapes.emplace_back(
              std::format("type {}/{}, version {}/{}, value type {}/{}, "
                          "count {}/{}",
                          type.present, type.value, version.present,
                          version.value, value_type.present, value_type.value,
                          count.present, count.value),
              std::move(bytes));
        }
      }
    }
  }

  std::string input_data;
  for (const auto& shape : shapes) {
    input_data += PrintHex(shape.second) + "\n";
  }
  const std::vector<std::string> verdicts =
      SplitLines(RunJava("ROUNDTRIP", 4, 0, input_data));
  ASSERT_EQ(verdicts.size(), shapes.size());

  for (size_t i = 0; i < shapes.size(); ++i) {
    const std::string& verdict = verdicts.at(i);
    const bool reference_accepts = verdict.starts_with("ACCEPT ");
    auto sketch =
        zetasketch::HyperLogLogPlusPlus::FromBytes(shapes.at(i).second);
    ASSERT_EQ(sketch.has_value(), reference_accepts) << shapes.at(i).first;
    if (!reference_accepts) {
      continue;
    }
    auto written = sketch.value().Serialize();
    ASSERT_TRUE(written.has_value()) << shapes.at(i).first;
    EXPECT_EQ(PrintHex(written.value()), verdict.substr(std::strlen("ACCEPT ")))
        << shapes.at(i).first;
  }
}

// A merge lowers whichever operand is higher, and only a pair of
// encodings that is unordered in both precisions has no common encoding
// to lower into. Every ordered pair of the configurations below is
// merged, at three populations each, and the verdict, the cardinality
// and the bytes are compared with the reference's. Both operands are
// the reference's own output, so nothing here depends on this library
// having written them. The product covers equal configurations, a
// difference in the normal precision alone, a difference in the sparse
// precision alone, differences in both in the same direction, and the
// pairs that differ in opposite directions, which are the ones with no
// common encoding.
TEST(ReferenceLibraryTest, MergesAcrossPrecisionsAgreeWithTheReference) {
  const std::vector<std::pair<int, int>> configurations = {
      {4, 0},   {4, 9},   {10, 0},  {10, 15},
      {10, 20}, {10, 25}, {12, 15}, {15, 20}};
  const std::vector<int> populations = {0, 3, 200};

  struct Operand {
    std::string description;
    std::string hex;
  };
  std::vector<Operand> operands;
  for (const auto& configuration : configurations) {
    for (const int population : populations) {
      auto [items, input_data] = MakeItems(
          std::format("m{}_{}_", configuration.first, configuration.second),
          population);
      operands.push_back(
          {.description = std::format("precision {} sparse {} population {}",
                                      configuration.first, configuration.second,
                                      population),
           .hex = RunJava("CREATE", configuration.first, configuration.second,
                          input_data)});
      ASSERT_FALSE(operands.back().hex.empty()) << operands.back().description;
    }
  }

  std::string input_data;
  for (const Operand& target : operands) {
    for (const Operand& operand : operands) {
      input_data += std::format("MERGE\t{}\t{}\n", target.hex, operand.hex);
    }
  }
  const std::vector<std::string> outcomes =
      SplitLines(RunJava("TRANSITION", 4, 0, input_data));
  ASSERT_EQ(outcomes.size(), operands.size() * operands.size());

  size_t index = 0;
  for (const Operand& target : operands) {
    for (const Operand& operand : operands) {
      const std::string& outcome = outcomes.at(index++);
      const bool reference_accepts = outcome.starts_with("ACCEPT\t");
      const std::string context = std::format(
          "{} merged with {}", target.description, operand.description);

      auto into = zetasketch::HyperLogLogPlusPlus::FromBytes(
          ParseHexString(target.hex));
      ASSERT_TRUE(into.has_value()) << context;
      auto from = zetasketch::HyperLogLogPlusPlus::FromBytes(
          ParseHexString(operand.hex));
      ASSERT_TRUE(from.has_value()) << context;

      const bool merged =
          into.value().Merge(std::move(from.value())).has_value();
      ASSERT_EQ(merged, reference_accepts) << context;
      if (!reference_accepts) {
        continue;
      }

      auto result = into.value().Result();
      ASSERT_TRUE(result.has_value()) << context;
      auto written = into.value().Serialize();
      ASSERT_TRUE(written.has_value()) << context;

      const std::vector<std::string> fields = SplitOnTabs(outcome);
      ASSERT_GE(fields.size(), 3U) << context;
      EXPECT_EQ(std::to_string(result.value()), fields.at(1)) << context;
      EXPECT_EQ(PrintHex(written.value()), fields.at(2)) << context;
    }
  }
}

// The scripted harness has to reach the same sketch as the create mode
// for the same values, or nothing measured through it can be trusted.
// The create mode adds each value as text, so the text channel is what
// is compared against it; this library's own addition hashes the bytes
// it is given, so the byte channel is what is compared against that.
// The two channels coincide for the values used here and part company
// for values that are not text, which the next comparison pins.
TEST(ReferenceLibraryTest, TheScriptedHarnessAgreesWithTheCreateMode) {
  const std::vector<std::pair<int, int>> configurations = {
      {15, 20}, {10, 0}, {4, 9}, {4, 4}};
  // For the values these tests add, populations 11 and 12 at precision 4
  // fall where writing the sketch out promotes it, so the estimate that
  // follows the write differs from the one that would precede it.
  // Promotion is decided by the encoded size of the sparse stream, not
  // by the count, so the window belongs to these values.
  const std::vector<int> populations = {0, 1, 11, 12, 100, 1000};

  for (const auto& configuration : configurations) {
    const int normal_precision = configuration.first;
    const int sparse_precision = configuration.second;
    for (const int population : populations) {
      auto [items, input_data] = MakeItems("script_", population);
      const std::string context =
          std::format("precision {} sparse {} population {}", normal_precision,
                      sparse_precision, population);

      const std::string created =
          RunJava("CREATE", normal_precision, sparse_precision, input_data);

      std::vector<std::string> as_text;
      std::vector<std::string> as_bytes;
      as_text.reserve(items.size() + 2);
      as_bytes.reserve(items.size() + 2);
      for (const std::string& item : items) {
        as_text.push_back("ADD_STRING " + EncodeBase64(item));
        as_bytes.push_back("ADD_BYTES " + EncodeBase64(item));
      }
      for (std::vector<std::string>* script : {&as_text, &as_bytes}) {
        script->emplace_back("CHECKPOINT");
        script->emplace_back("RESULT");
      }

      const std::vector<std::string> scripted_text =
          RunScript("strings", normal_precision, sparse_precision, as_text);
      ASSERT_EQ(scripted_text.size(), 2U) << context;
      EXPECT_EQ(scripted_text.at(0), created) << context;

      const std::vector<std::string> scripted_bytes =
          RunScript("strings", normal_precision, sparse_precision, as_bytes);
      ASSERT_EQ(scripted_bytes.size(), 2U) << context;

      // The values here are text, so the two channels reach the same
      // sketch and must report the same estimate; the comparison that
      // follows pins where they part company.
      EXPECT_EQ(scripted_text.at(1), scripted_bytes.at(1)) << context;

      // The script writes the sketch out before it estimates, and
      // writing compacts, so the same two operations are performed here
      // in the same order. Estimating first would compare an estimate
      // taken from a different representation.
      auto ours = zetasketch::HyperLogLogPlusPlus::Create(normal_precision,
                                                          sparse_precision);
      ASSERT_TRUE(ours.has_value()) << context;
      for (const std::string& item : items) {
        ASSERT_TRUE(ours.value().Add(item).has_value()) << context;
      }
      auto written = ours.value().Serialize();
      ASSERT_TRUE(written.has_value()) << context;
      EXPECT_EQ(PrintHex(written.value()), scripted_bytes.at(0)) << context;
      auto estimate = ours.value().Result();
      ASSERT_TRUE(estimate.has_value()) << context;
      EXPECT_EQ(std::to_string(estimate.value()), scripted_bytes.at(1))
          << context;
    }
  }
}

// The reference hashes a string by encoding it as UTF-8, so a value
// that is not valid UTF-8 is replaced before it is hashed and two
// distinct values can collapse into one. It hashes a byte array
// exactly. This library's addition hashes the bytes it is given, so the
// byte channel is the one that corresponds to it, and a comparison
// routed through the text channel would agree only by accident of every
// value being text. Both facts are pinned here.
TEST(ReferenceLibraryTest, TheByteChannelHashesValuesThatAreNotText) {
  const std::vector<std::string> values = {
      std::string("\xff"),
      std::string("\xfe"),
      std::string("\x80\x41"),
      std::string("\xc3\x28"),
      std::string("\x00\xff\x41", 3),
      std::string("\xed\xa0\x80"),
  };
  constexpr int kNormalPrecision = 10;
  constexpr int kSparsePrecision = 15;

  std::vector<std::string> as_bytes;
  std::vector<std::string> as_text;
  for (const std::string& value : values) {
    as_bytes.push_back("ADD_BYTES " + EncodeBase64(value));
    as_text.push_back("ADD_STRING " + EncodeBase64(value));
  }
  as_bytes.emplace_back("CHECKPOINT");
  as_text.emplace_back("CHECKPOINT");

  const std::vector<std::string> scripted_bytes =
      RunScript("bytes", kNormalPrecision, kSparsePrecision, as_bytes);
  const std::vector<std::string> scripted_text =
      RunScript("strings", kNormalPrecision, kSparsePrecision, as_text);
  ASSERT_EQ(scripted_bytes.size(), 1U);
  ASSERT_EQ(scripted_text.size(), 1U);

  auto ours = zetasketch::HyperLogLogPlusPlus::Create(kNormalPrecision,
                                                      kSparsePrecision);
  ASSERT_TRUE(ours.has_value());
  for (const std::string& value : values) {
    ASSERT_TRUE(ours.value().Add(value).has_value());
  }
  auto written = ours.value().Serialize();
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(PrintHex(written.value()), scripted_bytes.at(0));

  // Every value here is invalid UTF-8, so the text channel replaces it
  // and reaches a different sketch. Were the two equal, the byte
  // channel would not be measuring what it claims to.
  EXPECT_NE(scripted_bytes.at(0), scripted_text.at(0));
}

// Writing a sketch out compacts it, and compaction can promote a sparse
// sketch to a dense one, so an estimate taken after a write need not
// equal one taken before it. For the values added here, the populations
// below fall where the two differ at precision 4, which is what makes
// this an observation rather than an assertion about the code. Were the
// values to change, the window would move and the first expectation
// below would fail rather than pass by accident.
TEST(ReferenceLibraryTest, WritingBeforeEstimatingChangesTheEstimate) {
  constexpr int kNormalPrecision = 4;
  constexpr int kSparsePrecision = 9;

  for (const int population : {11, 12}) {
    auto [items, unused_input] = MakeItems("script_", population);
    std::vector<std::string> additions;
    additions.reserve(items.size());
    for (const std::string& item : items) {
      additions.push_back("ADD_BYTES " + EncodeBase64(item));
    }

    std::vector<std::string> write_first = additions;
    write_first.emplace_back("CHECKPOINT");
    write_first.emplace_back("RESULT");
    std::vector<std::string> estimate_first = additions;
    estimate_first.emplace_back("RESULT");
    estimate_first.emplace_back("CHECKPOINT");

    const std::vector<std::string> after =
        RunScript("strings", kNormalPrecision, kSparsePrecision, write_first);
    const std::vector<std::string> before = RunScript(
        "strings", kNormalPrecision, kSparsePrecision, estimate_first);
    ASSERT_EQ(after.size(), 2U) << population;
    ASSERT_EQ(before.size(), 2U) << population;
    EXPECT_NE(after.at(1), before.at(0)) << population;

    auto written_then_estimated = zetasketch::HyperLogLogPlusPlus::Create(
        kNormalPrecision, kSparsePrecision);
    ASSERT_TRUE(written_then_estimated.has_value()) << population;
    auto estimated_then_written = zetasketch::HyperLogLogPlusPlus::Create(
        kNormalPrecision, kSparsePrecision);
    ASSERT_TRUE(estimated_then_written.has_value()) << population;
    for (const std::string& item : items) {
      ASSERT_TRUE(written_then_estimated.value().Add(item).has_value())
          << population;
      ASSERT_TRUE(estimated_then_written.value().Add(item).has_value())
          << population;
    }

    ASSERT_TRUE(written_then_estimated.value().Serialize().has_value())
        << population;
    auto after_ours = written_then_estimated.value().Result();
    ASSERT_TRUE(after_ours.has_value()) << population;
    EXPECT_EQ(std::to_string(after_ours.value()), after.at(1)) << population;

    auto before_ours = estimated_then_written.value().Result();
    ASSERT_TRUE(before_ours.has_value()) << population;
    EXPECT_EQ(std::to_string(before_ours.value()), before.at(0)) << population;
  }
}

// A merge in the middle of a script is the operation this harness was
// written to reach: it leaves a state behind that the next command
// observes, and a merge that writes the right bytes can still leave the
// wrong state. The operands below are the reference's own, one of the
// same configuration and one of a lower precision, which the merge must
// lower into.
TEST(ReferenceLibraryTest, MergingInsideAScriptAgreesWithTheReference) {
  constexpr int kNormalPrecision = 10;
  constexpr int kSparsePrecision = 15;
  const std::vector<std::pair<int, int>> operand_configurations = {{10, 15},
                                                                   {4, 9}};

  for (const auto& operand_configuration : operand_configurations) {
    auto [operand_items, operand_input] = MakeItems("operand_", 200);
    const std::string operand =
        RunJava("CREATE", operand_configuration.first,
                operand_configuration.second, operand_input);
    ASSERT_FALSE(operand.empty());
    const std::string context =
        std::format("operand precision {} sparse {}",
                    operand_configuration.first, operand_configuration.second);

    auto [items, unused_input] = MakeItems("receiver_", 300);
    std::vector<std::string> script;
    for (const std::string& item : items) {
      script.push_back("ADD_BYTES " + EncodeBase64(item));
    }
    script.emplace_back("CHECKPOINT");
    script.push_back("MERGE " + operand);
    script.emplace_back("CHECKPOINT");
    script.emplace_back("RESULT");

    const std::vector<std::string> scripted =
        RunScript("strings", kNormalPrecision, kSparsePrecision, script);
    ASSERT_EQ(scripted.size(), 3U) << context;

    auto ours = zetasketch::HyperLogLogPlusPlus::Create(kNormalPrecision,
                                                        kSparsePrecision);
    ASSERT_TRUE(ours.has_value()) << context;
    for (const std::string& item : items) {
      ASSERT_TRUE(ours.value().Add(item).has_value()) << context;
    }
    auto before_merge = ours.value().Serialize();
    ASSERT_TRUE(before_merge.has_value()) << context;
    EXPECT_EQ(PrintHex(before_merge.value()), scripted.at(0)) << context;

    auto operand_sketch =
        zetasketch::HyperLogLogPlusPlus::FromBytes(ParseHexString(operand));
    ASSERT_TRUE(operand_sketch.has_value()) << context;
    ASSERT_TRUE(
        ours.value().Merge(std::move(operand_sketch.value())).has_value())
        << context;

    auto after_merge = ours.value().Serialize();
    ASSERT_TRUE(after_merge.has_value()) << context;
    EXPECT_EQ(PrintHex(after_merge.value()), scripted.at(1)) << context;
    auto estimate = ours.value().Result();
    ASSERT_TRUE(estimate.has_value()) << context;
    EXPECT_EQ(std::to_string(estimate.value()), scripted.at(2)) << context;
  }
}

// A script's checkpoints are the sketch as it stood at each point, in
// order, which is what lets a comparison see the state one operation
// leaves behind for the next. Each checkpoint here must equal the
// create mode's output for the values added up to it.
TEST(ReferenceLibraryTest, TheScriptedHarnessReturnsCheckpointsInOrder) {
  constexpr int kNormalPrecision = 10;
  constexpr int kSparsePrecision = 15;
  const std::vector<int> stops = {0, 1, 17, 300, 900};

  auto [items, unused_input] = MakeItems("stepped_", stops.back());
  std::vector<std::string> commands;
  size_t added = 0;
  for (const int stop : stops) {
    for (; std::cmp_less(added, stop); ++added) {
      commands.push_back("ADD_STRING " + EncodeBase64(items.at(added)));
    }
    commands.emplace_back("CHECKPOINT");
  }

  const std::vector<std::string> checkpoints =
      RunScript("strings", kNormalPrecision, kSparsePrecision, commands);
  ASSERT_EQ(checkpoints.size(), stops.size());

  for (size_t i = 0; i < stops.size(); ++i) {
    const std::vector<std::string> prefix(
        items.begin(),
        std::next(items.begin(), static_cast<std::ptrdiff_t>(stops.at(i))));
    std::string prefix_input;
    for (const std::string& item : prefix) {
      prefix_input += item + "\n";
    }
    EXPECT_EQ(checkpoints.at(i), RunJava("CREATE", kNormalPrecision,
                                         kSparsePrecision, prefix_input))
        << "after " << stops.at(i) << " additions";
  }
}

// A command the reference refuses reports itself and the script carries
// on, so that a refusal is as observable as a result and one refusal
// does not discard the rest of the script. An argument the harness
// cannot read carries a different marker, because a script that cannot
// be parsed is a fault in the script rather than a fact about the
// reference, and a comparison that confused the two would read a broken
// script as a pinned refusal. The set of additions an aggregator admits
// narrows with its first addition, to strings or to byte arrays, and
// that narrowing is invisible in the bytes the aggregator writes; the
// refusal message is the only place it can be observed, so both
// narrowings and the set before any narrowing are pinned here. The four
// messages naming an aggregator's type set are the reference library's
// own; the two reporting an unreadable number or base64 belong to the
// language's own library, and the rest are the harness reporting a
// script it cannot carry out.
TEST(ReferenceLibraryTest, TheScriptedHarnessSeparatesRefusalsFromBadInput) {
  struct Case {
    const char* description;
    const char* type;
    std::vector<std::string> commands;
    std::vector<std::string> expected;
  };
  const std::string letter = EncodeBase64("a");
  const std::string operand = RunJava("CREATE", 4, 0, "x\ny\n");
  // One expectation below is the length of this operand plus a nibble,
  // so an operand that failed to arrive would make that expectation
  // meaningless rather than failing.
  ASSERT_FALSE(operand.empty());
  ASSERT_EQ(operand.size() % 2, 0U);
  const std::vector<Case> cases = {
      {.description = "a text sketch narrowed to strings refuses a long",
       .type = "strings",
       .commands = {"ADD_STRING " + letter, "ADD_LONG 7",
                    "ADD_STRING " + letter, "RESULT"},
       .expected = {"ERROR unable to add type LONG to aggregator of type "
                    "[STRING]",
                    "1"}},
      {.description = "a text sketch narrowed to byte arrays refuses a long",
       .type = "strings",
       .commands = {"ADD_BYTES " + letter, "ADD_LONG 7", "RESULT"},
       .expected = {"ERROR unable to add type LONG to aggregator of type "
                    "[BYTES]",
                    "1"}},
      {.description = "a text sketch before any addition admits both",
       .type = "bytes",
       .commands = {"ADD_LONG 7", "RESULT"},
       .expected = {"ERROR unable to add type LONG to aggregator of type "
                    "[STRING, BYTES]",
                    "0"}},
      {.description = "a longs sketch refuses a string",
       .type = "longs",
       .commands = {"ADD_LONG 7", "ADD_STRING " + letter, "RESULT"},
       .expected = {"ERROR unable to add type STRING to aggregator of type "
                    "[LONG]",
                    "1"}},
      {.description = "arguments the harness cannot read",
       .type = "strings",
       .commands = {"ADD_STRING !!!!", "ADD_LONG notanumber", "MERGE",
                    "MERGE " + operand + "a", "MERGE -1", "MERGE 0",
                    "ADD_BYTES", "ADD_STRING " + letter, "RESULT"},
       .expected = {"BADINPUT Illegal base64 character 21",
                    "BADINPUT For input string: \"notanumber\"",
                    "BADINPUT missing argument for MERGE",
                    std::format("BADINPUT hexadecimal of odd length {}",
                                operand.size() + 1),
                    "BADINPUT not hexadecimal at offset 0: -1",
                    "BADINPUT hexadecimal of odd length 1",
                    "BADINPUT missing argument for ADD_BYTES", "1"}},
      {.description = "the empty value, which an absent argument is not",
       .type = "strings",
       .commands = {"ADD_STRING ", "RESULT", "ADD_STRING", "RESULT"},
       .expected = {"1", "BADINPUT missing argument for ADD_STRING", "1"}},
      {.description = "an unknown command",
       .type = "strings",
       .commands = {"NOT_A_COMMAND", "ADD_STRING " + letter, "RESULT"},
       .expected = {"BADINPUT unknown command NOT_A_COMMAND", "1"}},
      {.description = "an unknown type ends the script with its marker",
       .type = "Longs",
       .commands = {"ADD_LONG 7", "CHECKPOINT", "RESULT"},
       .expected = {"BADINPUT unknown type Longs"}},
  };

  for (const Case& test_case : cases) {
    EXPECT_EQ(RunScript(test_case.type, 4, 0, test_case.commands),
              test_case.expected)
        << test_case.description;
  }
}

// One sketch carried through every kind of operation in turn, with the
// bytes and the estimate compared against the reference at each point.
// The receiver is promoted by its first write and stays dense, so the
// additions and merges that follow act on a dense sketch; one operand
// is of the receiver's own configuration and one is of a lower
// precision. The operand of the sketch's own configuration is merged
// twice and the lower-precision one once, and the order is chosen so
// that an addition follows a write, a merge, and an estimate, and a
// merge follows a merge.
TEST(ReferenceLibraryTest, ALongSequenceOfOperationsAgreesWithTheReference) {
  constexpr int kNormalPrecision = 4;
  constexpr int kSparsePrecision = 9;
  auto [same_items, same_input] = MakeItems("same_", 30);
  auto [lower_items, lower_input] = MakeItems("lower_", 30);
  const std::string same = RunJava("CREATE", 4, 9, same_input);
  const std::string lower = RunJava("CREATE", 4, 4, lower_input);
  ASSERT_FALSE(same.empty());
  ASSERT_FALSE(lower.empty());

  auto [items, unused_input] = MakeItems("sequence_", 21);
  struct Step {
    const char* command;
    int add_count;
    const std::string* operand;
  };
  // Each add_count is how many further values the step adds.
  const std::vector<Step> steps = {
      {"ADD", 11, nullptr},   {"CHECKPOINT", 0, nullptr},
      {"ADD", 5, nullptr},    {"CHECKPOINT", 0, nullptr},
      {"MERGE", 0, &same},    {"CHECKPOINT", 0, nullptr},
      {"ADD", 3, nullptr},    {"RESULT", 0, nullptr},
      {"ADD", 2, nullptr},    {"MERGE", 0, &lower},
      {"MERGE", 0, &same},    {"CHECKPOINT", 0, nullptr},
      {"RESULT", 0, nullptr}, {"CHECKPOINT", 0, nullptr},
  };

  std::vector<std::string> script;
  size_t added = 0;
  for (const Step& step : steps) {
    if (std::string_view(step.command) == "ADD") {
      for (int i = 0; i < step.add_count; ++i) {
        script.push_back("ADD_BYTES " + EncodeBase64(items.at(added++)));
      }
    } else if (std::string_view(step.command) == "MERGE") {
      script.push_back("MERGE " + *step.operand);
    } else {
      script.emplace_back(step.command);
    }
  }
  const std::vector<std::string> scripted =
      RunScript("strings", kNormalPrecision, kSparsePrecision, script);

  auto ours = zetasketch::HyperLogLogPlusPlus::Create(kNormalPrecision,
                                                      kSparsePrecision);
  ASSERT_TRUE(ours.has_value());
  std::vector<std::string> mirrored;
  added = 0;
  for (const Step& step : steps) {
    const std::string_view command(step.command);
    if (command == "ADD") {
      for (int i = 0; i < step.add_count; ++i) {
        ASSERT_TRUE(ours.value().Add(items.at(added++)).has_value());
      }
    } else if (command == "MERGE") {
      auto operand = zetasketch::HyperLogLogPlusPlus::FromBytes(
          ParseHexString(*step.operand));
      ASSERT_TRUE(operand.has_value());
      ASSERT_TRUE(ours.value().Merge(std::move(operand.value())).has_value());
    } else if (command == "CHECKPOINT") {
      auto written = ours.value().Serialize();
      ASSERT_TRUE(written.has_value());
      mirrored.push_back(PrintHex(written.value()));
    } else {
      auto estimate = ours.value().Result();
      ASSERT_TRUE(estimate.has_value());
      mirrored.push_back(std::to_string(estimate.value()));
    }
  }
  EXPECT_EQ(mirrored, scripted);
}

// The integer path, compared with the reference at every precision
// configuration the plan names. A sketch built from integers is the
// same sketch either side only if the value is hashed identically, and
// the reference hashes an integer by fingerprinting its eight bytes
// with the least significant first, so a difference of byte order or
// of width would show here as different bytes at every population but
// the empty one.
class IntegerDifferentialTest
    : public ::testing::TestWithParam<std::pair<int, int>> {
 protected:
  // Builds a sketch of the given range of integers through this
  // library, and the command block that asks the reference for the
  // same sketch.
  static std::string CppCreateLongs(int np, int sp, int64_t first,
                                    int64_t last) {
    auto sketch = zetasketch::HyperLogLogPlusPlus::Create(
        np, sp, zetasketch::hll::ValueType::kUnsignedInt64);
    EXPECT_TRUE(sketch.has_value());
    if (!sketch.has_value()) return "";
    for (int64_t value = first; value < last; ++value) {
      EXPECT_TRUE(sketch.value().Add(value).has_value());
    }
    auto bytes = sketch.value().Serialize();
    EXPECT_TRUE(bytes.has_value());
    return bytes.has_value() ? PrintHex(bytes.value()) : "";
  }

  static std::string ReferenceBlock(int np, int sp, int64_t first,
                                    int64_t last) {
    std::string block = std::format("SKETCH {} {} longs\n", np, sp);
    for (int64_t value = first; value < last; ++value) {
      block += std::format("LONG {}\n", value);
    }
    return block;
  }

  // Values chosen to exercise the whole of the eight bytes an integer is
  // hashed as, which a run of small counting numbers never does: both
  // extremes of the type, either side of zero, the boundaries of the
  // narrower widths, and pairs sharing all but one byte of their
  // encoding.
  static std::span<const int64_t> BoundaryValues() {
    static constexpr std::array<int64_t, 20> kValues = {
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::min() + 1,
        -4294967296,
        -2147483648,
        -65536,
        -256,
        -2,
        -1,
        0,
        1,
        2,
        255,
        256,
        65535,
        2147483647,
        4294967296,
        72057594037927936,
        72057594037927937,
        std::numeric_limits<int64_t>::max() - 1,
        std::numeric_limits<int64_t>::max()};
    return kValues;
  }

  static std::string BoundaryBlock(int np, int sp) {
    std::string block = std::format("SKETCH {} {} longs\n", np, sp);
    for (const int64_t value : BoundaryValues()) {
      block += std::format("LONG {}\n", value);
    }
    return block;
  }

  static std::string CppCreateBoundary(int np, int sp) {
    auto sketch = zetasketch::HyperLogLogPlusPlus::Create(
        np, sp, zetasketch::hll::ValueType::kUnsignedInt64);
    EXPECT_TRUE(sketch.has_value());
    if (!sketch.has_value()) return "";
    for (const int64_t value : BoundaryValues()) {
      EXPECT_TRUE(sketch.value().Add(value).has_value());
    }
    auto bytes = sketch.value().Serialize();
    EXPECT_TRUE(bytes.has_value());
    return bytes.has_value() ? PrintHex(bytes.value()) : "";
  }
};

TEST_P(IntegerDifferentialTest, Create) {
  const int np = GetParam().first;
  const int sp = GetParam().second;

  const std::vector<int64_t> populations = {0, 1, 10, 100, 1000, 5000};

  std::string batch;
  for (const int64_t population : populations) {
    batch += ReferenceBlock(np, sp, 0, population);
  }
  batch += BoundaryBlock(np, sp);

  const std::vector<std::string> reference =
      SplitLines(RunJava("CREATE_BATCH", np, sp, batch));
  ASSERT_EQ(reference.size(), populations.size() + 1)
      << "NP=" << np << " SP=" << sp;

  for (size_t i = 0; i < populations.size(); ++i) {
    EXPECT_EQ(CppCreateLongs(np, sp, 0, populations.at(i)), reference.at(i))
        << "NP=" << np << " SP=" << sp << " integers=" << populations.at(i);
    ExpectValidates(reference.at(i), std::format("NP={} SP={} integers={}", np,
                                                 sp, populations.at(i)));
  }
  EXPECT_EQ(CppCreateBoundary(np, sp), reference.back())
      << "NP=" << np << " SP=" << sp << " boundary values";
  ExpectValidates(reference.back(),
                  std::format("NP={} SP={} boundary values", np, sp));
}

TEST_P(IntegerDifferentialTest, Merge) {
  const int np = GetParam().first;
  const int sp = GetParam().second;

  // The shapes the string comparison uses: enough operands to stay
  // sparse, enough to promote, and a mixture.
  const std::vector<std::pair<int, int64_t>> merge_configs = {
      {3, 100}, {3, 2000}, {10, 200}};

  std::string create_batch;
  std::vector<std::vector<std::string>> cpp_operands(merge_configs.size());
  for (size_t config = 0; config < merge_configs.size(); ++config) {
    const int operands = merge_configs.at(config).first;
    const int64_t each = merge_configs.at(config).second;
    for (int i = 0; i < operands; ++i) {
      // Disjoint ranges, so that a merge has something to combine.
      const int64_t first = static_cast<int64_t>(i) * each;
      create_batch += ReferenceBlock(np, sp, first, first + each);
      cpp_operands.at(config).push_back(
          CppCreateLongs(np, sp, first, first + each));
    }
  }

  const std::vector<std::string> reference_operands =
      SplitLines(RunJava("CREATE_BATCH", np, sp, create_batch));
  size_t expected = 0;
  for (const auto& merge_config : merge_configs) {
    expected += static_cast<size_t>(merge_config.first);
  }
  ASSERT_EQ(reference_operands.size(), expected) << "NP=" << np << " SP=" << sp;

  std::string merge_batch;
  size_t taken = 0;
  for (const auto& merge_config : merge_configs) {
    merge_batch += "MERGE\n";
    for (int i = 0; i < merge_config.first; ++i) {
      merge_batch += reference_operands.at(taken++) + "\n";
    }
  }
  const std::vector<std::string> reference_merged =
      SplitLines(RunJava("MERGE_BATCH", np, sp, merge_batch));
  ASSERT_EQ(reference_merged.size(), merge_configs.size())
      << "NP=" << np << " SP=" << sp;

  for (size_t config = 0; config < merge_configs.size(); ++config) {
    EXPECT_EQ(CppMerge(np, sp, cpp_operands.at(config),
                       zetasketch::hll::ValueType::kUnsignedInt64),
              reference_merged.at(config))
        << "NP=" << np << " SP=" << sp
        << " operands=" << merge_configs.at(config).first;
    ExpectValidates(reference_merged.at(config),
                    std::format("NP={} SP={} merged operands={}", np, sp,
                                merge_configs.at(config).first));
  }
}

INSTANTIATE_TEST_SUITE_P(
    IntegerConfigs, IntegerDifferentialTest,
    ::testing::Values(std::make_pair(15, 20), std::make_pair(10, 15),
                      std::make_pair(15, 15), std::make_pair(10, 25),
                      std::make_pair(24, 25), std::make_pair(15, 0),
                      std::make_pair(10, 0)));

class DifferentialFuzzerTest
    : public ::testing::TestWithParam<std::pair<int, int>> {
 protected:
  static std::pair<std::vector<std::string>, std::string> GenerateTestItems(
      const std::string& prefix, int count) {
    return MakeItems(prefix, count);
  }
};

TEST_P(DifferentialFuzzerTest, Create) {
  const int np = GetParam().first;
  const int sp = GetParam().second;

  const std::vector<int> counts = {10, 100, 1000, 5000};

  // Every population is built in one invocation of the reference.
  // Starting a virtual machine costs far more than building a sketch,
  // so asking for all four at once is the difference between four
  // starts and one, at every configuration.
  std::string batch;
  std::vector<std::vector<std::string>> populations;
  populations.reserve(counts.size());
  for (const int num : counts) {
    auto [items, unused_input] = GenerateTestItems("item_", num);
    batch += std::format("SKETCH {} {}\n", np, sp);
    for (const std::string& item : items) {
      batch += "ITEM " + EncodeBase64(item) + "\n";
    }
    populations.push_back(std::move(items));
  }

  const std::vector<std::string> reference =
      SplitLines(RunJava("CREATE_BATCH", np, sp, batch));
  ASSERT_EQ(reference.size(), counts.size()) << "NP=" << np << " SP=" << sp;

  for (size_t i = 0; i < counts.size(); ++i) {
    EXPECT_EQ(CppCreate(np, sp, populations.at(i)), reference.at(i))
        << "Mismatch at NP=" << np << " SP=" << sp
        << " Elements=" << counts.at(i);
    ExpectValidates(reference.at(i), std::format("NP={} SP={} Elements={}", np,
                                                 sp, counts.at(i)));
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

  // Both halves are batched: every operand of every configuration is
  // built in one invocation, and every merge is performed in a second.
  // Done one at a time this was nineteen starts of a virtual machine
  // for each configuration, which was the largest single cost in this
  // suite.
  std::string create_batch;
  std::vector<std::vector<std::string>> cpp_operands(merge_configs.size());
  for (size_t config = 0; config < merge_configs.size(); ++config) {
    const int num_sketches = merge_configs.at(config).first;
    const int items_per_sketch = merge_configs.at(config).second;
    for (int i = 0; i < num_sketches; ++i) {
      auto [items, unused_input] = GenerateTestItems(
          "test_" + std::to_string(i) + "_", items_per_sketch);
      create_batch += std::format("SKETCH {} {}\n", np, sp);
      for (const std::string& item : items) {
        create_batch += "ITEM " + EncodeBase64(item) + "\n";
      }
      cpp_operands.at(config).push_back(CppCreate(np, sp, items));
    }
  }

  const std::vector<std::string> reference_operands =
      SplitLines(RunJava("CREATE_BATCH", np, sp, create_batch));
  size_t expected_operands = 0;
  for (const auto& merge_config : merge_configs) {
    expected_operands += static_cast<size_t>(merge_config.first);
  }
  ASSERT_EQ(reference_operands.size(), expected_operands)
      << "NP=" << np << " SP=" << sp;

  std::string merge_batch;
  size_t taken = 0;
  for (const auto& merge_config : merge_configs) {
    merge_batch += "MERGE\n";
    for (int i = 0; i < merge_config.first; ++i) {
      merge_batch += reference_operands.at(taken++) + "\n";
    }
  }

  const std::vector<std::string> reference_merged =
      SplitLines(RunJava("MERGE_BATCH", np, sp, merge_batch));
  ASSERT_EQ(reference_merged.size(), merge_configs.size())
      << "NP=" << np << " SP=" << sp;

  for (size_t config = 0; config < merge_configs.size(); ++config) {
    EXPECT_EQ(CppMerge(np, sp, cpp_operands.at(config)),
              reference_merged.at(config))
        << "Mismatch at Merge NP=" << np << " SP=" << sp
        << " sketches=" << merge_configs.at(config).first;
    ExpectValidates(reference_merged.at(config),
                    std::format("NP={} SP={} merged sketches={}", np, sp,
                                merge_configs.at(config).first));
  }
}

INSTANTIATE_TEST_SUITE_P(
    Configs, DifferentialFuzzerTest,
    ::testing::Values(
        std::make_pair(15, 20), std::make_pair(10, 15), std::make_pair(15, 0),
        std::make_pair(10, 0), std::make_pair(4, 0), std::make_pair(4, 9),
        std::make_pair(9, 14), std::make_pair(15, 25), std::make_pair(24, 25),
        std::make_pair(4, 4), std::make_pair(5, 0), std::make_pair(5, 10),
        std::make_pair(7, 12), std::make_pair(8, 0), std::make_pair(6, 0),
        std::make_pair(7, 0), std::make_pair(8, 13), std::make_pair(9, 0)));

// The additions, if any, that narrow a sketch's admitted set before it
// is used. The reference also narrows on a byte-array addition and on
// a 32-bit integer addition, which this library has no way to make.
enum class Prelude { kNone, kString, kLong };

// One side of a merge: its precisions, and how many values a prelude
// adds to it, which decides whether it is still sparse when merged.
struct Side {
  int np;
  int sp;
  int values;
};

// A state a sketch's admitted set can be in, paired with the harness
// words that put the reference in the same state. The set is derived
// from the recorded value type when a sketch is built or read, so a
// typed sketch reaches the same state from Create and from FromBytes,
// and both routes are here.
struct AdmittedState {
  std::string name;
  std::string reference_spec;
  Prelude prelude;
  std::string hex;  // Read with FromBytes when set; built otherwise.
  zetasketch::hll::ValueType value_type;
  Side side;
};

// The bytes of an empty sketch at the given precisions and value type.
// They are this library's, which the parse-shape and aggregator-field
// comparisons establish are the reference's own.
std::string EmptySketchHex(const Side& side,
                           zetasketch::hll::ValueType value_type) {
  auto sketch = HyperLogLogPlusPlus::Create(side.np, side.sp, value_type);
  EXPECT_TRUE(sketch.has_value());
  if (!sketch.has_value()) return "";
  auto bytes = sketch->Serialize();
  EXPECT_TRUE(bytes.has_value());
  return bytes.has_value() ? PrintHex(*bytes) : "";
}

std::vector<AdmittedState> AdmittedStates(const Side& side) {
  using zetasketch::hll::ValueType;
  const auto built = [&side](std::string_view word) {
    return std::format("{} {} {}", word, side.np, side.sp);
  };
  const std::string untyped = EmptySketchHex(side, ValueType::kUnknown);
  const std::string text = EmptySketchHex(side, ValueType::kBytesOrUtf8String);
  const std::string longs = EmptySketchHex(side, ValueType::kUnsignedInt64);
  const std::string integers = EmptySketchHex(side, ValueType::kUnsignedInt32);
  return {
      {.name = "untyped",
       .reference_spec = "proto " + untyped,
       .prelude = Prelude::kNone,
       .hex = untyped,
       .value_type = ValueType::kUnknown,
       .side = side},
      {.name = "text",
       .reference_spec = built("strings"),
       .prelude = Prelude::kNone,
       .hex = "",
       .value_type = ValueType::kBytesOrUtf8String,
       .side = side},
      {.name = "longs",
       .reference_spec = built("longs"),
       .prelude = Prelude::kNone,
       .hex = "",
       .value_type = ValueType::kUnsignedInt64,
       .side = side},
      {.name = "integers",
       .reference_spec = built("integers"),
       .prelude = Prelude::kNone,
       .hex = "",
       .value_type = ValueType::kUnsignedInt32,
       .side = side},
      {.name = "text-from-bytes",
       .reference_spec = "proto " + text,
       .prelude = Prelude::kNone,
       .hex = text,
       .value_type = ValueType::kBytesOrUtf8String,
       .side = side},
      {.name = "longs-from-bytes",
       .reference_spec = "proto " + longs,
       .prelude = Prelude::kNone,
       .hex = longs,
       .value_type = ValueType::kUnsignedInt64,
       .side = side},
      {.name = "integers-from-bytes",
       .reference_spec = "proto " + integers,
       .prelude = Prelude::kNone,
       .hex = integers,
       .value_type = ValueType::kUnsignedInt32,
       .side = side},
      {.name = "untyped-then-strings",
       .reference_spec = "proto " + untyped,
       .prelude = Prelude::kString,
       .hex = untyped,
       .value_type = ValueType::kUnknown,
       .side = side},
      {.name = "untyped-then-longs",
       .reference_spec = "proto " + untyped,
       .prelude = Prelude::kLong,
       .hex = untyped,
       .value_type = ValueType::kUnknown,
       .side = side},
      {.name = "text-then-strings",
       .reference_spec = built("strings"),
       .prelude = Prelude::kString,
       .hex = "",
       .value_type = ValueType::kBytesOrUtf8String,
       .side = side},
      {.name = "longs-then-longs",
       .reference_spec = built("longs"),
       .prelude = Prelude::kLong,
       .hex = "",
       .value_type = ValueType::kUnsignedInt64,
       .side = side},
  };
}

// The harness commands for a state's prelude, one per value; those on
// the operand carry the OPERAND_ prefix. The strings added are
// "value-0", "value-1" and so on, the longs 0, 1 and so on.
std::string PreludeCommands(const AdmittedState& state,
                            std::string_view prefix) {
  std::string commands;
  for (int value = 0; value < state.side.values; ++value) {
    switch (state.prelude) {
      case Prelude::kString:
        commands += std::format("{}ADD_STRING {}\n", prefix,
                                EncodeBase64(std::format("value-{}", value)));
        break;
      case Prelude::kLong:
        commands += std::format("{}ADD_LONG {}\n", prefix, value);
        break;
      case Prelude::kNone:
        return "";
    }
  }
  return commands;
}

std::expected<HyperLogLogPlusPlus, zetasketch::utils::Error> BuildAdmittedState(
    const AdmittedState& state) {
  auto built = state.hex.empty()
                   ? HyperLogLogPlusPlus::Create(state.side.np, state.side.sp,
                                                 state.value_type)
                   : HyperLogLogPlusPlus::FromBytes(ParseHexString(state.hex));
  if (!built.has_value()) return built;
  for (int value = 0; value < state.side.values; ++value) {
    std::expected<void, zetasketch::utils::Error> added;
    switch (state.prelude) {
      case Prelude::kString:
        added = built->Add(std::format("value-{}", value));
        break;
      case Prelude::kLong:
        added = built->Add(int64_t{value});
        break;
      case Prelude::kNone:
        return built;
    }
    if (!added.has_value()) return std::unexpected(added.error());
  }
  return built;
}

// One block of the matrix script: the receiver and operand put into
// their states, merged, written and estimated, then given a long and a
// string in the order asked for, written after each. A refusal prints
// under the reference's marker and the script continues, so the block
// has the same shape whichever way each step goes.
// The values added after the merge, chosen not to be among any prelude's:
// the long 1000000 and the string "post".
constexpr std::string_view kPostLong = "ADD_LONG 1000000";
constexpr std::string_view kPostString = "ADD_STRING cG9zdA==";

std::string ReferenceMatrixBlock(std::string_view pairing,
                                 const AdmittedState& receiver,
                                 const AdmittedState& operand,
                                 bool long_first) {
  std::string block = std::format(
      "MARK {}/{}/{}/{}\nRECEIVER {}\n", pairing, receiver.name, operand.name,
      long_first ? "long-first" : "string-first", receiver.reference_spec);
  block += PreludeCommands(receiver, "");
  block += "OPERAND " + operand.reference_spec + "\n";
  block += PreludeCommands(operand, "OPERAND_");
  block += "MERGE_OPERAND\nCHECKPOINT\nRESULT\n";
  const std::string_view first = long_first ? kPostLong : kPostString;
  const std::string_view second = long_first ? kPostString : kPostLong;
  block += std::format("{}\nCHECKPOINT\n{}\nCHECKPOINT\n", first, second);
  return block;
}

// The same block, performed by this library, printed as the reference
// prints it.
std::vector<std::string> CppMatrixBlock(const AdmittedState& receiver_state,
                                        const AdmittedState& operand_state,
                                        bool long_first) {
  std::vector<std::string> lines;
  auto receiver = BuildAdmittedState(receiver_state);
  auto operand = BuildAdmittedState(operand_state);
  if (!receiver.has_value() || !operand.has_value()) {
    ADD_FAILURE() << "could not build " << receiver_state.name << " and "
                  << operand_state.name;
    return lines;
  }
  auto merged = receiver->Merge(std::move(*operand));
  if (!merged.has_value()) {
    lines.push_back("ERROR " + merged.error().message);
  } else {
    // What a merge that went through leaves behind must pass the full
    // walk, since the reference writes the same bytes. A merge refused
    // part way can leave the receiver with its sparse size recorded and
    // its stream cleared, which the walk refuses until the next write.
    auto valid = receiver->Validate();
    EXPECT_TRUE(valid.has_value())
        << receiver_state.name << " x " << operand_state.name
        << (valid.has_value() ? "" : ": " + valid.error().message);
  }
  const auto checkpoint = [&receiver, &lines]() {
    auto bytes = receiver->Serialize();
    lines.push_back(bytes.has_value() ? PrintHex(*bytes)
                                      : "ERROR " + bytes.error().message);
  };
  checkpoint();
  auto result = receiver->Result();
  lines.push_back(result.has_value() ? std::to_string(*result)
                                     : "ERROR " + result.error().message);
  const auto add = [&receiver, &lines, &checkpoint](bool as_long) {
    auto added =
        as_long ? receiver->Add(int64_t{1000000}) : receiver->Add("post");
    if (!added.has_value()) {
      lines.push_back("ERROR " + added.error().message);
    }
    checkpoint();
  };
  add(long_first);
  add(!long_first);
  return lines;
}

// Every pair of states a receiver and an operand can be in, merged both
// as the reference merges them and as this library does, then given
// each kind of addition this library offers, in both orders. What is
// compared is everything the reference prints: whether the merge was
// refused and in what words, the bytes and the estimate afterwards, and
// for each addition whether it was refused and in what words and the
// bytes it left. The last is where a merge that narrowed the set shows:
// an addition to a set narrowed to one kind writes no value type, in
// either library.
//
// The whole matrix runs at six pairings of configuration, so that the
// admitted set is exercised whichever representation each side is in:
// both sparse; a dense receiver and a sparse operand, and the reverse;
// an operand at higher precisions than the receiver, which the merge
// lowers; a receiver at a higher sparse precision than the operand,
// holding unflushed values the downgrade carries across as they are;
// and both sides at the minimum precision with enough values added to
// have been promoted past the sparse threshold.
TEST(ReferenceLibraryTest,
     MergeAdmittedKindsAgreeWithTheReferenceInEveryState) {
  struct Pairing {
    std::string_view name;
    Side receiver;
    Side operand;
  };
  const std::array<Pairing, 6> pairings = {{
      {.name = "sparse-sparse",
       .receiver = {.np = 10, .sp = 15, .values = 1},
       .operand = {.np = 10, .sp = 15, .values = 1}},
      {.name = "dense-sparse",
       .receiver = {.np = 10, .sp = 0, .values = 1},
       .operand = {.np = 10, .sp = 15, .values = 1}},
      {.name = "sparse-dense",
       .receiver = {.np = 10, .sp = 15, .values = 1},
       .operand = {.np = 10, .sp = 0, .values = 1}},
      {.name = "operand-at-higher-precisions",
       .receiver = {.np = 10, .sp = 15, .values = 1},
       .operand = {.np = 15, .sp = 20, .values = 1}},
      {.name = "receiver-at-higher-sparse-precision",
       .receiver = {.np = 10, .sp = 20, .values = 3},
       .operand = {.np = 10, .sp = 15, .values = 3}},
      {.name = "promoted",
       .receiver = {.np = 4, .sp = 9, .values = 30},
       .operand = {.np = 4, .sp = 9, .values = 30}},
  }};

  for (const Pairing& pairing : pairings) {
    const std::vector<AdmittedState> receivers =
        AdmittedStates(pairing.receiver);
    const std::vector<AdmittedState> operands = AdmittedStates(pairing.operand);
    std::string script;
    for (const AdmittedState& receiver : receivers) {
      for (const AdmittedState& operand : operands) {
        script += ReferenceMatrixBlock(pairing.name, receiver, operand, true);
        script += ReferenceMatrixBlock(pairing.name, receiver, operand, false);
      }
    }
    const auto blocks = SplitAtMarks(RunScriptWithReceivers(script));
    ASSERT_EQ(blocks.size(), receivers.size() * operands.size() * 2)
        << pairing.name;

    size_t index = 0;
    for (const AdmittedState& receiver : receivers) {
      for (const AdmittedState& operand : operands) {
        for (const bool long_first : {true, false}) {
          const auto& [name, reference] = blocks.at(index++);
          ASSERT_EQ(name,
                    std::format("{}/{}/{}/{}", pairing.name, receiver.name,
                                operand.name,
                                long_first ? "long-first" : "string-first"));
          ASSERT_FALSE(reference.empty()) << name;
          EXPECT_EQ(CppMatrixBlock(receiver, operand, long_first), reference)
              << name;
        }
      }
    }
  }
}

// One state of the sweep below as two blocks of the reference's script:
// the receiver and the operand built and given their strings and
// merged, then, under a second mark, the receiver written and
// estimated, given three more strings and written again. The second
// mark is what makes a refusal at the merge distinguishable from one at
// the first write, since a merge that goes through prints nothing.
std::string ReferenceSweepBlock(std::string_view name, int32_t np,
                                int32_t receiver_sp, int receiver_population,
                                int32_t operand_sp, int operand_population) {
  std::string block =
      std::format("MARK {}\nRECEIVER strings {} {}\n", name, np, receiver_sp);
  for (int i = 0; i < receiver_population; ++i) {
    block +=
        std::format("ADD_STRING {}\n", EncodeBase64(std::format("r{}", i)));
  }
  block += std::format("OPERAND strings {} {}\n", np, operand_sp);
  for (int i = 0; i < operand_population; ++i) {
    block += std::format("OPERAND_ADD_STRING {}\n",
                         EncodeBase64(std::format("o{}", i)));
  }
  block +=
      std::format("MERGE_OPERAND\nMARK {}/merged\nCHECKPOINT\nRESULT\n", name);
  for (const std::string_view value : {"x", "y", "z"}) {
    block += std::format("ADD_STRING {}\n", EncodeBase64(value));
  }
  block += "CHECKPOINT\n";
  return block;
}

// What this library prints for one state of the sweep: the merge's
// refusal if there was one, and the lines after the merge as the
// reference prints them. A sketch this library writes before any
// refusal must pass the full walk, since its bytes are the reference's.
// After a refused merge nothing more is performed: the reference's own
// state is undefined there, so the lines it goes on to print are
// compared with nothing.
struct SweepOutcome {
  std::vector<std::string> merge;
  std::vector<std::string> after;
};

SweepOutcome CppSweepBlock(int32_t np, int32_t receiver_sp,
                           int receiver_population, int32_t operand_sp,
                           int operand_population) {
  SweepOutcome outcome;
  auto receiver = HyperLogLogPlusPlus::Create(np, receiver_sp);
  auto operand = HyperLogLogPlusPlus::Create(np, operand_sp);
  if (!receiver.has_value() || !operand.has_value()) {
    ADD_FAILURE() << "could not build (" << np << ", " << receiver_sp
                  << ") and (" << np << ", " << operand_sp << ")";
    return outcome;
  }
  for (int i = 0; i < receiver_population; ++i) {
    EXPECT_TRUE(receiver->Add(std::format("r{}", i)).has_value());
  }
  for (int i = 0; i < operand_population; ++i) {
    EXPECT_TRUE(operand->Add(std::format("o{}", i)).has_value());
  }
  auto merged = receiver->Merge(std::move(*operand));
  if (!merged.has_value()) {
    outcome.merge.push_back("ERROR " + merged.error().message);
    return outcome;
  }
  std::vector<std::string>& lines = outcome.after;
  bool refused = false;
  const auto checkpoint = [&receiver, &lines, &refused]() {
    auto bytes = receiver->Serialize();
    if (!bytes.has_value()) {
      lines.push_back("ERROR " + bytes.error().message);
      refused = true;
      return;
    }
    lines.push_back(PrintHex(*bytes));
    if (refused) return;
    auto reread = HyperLogLogPlusPlus::FromBytes(*bytes);
    ASSERT_TRUE(reread.has_value());
    auto valid = reread->Validate();
    EXPECT_TRUE(valid.has_value())
        << (valid.has_value() ? "" : valid.error().message);
  };
  checkpoint();
  auto result = receiver->Result();
  lines.push_back(result.has_value() ? std::to_string(*result)
                                     : "ERROR " + result.error().message);
  for (const std::string_view value : {"x", "y", "z"}) {
    auto added = receiver->Add(value);
    if (!added.has_value()) {
      lines.push_back("ERROR " + added.error().message);
      refused = true;
    }
  }
  checkpoint();
  return outcome;
}

// Cuts a block's lines after the first refusal and reduces that line to
// the fact of the refusal. Nothing after a refusal is compared: after a
// throw that interrupts a merge part way the reference's state is not
// defined. The wording is not compared either; it is the same in both
// libraries for a refusal of incompatible precisions or kinds, and
// differs where the reference reports an index out of bounds.
std::vector<std::string> UpToFirstRefusal(std::vector<std::string> lines) {
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].starts_with("ERROR")) {
      lines[i] = "ERROR";
      lines.resize(i + 1);
      break;
    }
  }
  return lines;
}

// Every merge across sparse precisions at normal precisions 4 to 12,
// with populations that leave values unflushed on either side, three
// additions after the merge, and a write and an estimate at each
// stage, performed by both libraries in one run of the reference and
// compared line for line. This is the shape that lowers a receiver to
// its operand's precision and carries the receiver's unflushed values
// across as they are, so the bytes depend on exactly which values were
// still unflushed, which is decided by where the flushes fell. The
// forty strings on either side repeat an encoding at some precisions,
// which is what parts a buffer that counts additions from one that
// counts distinct values.
//
// The reference throws in some of these states, at the merge or later,
// when a carried value is promoted under the lower precision and names
// a register outside the array. This library refuses the same states
// at the same stage.
TEST(ReferenceLibraryTest,
     EveryMergeAcrossSparsePrecisionsMatchesTheReference) {
  struct Sweep {
    std::string name;
    int32_t np;
    int32_t receiver_sp;
    int receiver_population;
    int32_t operand_sp;
    int operand_population;
  };
  constexpr std::array<int, 3> kPopulations = {0, 3, 40};
  std::vector<Sweep> sweeps;
  std::string script;
  for (int32_t np = HyperLogLogPlusPlus::kMinimumPrecision; np <= 12; ++np) {
    const std::array<int32_t, 6> sparse_precisions = {
        HyperLogLogPlusPlus::kSparsePrecisionDisabled,
        np,
        np + 1,
        np + 2,
        np + 5,
        HyperLogLogPlusPlus::kMaximumSparsePrecision};
    for (const int32_t receiver_sp : sparse_precisions) {
      for (const int32_t operand_sp : sparse_precisions) {
        for (const int receiver_population : kPopulations) {
          for (const int operand_population : kPopulations) {
            sweeps.push_back(
                {.name = std::format("np={}/receiver-sp={}x{}/operand-sp={}x{}",
                                     np, receiver_sp, receiver_population,
                                     operand_sp, operand_population),
                 .np = np,
                 .receiver_sp = receiver_sp,
                 .receiver_population = receiver_population,
                 .operand_sp = operand_sp,
                 .operand_population = operand_population});
            script += ReferenceSweepBlock(sweeps.back().name, np, receiver_sp,
                                          receiver_population, operand_sp,
                                          operand_population);
          }
        }
      }
    }
  }
  const auto blocks = SplitAtMarks(RunScriptWithReceivers(script));
  ASSERT_EQ(blocks.size(), sweeps.size() * 2);

  int refused_at_the_merge = 0;
  int refused_at_the_first_write = 0;
  int refused_after_the_additions = 0;
  for (size_t index = 0; index < sweeps.size(); ++index) {
    const Sweep& sweep = sweeps[index];
    const auto& [name, reference_merge] = blocks[index * 2];
    const auto& [merged_name, reference_after] = blocks[(index * 2) + 1];
    ASSERT_EQ(name, sweep.name);
    ASSERT_EQ(merged_name, sweep.name + "/merged");
    ASSERT_FALSE(reference_after.empty()) << name;

    const SweepOutcome mine =
        CppSweepBlock(sweep.np, sweep.receiver_sp, sweep.receiver_population,
                      sweep.operand_sp, sweep.operand_population);
    const std::vector<std::string> merge = UpToFirstRefusal(reference_merge);
    EXPECT_EQ(UpToFirstRefusal(mine.merge), merge) << name;
    const std::vector<std::string> after = UpToFirstRefusal(reference_after);
    const bool merge_refused = !merge.empty();
    const bool refused_later = after.back() == "ERROR";
    if (merge_refused) {
      ++refused_at_the_merge;
    } else {
      EXPECT_EQ(UpToFirstRefusal(mine.after), after) << name;
      if (refused_later && after.size() == 1) ++refused_at_the_first_write;
      if (refused_later && after.size() > 1) ++refused_after_the_additions;
    }
    if (merge_refused || refused_later) {
      // The reference throws only for a sparse receiver holding
      // unflushed values whose operand is sparse at a lower precision.
      EXPECT_TRUE(sweep.receiver_sp > sweep.operand_sp &&
                  sweep.operand_sp !=
                      HyperLogLogPlusPlus::kSparsePrecisionDisabled &&
                  sweep.receiver_population > 0)
          << name;
    }
  }
  // The states the reference throws in, of 2916, by stage: at the
  // merge, at the write after it, and at the write after the three
  // additions. The comparison above already holds this library to the
  // same states at the same stages; the counts are what would show a
  // change in the reference's own behaviour, or a script the harness no
  // longer runs as intended.
  EXPECT_EQ(refused_at_the_merge, 21);
  EXPECT_EQ(refused_at_the_first_write, 9);
  EXPECT_EQ(refused_after_the_additions, 6);
}

// A sketch built from a normal precision alone takes the sparse
// precision the reference's builder would choose. Every normal
// precision from one below the minimum to one above the maximum is
// built through both, for each value type the reference builds for,
// and compared empty, after additions where this library can make
// them, and estimated. The two refused precisions are compared in the
// words of their refusal.
TEST(ReferenceLibraryTest,
     TheDefaultSparsePrecisionAgreesWithTheReferenceBuilder) {
  using zetasketch::hll::ValueType;
  struct Kind {
    std::string_view word;
    ValueType value_type;
  };
  const std::array<Kind, 3> kinds = {{
      {.word = "strings", .value_type = ValueType::kBytesOrUtf8String},
      {.word = "longs", .value_type = ValueType::kUnsignedInt64},
      {.word = "integers", .value_type = ValueType::kUnsignedInt32},
  }};
  const int first = HyperLogLogPlusPlus::kMinimumPrecision - 1;
  const int last = HyperLogLogPlusPlus::kMaximumPrecision + 1;

  std::string script;
  for (int np = first; np <= last; ++np) {
    for (const Kind& kind : kinds) {
      script += std::format("MARK {}/{}\nRECEIVER {} {} default\nCHECKPOINT\n",
                            np, kind.word, kind.word, np);
      if (kind.word == "strings") {
        script += "ADD_STRING YQ==\nADD_STRING Yg==\nADD_STRING Yw==\n";
      } else if (kind.word == "longs") {
        script += "ADD_LONG 1\nADD_LONG 2\nADD_LONG 3\n";
      }
      script += "CHECKPOINT\nRESULT\n";
    }
  }
  const auto blocks = SplitAtMarks(RunScriptWithReceivers(script));
  ASSERT_EQ(blocks.size(),
            static_cast<size_t>(last - first + 1) * kinds.size());

  size_t index = 0;
  for (int np = first; np <= last; ++np) {
    for (const Kind& kind : kinds) {
      const auto& [name, reference] = blocks.at(index++);
      ASSERT_EQ(name, std::format("{}/{}", np, kind.word));
      ASSERT_FALSE(reference.empty()) << name;

      auto sketch = HyperLogLogPlusPlus::Create(np, kind.value_type);
      if (!sketch.has_value()) {
        // A refused receiver leaves the reference with none, and every
        // command after it in the block reports that; nothing else may
        // appear.
        EXPECT_EQ("ERROR " + sketch.error().message, reference.front()) << name;
        for (size_t line = 1; line < reference.size(); ++line) {
          EXPECT_TRUE(reference.at(line).starts_with("BADINPUT "))
              << name << ": " << reference.at(line);
        }
        continue;
      }
      std::vector<std::string> mine;
      const auto checkpoint = [&sketch, &mine]() {
        auto bytes = sketch->Serialize();
        mine.push_back(bytes.has_value() ? PrintHex(*bytes)
                                         : "ERROR " + bytes.error().message);
      };
      checkpoint();
      if (kind.word == "strings") {
        for (const std::string_view value : {"a", "b", "c"}) {
          EXPECT_TRUE(sketch->Add(value).has_value()) << name;
        }
      } else if (kind.word == "longs") {
        for (const int64_t value : {1, 2, 3}) {
          EXPECT_TRUE(sketch->Add(value).has_value()) << name;
        }
      }
      checkpoint();
      auto result = sketch->Result();
      mine.push_back(result.has_value() ? std::to_string(*result)
                                        : "ERROR " + result.error().message);
      EXPECT_EQ(mine, reference) << name;
    }
  }
}

// The bounded guarantee. A sequence is a receiver configuration, an
// operand (its configuration and the steps that build it) and the
// steps performed on the receiver. Every addition, the building of the
// operand, the merge itself, every write and every estimate is one
// block of the reference's script, under its own mark, so that a
// refusal is located to the very addition or operation it happened at.
// The two libraries are compared block by block up to and including
// the first block in which the reference refused; after a throw that
// interrupts a merge part way the reference's state is not defined, so
// nothing after that block is compared. This library's remaining blocks
// are still performed, uncompared, since it must not fault whatever went
// before.
struct Step {
  enum class Kind : uint8_t {
    kAddStrings,
    kAddLongs,
    kMerge,
    kWrite,
    kEstimate
  };
  Kind kind;
  int count = 0;
};

// A sketch's precisions and the kind of value it is built for, which
// decides the kinds of addition it admits.
struct Configuration {
  int32_t np = 0;
  int32_t sp = 0;
  Step::Kind kind = Step::Kind::kAddStrings;
};

struct Operand {
  Configuration configuration;
  std::vector<Step> prelude;
};

struct Sequence {
  Configuration receiver;
  Operand operand;
  std::vector<Step> steps;
};

// The receiver's values are the strings s0, s1, ... and the longs 0,
// 1, ...; the operand's are o0, o1, ... and 1000, 1001, .... A step
// adding one value always adds the first, so repeated single additions
// repeat a value, which is what the distinct-value buffer is about; a
// step adding more begins where no earlier step of the same side
// reached, so the population grows with every such step and can
// promote the sketch. The operand's values never coincide with the
// receiver's.
constexpr int64_t kOperandLongBase = 1000;

int FirstValueOf(const std::vector<Step>& steps, size_t position) {
  if (steps[position].count == 1) return 0;
  int first = 0;
  for (size_t k = 0; k < position; ++k) {
    if (steps[k].count > 1) first += steps[k].count;
  }
  return first;
}

std::string Describe(const Step& step) {
  switch (step.kind) {
    case Step::Kind::kAddStrings:
      return std::format("S{}", step.count);
    case Step::Kind::kAddLongs:
      return std::format("L{}", step.count);
    case Step::Kind::kMerge:
      return "M";
    case Step::Kind::kWrite:
      return "W";
    case Step::Kind::kEstimate:
      return "E";
  }
  return "?";
}

std::string Describe(const std::vector<Step>& steps) {
  std::string text;
  for (const Step& step : steps) {
    text += (text.empty() ? "" : " ") + Describe(step);
  }
  return text;
}

// The word the harness builds a sketch of the kind with, and the value
// type this library builds it with.
std::string_view TypeWord(Step::Kind kind) {
  return kind == Step::Kind::kAddLongs ? "longs" : "strings";
}

zetasketch::hll::ValueType TypeOf(Step::Kind kind) {
  return kind == Step::Kind::kAddLongs
             ? zetasketch::hll::ValueType::kUnsignedInt64
             : zetasketch::hll::ValueType::kBytesOrUtf8String;
}

bool Merges(const Sequence& sequence) {
  return std::ranges::any_of(sequence.steps, [](const Step& step) {
    return step.kind == Step::Kind::kMerge;
  });
}

std::string Describe(const Sequence& sequence) {
  std::string text =
      std::format("receiver {} ({}, {})", TypeWord(sequence.receiver.kind),
                  sequence.receiver.np, sequence.receiver.sp);
  if (Merges(sequence)) {
    text += std::format("; operand {} ({}, {}) [{}]",
                        TypeWord(sequence.operand.configuration.kind),
                        sequence.operand.configuration.np,
                        sequence.operand.configuration.sp,
                        Describe(sequence.operand.prelude));
  }
  return text + "; steps: " + Describe(sequence.steps);
}

// One block of a sequence: what the reference's script says under its
// mark, and what this library does for it.
struct Block {
  enum class Action : uint8_t {
    kAddString,
    kAddLong,
    kBuildOperand,
    kMerge,
    kWrite,
    kEstimate
  };
  std::string name;
  Action action;
  std::string value;
  int64_t number = 0;
};

std::string OperandScript(const Sequence& sequence) {
  const Operand& operand = sequence.operand;
  std::string script =
      std::format("OPERAND {} {} {}\n", TypeWord(operand.configuration.kind),
                  operand.configuration.np, operand.configuration.sp);
  for (size_t p = 0; p < operand.prelude.size(); ++p) {
    const Step& step = operand.prelude[p];
    const int first = FirstValueOf(operand.prelude, p);
    switch (step.kind) {
      case Step::Kind::kAddStrings:
        for (int i = 0; i < step.count; ++i) {
          script += std::format("OPERAND_ADD_STRING {}\n",
                                EncodeBase64(std::format("o{}", first + i)));
        }
        break;
      case Step::Kind::kAddLongs:
        for (int i = 0; i < step.count; ++i) {
          script += std::format("OPERAND_ADD_LONG {}\n",
                                kOperandLongBase + first + i);
        }
        break;
      case Step::Kind::kWrite:
        script += "OPERAND_CHECKPOINT\n";
        break;
      case Step::Kind::kMerge:
      case Step::Kind::kEstimate:
        ADD_FAILURE() << "an operand is built by additions and writes";
        break;
    }
  }
  return script;
}

// The blocks of a sequence, in order: "<index>/0" builds the receiver,
// "<index>/<step>.<n>" is the n-th addition of an adding step,
// "<index>/<step>.operand" builds the operand for a merging step and
// "<index>/<step>" is the merge, write or estimate itself.
std::vector<Block> BlocksOf(const Sequence& sequence, size_t index) {
  std::vector<Block> blocks;
  for (size_t k = 0; k < sequence.steps.size(); ++k) {
    const Step& step = sequence.steps[k];
    const std::string prefix = std::format("{}/{}", index, k + 1);
    const int first = FirstValueOf(sequence.steps, k);
    switch (step.kind) {
      case Step::Kind::kAddStrings:
        for (int i = 0; i < step.count; ++i) {
          blocks.push_back({.name = std::format("{}.{}", prefix, i + 1),
                            .action = Block::Action::kAddString,
                            .value = std::format("s{}", first + i),
                            .number = 0});
        }
        break;
      case Step::Kind::kAddLongs:
        for (int i = 0; i < step.count; ++i) {
          blocks.push_back({.name = std::format("{}.{}", prefix, i + 1),
                            .action = Block::Action::kAddLong,
                            .value = {},
                            .number = first + i});
        }
        break;
      case Step::Kind::kMerge:
        blocks.push_back({.name = prefix + ".operand",
                          .action = Block::Action::kBuildOperand,
                          .value = {},
                          .number = 0});
        blocks.push_back({.name = prefix,
                          .action = Block::Action::kMerge,
                          .value = {},
                          .number = 0});
        break;
      case Step::Kind::kWrite:
        blocks.push_back({.name = prefix,
                          .action = Block::Action::kWrite,
                          .value = {},
                          .number = 0});
        break;
      case Step::Kind::kEstimate:
        blocks.push_back({.name = prefix,
                          .action = Block::Action::kEstimate,
                          .value = {},
                          .number = 0});
        break;
    }
  }
  return blocks;
}

// The reference's script for one sequence.
std::string ReferenceScript(const Sequence& sequence, size_t index,
                            const std::vector<Block>& blocks) {
  std::string script = std::format("MARK {}/0\nRECEIVER {} {} {}\n", index,
                                   TypeWord(sequence.receiver.kind),
                                   sequence.receiver.np, sequence.receiver.sp);
  for (const Block& block : blocks) {
    script += std::format("MARK {}\n", block.name);
    switch (block.action) {
      case Block::Action::kAddString:
        script += std::format("ADD_STRING {}\n", EncodeBase64(block.value));
        break;
      case Block::Action::kAddLong:
        script += std::format("ADD_LONG {}\n", block.number);
        break;
      case Block::Action::kBuildOperand:
        script += OperandScript(sequence);
        break;
      case Block::Action::kMerge:
        script += "MERGE_OPERAND\n";
        break;
      case Block::Action::kWrite:
        script += "CHECKPOINT\n";
        break;
      case Block::Action::kEstimate:
        script += "RESULT\n";
        break;
    }
  }
  return script;
}

// This library's side of one sequence: the receiver, and the operand
// between its building and the merge.
struct Performer {
  HyperLogLogPlusPlus receiver;
  std::optional<HyperLogLogPlusPlus> operand;
};

// Performs one block and returns the lines the reference prints for it.
// A write also reads its bytes back and requires the sketch read to
// pass the walk, estimate as the receiver does, and write the same
// bytes again: FromBytes is in the grammar as the identity it must be.
std::vector<std::string> Perform(Performer& performer, const Sequence& sequence,
                                 const Block& block) {
  std::vector<std::string> lines;
  const auto refused = [&lines](const zetasketch::utils::Error& error) {
    lines.push_back("ERROR " + error.message);
  };
  switch (block.action) {
    case Block::Action::kAddString: {
      auto added = performer.receiver.Add(block.value);
      if (!added.has_value()) refused(added.error());
      break;
    }
    case Block::Action::kAddLong: {
      auto added = performer.receiver.Add(block.number);
      if (!added.has_value()) refused(added.error());
      break;
    }
    case Block::Action::kBuildOperand: {
      const Operand& operand = sequence.operand;
      auto built = HyperLogLogPlusPlus::Create(
          operand.configuration.np, operand.configuration.sp,
          TypeOf(operand.configuration.kind));
      if (!built.has_value()) {
        refused(built.error());
        performer.operand.reset();
        break;
      }
      for (size_t p = 0; p < operand.prelude.size(); ++p) {
        const Step& step = operand.prelude[p];
        const int first = FirstValueOf(operand.prelude, p);
        switch (step.kind) {
          case Step::Kind::kAddStrings:
            for (int i = 0; i < step.count; ++i) {
              auto added = built->Add(std::format("o{}", first + i));
              if (!added.has_value()) refused(added.error());
            }
            break;
          case Step::Kind::kAddLongs:
            for (int i = 0; i < step.count; ++i) {
              auto added = built->Add(kOperandLongBase + first + i);
              if (!added.has_value()) refused(added.error());
            }
            break;
          case Step::Kind::kWrite: {
            auto bytes = built->Serialize();
            if (bytes.has_value()) {
              lines.push_back(PrintHex(*bytes));
            } else {
              refused(bytes.error());
            }
            break;
          }
          case Step::Kind::kMerge:
          case Step::Kind::kEstimate:
            break;
        }
      }
      performer.operand = std::move(*built);
      break;
    }
    case Block::Action::kMerge: {
      if (!performer.operand.has_value()) {
        ADD_FAILURE() << Describe(sequence) << ": a merge with no operand";
        break;
      }
      auto merged = performer.receiver.Merge(std::move(*performer.operand));
      performer.operand.reset();
      if (!merged.has_value()) refused(merged.error());
      break;
    }
    case Block::Action::kWrite: {
      auto bytes = performer.receiver.Serialize();
      if (!bytes.has_value()) {
        refused(bytes.error());
        break;
      }
      lines.push_back(PrintHex(*bytes));
      auto reread = HyperLogLogPlusPlus::FromBytes(*bytes);
      EXPECT_TRUE(reread.has_value()) << Describe(sequence);
      if (!reread.has_value()) break;
      auto valid = reread->Validate();
      EXPECT_TRUE(valid.has_value())
          << Describe(sequence)
          << (valid.has_value() ? "" : ": " + valid.error().message);
      auto estimate_read = reread->Result();
      auto estimate_written = performer.receiver.Result();
      EXPECT_TRUE(estimate_read.has_value() && estimate_written.has_value() &&
                  *estimate_read == *estimate_written)
          << Describe(sequence);
      auto again = reread->Serialize();
      EXPECT_TRUE(again.has_value() && *again == *bytes) << Describe(sequence);
      break;
    }
    case Block::Action::kEstimate: {
      auto result = performer.receiver.Result();
      lines.push_back(result.has_value() ? std::to_string(*result)
                                         : "ERROR " + result.error().message);
      break;
    }
  }
  return lines;
}

// A digest of the sequences, printed beside the seed, so that a replay
// can be seen to have generated the same sequences and not only the
// same count of refusals. FNV-1a over the descriptions; the standard
// library's hash is not fixed across platforms, this is.
uint64_t DigestOf(const std::vector<Sequence>& sequences) {
  constexpr uint64_t kOffset = 0xcbf29ce484222325ULL;
  constexpr uint64_t kPrime = 0x100000001b3ULL;
  uint64_t digest = kOffset;
  for (const Sequence& sequence : sequences) {
    for (const char character : Describe(sequence) + "\n") {
      digest ^= static_cast<uint8_t>(character);
      digest *= kPrime;
    }
  }
  return digest;
}

// Runs every sequence through the reference in one start and through
// this library, and compares them block by block. Returns the indices
// of the sequences in which the reference refused a block.
std::vector<size_t> CompareWithTheReference(
    const std::vector<Sequence>& sequences, std::string_view context) {
  std::vector<std::vector<Block>> blocks_of;
  blocks_of.reserve(sequences.size());
  std::string script;
  size_t blocks_expected = 0;
  for (size_t index = 0; index < sequences.size(); ++index) {
    blocks_of.push_back(BlocksOf(sequences[index], index));
    script += ReferenceScript(sequences[index], index, blocks_of.back());
    blocks_expected += blocks_of.back().size() + 1;
  }
  const auto blocks = SplitAtMarks(RunScriptWithReceivers(script));
  std::vector<size_t> sequences_refused;
  if (blocks.size() != blocks_expected) {
    ADD_FAILURE() << context << ": the reference printed " << blocks.size()
                  << " blocks where " << blocks_expected << " were expected";
    return sequences_refused;
  }

  size_t block = 0;
  for (size_t index = 0; index < sequences.size(); ++index) {
    const Sequence& sequence = sequences[index];
    const std::string description = Describe(sequence);
    auto receiver =
        HyperLogLogPlusPlus::Create(sequence.receiver.np, sequence.receiver.sp,
                                    TypeOf(sequence.receiver.kind));
    EXPECT_TRUE(receiver.has_value()) << description;
    EXPECT_EQ(blocks[block].first, std::format("{}/0", index)) << description;
    EXPECT_TRUE(blocks[block].second.empty())
        << description << ": " << blocks[block].second.front();
    ++block;
    if (!receiver.has_value()) {
      block += blocks_of[index].size();
      continue;
    }
    Performer performer{.receiver = std::move(*receiver),
                        .operand = std::nullopt};
    bool refused = false;
    for (const Block& expected_block : blocks_of[index]) {
      const auto& [name, reference] = blocks[block++];
      EXPECT_EQ(name, expected_block.name) << description;
      const std::vector<std::string> mine =
          UpToFirstRefusal(Perform(performer, sequence, expected_block));
      if (refused) continue;
      const std::vector<std::string> expected = UpToFirstRefusal(reference);
      EXPECT_EQ(mine, expected)
          << description << "; at block " << name << "; " << context;
      if (!expected.empty() && expected.back() == "ERROR") {
        refused = true;
        sequences_refused.push_back(index);
      }
      if (mine != expected) refused = true;
    }
  }
  return sequences_refused;
}

// Whether a sequence has one of the two shapes in which the reference
// is known to throw. The first: two sparse configurations of which one
// is higher in normal precision and lower in sparse precision, which
// the reference's encodings declare incompatible; the check is made
// only while the receiver is sparse, so a receiver promoted before the
// merge is not refused. The second: a sparse receiver given values and
// then a sparse operand at a lower sparse precision, whose unflushed
// values the lowering carries across at their old precision, to be
// promoted later under the new one outside the register array. Both
// are necessary shapes, not sufficient ones: whether the receiver is
// still sparse, and whether values are still unflushed, depend on the
// counts.
bool HasAKnownRefusalShape(const Sequence& sequence) {
  const Configuration& receiver = sequence.receiver;
  const Configuration& operand = sequence.operand.configuration;
  if (!Merges(sequence) || receiver.sp == 0 || operand.sp == 0) return false;
  if ((receiver.np < operand.np && receiver.sp > operand.sp) ||
      (receiver.np > operand.np && receiver.sp < operand.sp)) {
    return true;
  }
  if (operand.sp >= receiver.sp) return false;
  for (const Step& step : sequence.steps) {
    if (step.kind == Step::Kind::kMerge) return false;
    if (step.kind == Step::Kind::kAddStrings ||
        step.kind == Step::Kind::kAddLongs) {
      return true;
    }
  }
  return false;
}

// The exhaustive space. Every sequence of one to three steps over six
// operations, for each kind of value, at every receiver configuration
// and, where the sequence merges, every operand state. The operations:
// add one value, add m/4 + 1 values, add 2m values (m being 2^np of the
// sketch added to, so that the second count flushes the buffer and the
// third promotes the sketch), merge, write,
// estimate; the values all strings or all longs, on both sides, since a
// sequence mixing the kinds is refused at its second kind by both
// libraries, which the admitted-kinds matrix already compares in every
// state. The receiver is at normal precision 4, 5 or 6 with sparse mode
// disabled, at the normal precision, one above it, or at 25; the
// operand at the receiver's normal precision or the next, with sparse
// mode disabled, at its own normal precision, or at 25, holding
// nothing, one value, m/4 + 1 values or 2m values. An operand written before
// the merge is not a further state: the receiver reads the operand's buffer
// whether flushed or not, and m/4 + 1 values have flushed it already; measured,
// the receiver's bytes and estimates are the same. That is, for each kind, 12
// receivers by 258 sequences, the 103 of which that merge taken at all 24
// operand states: 31,524 sequences, 63,048 for both kinds.
//
// The operand is never at a lower normal precision than the receiver.
// Merging one into a dense receiver lowers the receiver's state but
// not the reference's own encoding of it, so the reference's later
// additions index outside the array and throw, or land in the wrong
// register, and a sketch written after those additions carries the
// miscount. The merge itself writes correctly; the defect is in what
// the reference's object does next, and it is not compared.
constexpr size_t kExhaustiveSequences = 63048;

std::vector<Sequence> EveryShortSequence() {
  std::vector<Sequence> sequences;
  constexpr int kLongestSequence = 3;
  for (const Step::Kind kind :
       {Step::Kind::kAddStrings, Step::Kind::kAddLongs}) {
    for (const int32_t np : {4, 5, 6}) {
      const int m = static_cast<int>(1U << static_cast<uint32_t>(np));
      const int many = (m / 4) + 1;
      const int promoting = 2 * m;
      const std::array<Step, 6> alphabet = {{
          {.kind = kind, .count = 1},
          {.kind = kind, .count = many},
          {.kind = kind, .count = promoting},
          {.kind = Step::Kind::kMerge},
          {.kind = Step::Kind::kWrite},
          {.kind = Step::Kind::kEstimate},
      }};
      for (const int32_t sp : {0, np, np + 1, 25}) {
        const Configuration receiver = {.np = np, .sp = sp, .kind = kind};
        for (int length = 1; length <= kLongestSequence; ++length) {
          std::vector<size_t> digits(static_cast<size_t>(length), 0);
          while (true) {
            std::vector<Step> steps;
            steps.reserve(digits.size());
            for (const size_t digit : digits)
              steps.push_back(alphabet.at(digit));
            const Sequence without_merge = {
                .receiver = receiver,
                .operand = {.configuration = receiver, .prelude = {}},
                .steps = steps};
            if (!Merges(without_merge)) {
              sequences.push_back(without_merge);
            } else {
              for (const int32_t onp : {np, np + 1}) {
                const int operand_m =
                    static_cast<int>(1U << static_cast<uint32_t>(onp));
                const std::array<std::vector<Step>, 4> preludes = {{
                    {},
                    {{.kind = kind, .count = 1}},
                    {{.kind = kind, .count = (operand_m / 4) + 1}},
                    {{.kind = kind, .count = 2 * operand_m}},
                }};
                for (const int32_t osp : {0, onp, 25}) {
                  for (const std::vector<Step>& prelude : preludes) {
                    sequences.push_back(
                        {.receiver = receiver,
                         .operand = {.configuration = {.np = onp,
                                                       .sp = osp,
                                                       .kind = kind},
                                     .prelude = prelude},
                         .steps = steps});
                  }
                }
              }
            }
            // The next combination, counting in base six.
            size_t position = 0;
            while (position < digits.size() &&
                   ++digits[position] == alphabet.size()) {
              digits[position] = 0;
              ++position;
            }
            if (position == digits.size()) break;
          }
        }
      }
    }
  }
  return sequences;
}

TEST(ReferenceLibraryTest, EveryShortSequenceMatchesTheReference) {
  const std::vector<Sequence> sequences = EveryShortSequence();
  ASSERT_EQ(sequences.size(), kExhaustiveSequences);
  const std::vector<size_t> refused =
      CompareWithTheReference(sequences, "exhaustive space");
  // Every sequence the reference refuses has one of the two shapes it is
  // known to throw in; a refusal of any other shape would be a third
  // behaviour of the reference's that this library happened to share.
  int unexplained = 0;
  for (const size_t index : refused) {
    if (!HasAKnownRefusalShape(sequences[index])) {
      ++unexplained;
      ADD_FAILURE() << "refused in a shape not known to throw: "
                    << Describe(sequences[index]);
    }
  }
  // The number the reference throws in: a change in it would be a
  // change in the reference's behaviour or in the script the harness
  // runs, since the comparison above already holds this library to the
  // same sequences.
  EXPECT_EQ(refused.size(), 1358U);
  std::cout << "sequences compared: " << sequences.size()
            << "; refused by the reference at some block: " << refused.size()
            << "; of an unknown shape: " << unexplained << "\n";
  ::testing::Test::RecordProperty("sequences",
                                  static_cast<int>(sequences.size()));
  ::testing::Test::RecordProperty("refused", static_cast<int>(refused.size()));
}

// The exhaustive space's claim that its largest addition, 2m values,
// promotes the sketch at every receiver configuration and for both
// kinds: after that one step the written sketch holds a register array.
TEST(ReferenceLibraryTest, ThePromotingAdditionPromotesAtEveryConfiguration) {
  for (const Step::Kind kind :
       {Step::Kind::kAddStrings, Step::Kind::kAddLongs}) {
    for (const int32_t np : {4, 5, 6}) {
      const int promoting =
          2 * static_cast<int>(1U << static_cast<uint32_t>(np));
      for (const int32_t sp : {0, np, np + 1, 25}) {
        const Sequence sequence = {
            .receiver = {.np = np, .sp = sp, .kind = kind},
            .operand = {},
            .steps = {{.kind = kind, .count = promoting}}};
        auto receiver = HyperLogLogPlusPlus::Create(np, sp, TypeOf(kind));
        ASSERT_TRUE(receiver.has_value());
        Performer performer{.receiver = std::move(*receiver),
                            .operand = std::nullopt};
        for (const Block& block : BlocksOf(sequence, 0)) {
          EXPECT_TRUE(Perform(performer, sequence, block).empty())
              << Describe(sequence);
        }
        auto bytes = performer.receiver.Serialize();
        ASSERT_TRUE(bytes.has_value()) << Describe(sequence);
        auto state = zetasketch::hll::State::Parse(*bytes);
        ASSERT_TRUE(state.has_value()) << Describe(sequence);
        EXPECT_TRUE(state->data.has_value() && !state->data->empty())
            << Describe(sequence) << " is still sparse";
      }
    }
  }
}

// The random space. A fresh sample of longer sequences on every run,
// over the whole range of normal precisions, with additions of up to m
// values, which promote the sketch at every precision. The seed is
// taken from --seed= when given, otherwise from the clock, and is
// printed first with a digest of the sequences it produced, so that any
// failure can be replayed exactly with --seed=<value> and the replay
// seen to be the same sample. The generator draws from std::mt19937_64
// alone, whose output the standard fixes, so a seed replays the same
// sequences on every platform.
std::optional<uint64_t>& SeedFlag() {
  static std::optional<uint64_t> seed;
  return seed;
}

constexpr int32_t kLowestNormal = 4;
constexpr int32_t kHighestNormal = 12;
constexpr int32_t kHighestSparse = 25;
constexpr size_t kLongestRandomSequence = 12;
constexpr size_t kRandomSequences = 600;

std::vector<Sequence> RandomSequences(uint64_t seed, size_t count) {
  std::mt19937_64 engine(seed);
  const auto pick = [&engine](size_t choices) {
    return static_cast<size_t>(engine() % choices);
  };

  const auto sparse_for = [&pick](int32_t np) -> int32_t {
    if (pick(2) == 0) return 0;
    return np + static_cast<int32_t>(pick(static_cast<size_t>(kHighestSparse) -
                                          static_cast<size_t>(np) + 1));
  };
  // One value, three, the count that flushes the buffer, or, one time in
  // eight, 2m values, which promote the sketch at any sparse precision.
  const auto count_for = [&pick](int32_t np) {
    const int m = static_cast<int>(1U << static_cast<uint32_t>(np));
    if (pick(8) == 0) return 2 * m;
    const std::array<int, 3> counts = {1, 3, (m / 4) + 1};
    return counts.at(pick(counts.size()));
  };
  const auto other_kind = [](Step::Kind kind) {
    return kind == Step::Kind::kAddStrings ? Step::Kind::kAddLongs
                                           : Step::Kind::kAddStrings;
  };

  std::vector<Sequence> sequences;
  sequences.reserve(count);
  for (size_t n = 0; n < count; ++n) {
    Sequence sequence;
    const auto np = static_cast<int32_t>(
        kLowestNormal + pick(static_cast<size_t>(kHighestNormal) -
                             static_cast<size_t>(kLowestNormal) + 1));
    // One sequence in four is drawn to mix the kinds of value: its operand is
    // built for the other kind, and each of its additions may be of
    // either kind. Both libraries refuse the crossing, at the merge or
    // at the addition, and the refusal is compared like any other.
    const bool mixed = pick(4) == 0;
    const Step::Kind kind =
        pick(2) == 0 ? Step::Kind::kAddStrings : Step::Kind::kAddLongs;
    sequence.receiver = {.np = np, .sp = sparse_for(np), .kind = kind};
    // At the receiver's normal precision or the next, never lower, for
    // the reason given at EveryShortSequence.
    const int32_t onp = np + static_cast<int32_t>(pick(2));
    const Step::Kind operand_kind = mixed ? other_kind(kind) : kind;
    sequence.operand.configuration = {
        .np = onp, .sp = sparse_for(onp), .kind = operand_kind};
    const size_t prelude_length = pick(3);
    for (size_t k = 0; k < prelude_length; ++k) {
      if (pick(4) == 0) {
        sequence.operand.prelude.push_back({.kind = Step::Kind::kWrite});
      } else {
        sequence.operand.prelude.push_back(
            {.kind = operand_kind, .count = count_for(onp)});
      }
    }
    const size_t length = 1 + pick(kLongestRandomSequence);
    for (size_t k = 0; k < length; ++k) {
      switch (pick(5)) {
        case 0:
        case 1:
          sequence.steps.push_back(
              {.kind = mixed && pick(2) == 0 ? other_kind(kind) : kind,
               .count = count_for(np)});
          break;
        case 2:
          sequence.steps.push_back({.kind = Step::Kind::kMerge});
          break;
        case 3:
          sequence.steps.push_back({.kind = Step::Kind::kWrite});
          break;
        default:
          sequence.steps.push_back({.kind = Step::Kind::kEstimate});
          break;
      }
    }
    sequences.push_back(std::move(sequence));
  }
  return sequences;
}

TEST(ReferenceLibraryTest, RandomSequencesMatchTheReference) {
  const uint64_t seed = SeedFlag().value_or(static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::vector<Sequence> sequences =
      RandomSequences(seed, kRandomSequences);
  ASSERT_EQ(sequences.size(), kRandomSequences);
  const uint64_t digest = DigestOf(sequences);
  std::cout << std::format(
      "seed={} digest={:016x} (replay with --test_arg=--seed={})\n", seed,
      digest, seed);
  ::testing::Test::RecordProperty("seed", std::to_string(seed));
  ::testing::Test::RecordProperty("digest", std::format("{:016x}", digest));
  const std::vector<size_t> refused = CompareWithTheReference(
      sequences, std::format("random space, seed {}", seed));
  std::cout << "sequences compared: " << sequences.size()
            << "; refused by the reference at some block: " << refused.size()
            << "\n";
  ::testing::Test::RecordProperty("refused", static_cast<int>(refused.size()));
}

}  // namespace

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  constexpr std::string_view kFlag = "--java_cli=";
  constexpr std::string_view kSeedFlag = "--seed=";

  const std::span<char*> args_span(argv, argc);
  for (size_t i = 1; i < args_span.size(); ++i) {
    const std::string_view arg = args_span[i];
    if (arg.starts_with(kFlag)) {
      JavaCliFlag() = arg.substr(kFlag.length());
    } else if (arg.starts_with(kSeedFlag)) {
      const std::string_view digits = arg.substr(kSeedFlag.length());
      uint64_t seed = 0;
      const auto parsed = std::from_chars(digits.begin(), digits.end(), seed);
      if (parsed.ec != std::errc() || parsed.ptr != digits.end()) {
        std::cerr << "Error: --seed= takes an unsigned integer\n";
        return 1;
      }
      SeedFlag() = seed;
    }
  }

  if (JavaCliFlag().empty()) {
    std::cerr << "Error: --java_cli= flag is required\n";
    return 1;
  }
  return RUN_ALL_TESTS();
}
