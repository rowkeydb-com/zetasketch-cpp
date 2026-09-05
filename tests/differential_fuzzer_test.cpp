#include <stdlib.h>
#include <unistd.h>
#include <array>
#include <bit>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iterator>
#include <numeric>
#include <optional>
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

namespace {

using zetasketch::HyperLogLogPlusPlus;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,fuchsia-statically-constructed-objects)
std::string g_java_cli;

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
  if (g_java_cli.empty()) {
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
  owned_arguments.push_back(g_java_cli);
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

    execv(g_java_cli.c_str(), argv.data());
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
