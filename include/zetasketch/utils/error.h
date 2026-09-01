// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_UTILS_ERROR_H_
#define ZETASKETCH_UTILS_ERROR_H_

#include <string>

namespace zetasketch::utils {

// The error vocabulary shared by all ZetaSketch operations.
enum class ErrorCode {
  kInvalidState,
  kIllegalArgument,
  kIncompatiblePrecision,
  kProtoDeserialization,
  kProtoSerialization,
};

struct Error {
  ErrorCode code;
  std::string message;
};

}  // namespace zetasketch::utils

#endif  // ZETASKETCH_UTILS_ERROR_H_
