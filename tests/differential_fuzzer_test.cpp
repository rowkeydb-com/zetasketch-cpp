#include <stdlib.h>
#include <unistd.h>
#include <array>
#include <bit>
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

std::string RunJava(const std::string& mode, int np, int sp,
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
// Decodes a sketch the harness reported as hexadecimal.
std::vector<uint8_t> ParseHexString(std::string_view hex) {
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
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
