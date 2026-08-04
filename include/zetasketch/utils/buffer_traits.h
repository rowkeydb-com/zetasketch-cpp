// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_UTILS_BUFFER_TRAITS_H_
#define ZETASKETCH_UTILS_BUFFER_TRAITS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "zetasketch/utils/var_int.h"

namespace zetasketch::utils {

// This structure defines the base error enumeration for ZetaSketch operations,
// which is equivalent to the SketchError enumeration in the Rust
// implementation.
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

class BufferReader {
 public:
  explicit BufferReader(std::span<const uint8_t> data) : data_(data) {}

  std::expected<int32_t, Error> ReadVarInt() {
    if (position_ >= data_.size()) {
      return std::unexpected(Error{.code = ErrorCode::kInvalidState,
                                   .message = "No more data to read"});
    }
    auto decoded = VarInt::Get(data_.subspan(position_));
    position_ += decoded.bytes_read;
    return decoded.value;
  }

  [[nodiscard]] bool HasRemaining() const { return position_ < data_.size(); }
  [[nodiscard]] size_t Remaining() const { return data_.size() - position_; }

 private:
  std::span<const uint8_t> data_;
  size_t position_ = 0;
};

class BufferWriter {
 public:
  BufferWriter() = default;
  explicit BufferWriter(std::vector<uint8_t>& out_buffer)
      : data_(&out_buffer) {}

  void WriteVarInt(int32_t value) {
    const size_t size_needed = VarInt::Size(value);
    const size_t start_pos = data_->size();
    data_->resize(start_pos + size_needed);
    VarInt::Set(value,
                std::span<uint8_t>(*data_).subspan(start_pos, size_needed));
  }

  std::expected<void, Error> WriteMax(size_t index, uint8_t value) {
    if (index >= data_->size()) {
      return std::unexpected(Error{
          .code = ErrorCode::kInvalidState,
          .message = std::format("Index {} out of bounds for buffer length {}",
                                 index, data_->size())});
    }
    (*data_)[index] = std::max((*data_)[index], value);
    return {};
  }

  [[nodiscard]] size_t capacity() const { return data_->capacity(); }
  [[nodiscard]] size_t size() const { return data_->size(); }
  [[nodiscard]] bool empty() const { return data_->empty(); }

  // Exposes the underlying buffer for adoption or copy.
  std::vector<uint8_t> Consume() { return std::move(*data_); }
  [[nodiscard]] const std::vector<uint8_t>& GetBuffer() const { return *data_; }

 private:
  // If we don't borrow a buffer, we use our own local storage.
  std::vector<uint8_t> local_buffer_;
  std::vector<uint8_t>* data_ = &local_buffer_;
};

}  // namespace zetasketch::utils

#endif  // ZETASKETCH_UTILS_BUFFER_TRAITS_H_
