// SPDX-FileCopyrightText: 2026 RowKeyDB
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ZETASKETCH_HLL_SPARSE_BUFFER_H_
#define ZETASKETCH_HLL_SPARSE_BUFFER_H_

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace zetasketch::hll {

// The values a sparse representation has taken in but not yet written
// into its difference-encoded stream.
//
// The reference keeps these in a hash set, so a value taken in twice is
// held once and the count that decides when the buffer is flushed is
// the count of distinct values. That count is observable: a flush moves
// values from the buffer into the stream, and a downgrade re-encodes
// the stream for the lower precision but carries the buffer across as
// it is, so which values are still buffered when a lower-precision
// operand arrives decides the bytes written afterwards. A list that
// counted repeats flushed early and left different values in the
// buffer, and the sketch written after such a merge differed from the
// reference's.
//
// The set is open addressing with linear probing over a table whose
// size is a power of two. It holds no storage until the first value,
// doubles whenever a value would take it past three quarters full, and
// is never shrunk; a flush clears it in place. The values are already
// uniform hashes in their upper bits but not in their lower ones, where
// a rho value sits, so a slot is chosen by mixing the value first.
//
// Every slot outside the values held is the empty marker, in the table
// and in the sorted array alike, so a reader may take the whole of
// slots_ as the set whenever sorted_ is false, and its first size_
// entries as the values whenever sorted_ is true.
class SparseBuffer {
 public:
  SparseBuffer() = default;
  SparseBuffer(const SparseBuffer&) = delete;
  SparseBuffer& operator=(const SparseBuffer&) = delete;
  ~SparseBuffer() = default;

  // A buffer moved from is empty and usable. A representation that a
  // refused operation was moved out of keeps its buffer, and the next
  // operation on it must find the count and the storage in agreement,
  // so the storage is emptied rather than left as a move leaves it.
  SparseBuffer(SparseBuffer&& other) noexcept
      : slots_(std::move(other.slots_)),
        size_(std::exchange(other.size_, 0)),
        sorted_(std::exchange(other.sorted_, false)),
        holds_empty_marker_(std::exchange(other.holds_empty_marker_, false)) {
    other.slots_.clear();
  }
  SparseBuffer& operator=(SparseBuffer&& other) noexcept {
    if (this != &other) {
      slots_ = std::move(other.slots_);
      other.slots_.clear();
      size_ = std::exchange(other.size_, 0);
      sorted_ = std::exchange(other.sorted_, false);
      holds_empty_marker_ = std::exchange(other.holds_empty_marker_, false);
    }
    return *this;
  }

  // Takes a value in. Returns whether the buffer did not hold it yet.
  bool Insert(uint32_t value) {
    if (value == kEmptySlot) {
      const bool inserted = !holds_empty_marker_;
      holds_empty_marker_ = true;
      return inserted;
    }
    if (sorted_) {
      Rehash(std::bit_ceil(std::max(slots_.size(), kInitialSlots)));
    }
    if ((size_ + 1) * kGrowthDenominator > slots_.size() * kGrowthNumerator) {
      Rehash(slots_.empty() ? kInitialSlots : slots_.size() * 2);
    }
    size_t slot = SlotOf(value);
    while (slots_[slot] != kEmptySlot) {
      if (slots_[slot] == value) return false;
      slot = (slot + 1) & (slots_.size() - 1);
    }
    slots_[slot] = value;
    ++size_;
    return true;
  }

  // The number of distinct values held.
  [[nodiscard]] size_t size() const {
    return size_ + (holds_empty_marker_ ? 1 : 0);
  }
  [[nodiscard]] bool empty() const { return size() == 0; }

  // Forgets every value and keeps the storage for the next ones.
  void clear() {
    std::ranges::fill(slots_, kEmptySlot);
    size_ = 0;
    sorted_ = false;
    holds_empty_marker_ = false;
  }

  // The values in increasing order, arranged in place at the front of
  // the storage, the rest of it emptied. The table is a set no longer
  // afterwards; the next Insert rebuilds it, and clear() discards it.
  [[nodiscard]] std::span<const uint32_t> Sorted() {
    if (!sorted_) {
      const auto vacated = std::ranges::remove(slots_, kEmptySlot);
      std::ranges::sort(slots_.begin(), vacated.begin());
      std::ranges::fill(vacated, kEmptySlot);
      sorted_ = true;
    }
    if (!holds_empty_marker_) {
      return std::span<const uint32_t>(slots_.data(), size_);
    }
    // The marker value is the largest possible, so it belongs last.
    if (slots_.size() <= size_) slots_.resize(size_ + 1, kEmptySlot);
    slots_[size_] = kEmptySlot;
    return std::span<const uint32_t>(slots_.data(), size_ + 1);
  }

  // Calls the visitor with every value held, in no particular order,
  // for as long as the visitor returns true.
  template <typename Visitor>
  void ForEach(const Visitor& visit) const {
    const size_t held = sorted_ ? size_ : slots_.size();
    for (size_t i = 0; i < held; ++i) {
      if (slots_[i] != kEmptySlot && !visit(slots_[i])) return;
    }
    if (holds_empty_marker_) visit(kEmptySlot);
  }

 private:
  // Marks a slot that holds no value. No encoding produces this value,
  // but a stream read from untrusted bytes can, so it is held apart.
  static constexpr uint32_t kEmptySlot = std::numeric_limits<uint32_t>::max();
  static constexpr size_t kInitialSlots = 16;
  static constexpr size_t kGrowthNumerator = 3;
  static constexpr size_t kGrowthDenominator = 4;
  // The finalizer of MurmurHash3, which spreads every input bit over
  // every output bit.
  static constexpr uint32_t kMixFirstMultiplier = 0x85EBCA6BU;
  static constexpr uint32_t kMixSecondMultiplier = 0xC2B2AE35U;
  static constexpr uint32_t kMixWideShift = 16;
  static constexpr uint32_t kMixNarrowShift = 13;

  [[nodiscard]] size_t SlotOf(uint32_t value) const {
    uint32_t mixed = value;
    mixed ^= mixed >> kMixWideShift;
    mixed *= kMixFirstMultiplier;
    mixed ^= mixed >> kMixNarrowShift;
    mixed *= kMixSecondMultiplier;
    mixed ^= mixed >> kMixWideShift;
    return mixed & (slots_.size() - 1);
  }

  // Moves the values held into a fresh table of the given size, which
  // must be a power of two large enough to hold them.
  void Rehash(size_t new_slots) {
    std::vector<uint32_t> old_slots = std::move(slots_);
    const size_t held = sorted_ ? size_ : old_slots.size();
    slots_.assign(new_slots, kEmptySlot);
    size_ = 0;
    sorted_ = false;
    for (size_t i = 0; i < held; ++i) {
      if (old_slots[i] == kEmptySlot) continue;
      size_t slot = SlotOf(old_slots[i]);
      while (slots_[slot] != kEmptySlot) {
        slot = (slot + 1) & (slots_.size() - 1);
      }
      slots_[slot] = old_slots[i];
      ++size_;
    }
  }

  std::vector<uint32_t> slots_;
  size_t size_ = 0;
  bool sorted_ = false;
  bool holds_empty_marker_ = false;
};

}  // namespace zetasketch::hll

#endif  // ZETASKETCH_HLL_SPARSE_BUFFER_H_
