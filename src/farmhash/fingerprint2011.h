// Implementation of Geoff Pike's fingerprint2011 hash.
// Ported to C++ for Zetasketch compatibility with Java.

#ifndef ZETASKETCH_FARMHASH_FINGERPRINT2011_H_
#define ZETASKETCH_FARMHASH_FINGERPRINT2011_H_

// NOLINTBEGIN

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zetasketch {

uint64_t Fingerprint2011(const char* bytes, size_t length);

inline uint64_t Fingerprint2011(std::string_view s) {
  return Fingerprint2011(s.data(), s.size());
}

}  // namespace zetasketch

// NOLINTEND
#endif  // ZETASKETCH_FARMHASH_FINGERPRINT2011_H_
