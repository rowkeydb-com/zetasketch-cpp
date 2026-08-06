// Implementation of Geoff Pike's fingerprint2011 hash.
// Ported to C++ for Zetasketch compatibility with Java.

// NOLINTBEGIN
#include "fingerprint2011.h"
#include <algorithm>
#include <cstring>

namespace zetasketch {

namespace {

const uint64_t K0 = 0xa5b85c5e198ed849ULL;
const uint64_t K1 = 0x8d58ac26afe12e47ULL;
const uint64_t K2 = 0xc47b6e9e3a970ed3ULL;
const uint64_t K3 = 0xc6a4a7935bd1e995ULL;

inline uint64_t Load64(const char* p) {
  uint64_t result;
  std::memcpy(&result, p, sizeof(result));
  return result;
}

inline uint64_t Load64Safely(const char* p, size_t len) {
  uint64_t result = 0;
  size_t limit = std::min<size_t>(len, 8);
  for (size_t i = 0; i < limit; ++i) {
    result |= (static_cast<uint64_t>(static_cast<unsigned char>(p[i])))
              << (i * 8);
  }
  return result;
}

inline uint64_t RotateRight(uint64_t val, int shift) {
  return (shift == 0) ? val : ((val >> shift) | (val << (64 - shift)));
}

inline uint64_t ShiftMix(uint64_t val) { return val ^ (val >> 47); }

inline uint64_t Hash128To64(uint64_t high, uint64_t low) {
  uint64_t a = (low ^ high) * K3;
  a ^= (a >> 47);
  uint64_t b = (high ^ a) * K3;
  b ^= (b >> 47);
  b *= K3;
  return b;
}

struct Uint128 {
  uint64_t first;
  uint64_t second;
};

inline Uint128 WeakHashLength32WithSeed(const char* bytes, uint64_t seed_a,
                                        uint64_t seed_b) {
  uint64_t part1 = Load64(bytes);
  uint64_t part2 = Load64(bytes + 8);
  uint64_t part3 = Load64(bytes + 16);
  uint64_t part4 = Load64(bytes + 24);

  seed_a += part1;
  seed_b = RotateRight(seed_b + seed_a + part4, 51);
  uint64_t c = seed_a;
  seed_a += part2;
  seed_a += part3;
  seed_b += RotateRight(seed_a, 23);
  return {seed_a + part4, seed_b + c};
}

uint64_t MurmurHash64WithSeed(const char* bytes, size_t length, uint64_t seed) {
  const uint64_t mul = K3;
  const size_t top_bit = 0x7;
  size_t length_aligned = length & ~top_bit;
  size_t length_reminder = length & top_bit;
  uint64_t hash = seed ^ (static_cast<uint64_t>(length) * mul);

  for (size_t i = 0; i < length_aligned; i += 8) {
    uint64_t loaded = Load64(bytes + i);
    uint64_t data = ShiftMix(loaded * mul) * mul;
    hash ^= data;
    hash *= mul;
  }

  if (length_reminder != 0) {
    uint64_t data = Load64Safely(bytes + length_aligned, length_reminder);
    hash ^= data;
    hash *= mul;
  }
  hash = ShiftMix(hash) * mul;
  hash = ShiftMix(hash);
  return hash;
}

uint64_t HashLength33To64(const char* bytes, size_t length) {
  uint64_t z = Load64(bytes + 24);
  uint64_t a =
      Load64(bytes) +
      (static_cast<uint64_t>(length) + Load64(bytes + length - 16)) * K0;
  uint64_t b = RotateRight(a + z, 52);
  uint64_t c = RotateRight(a, 37);
  a += Load64(bytes + 8);
  c += RotateRight(a, 7);
  a += Load64(bytes + 16);
  uint64_t vf = a + z;
  uint64_t vs = b + RotateRight(a, 31) + c;
  a = Load64(bytes + 16) + Load64(bytes + length - 32);
  z = Load64(bytes + length - 8);
  b = RotateRight(a + z, 52);
  c = RotateRight(a, 37);
  a += Load64(bytes + length - 24);
  c += RotateRight(a, 7);
  a += Load64(bytes + length - 16);
  uint64_t wf = a + z;
  uint64_t ws = b + RotateRight(a, 31) + c;
  uint64_t r = ShiftMix((vf + ws) * K2 + (wf + vs) * K0);
  return ShiftMix(r * K0 + vs) * K2;
}

uint64_t FullFingerprint(const char* bytes, size_t length_total) {
  uint64_t x = Load64(bytes);
  uint64_t y = Load64(bytes + length_total - 16) ^ K1;
  uint64_t z = Load64(bytes + length_total - 56) ^ K0;
  Uint128 v = WeakHashLength32WithSeed(bytes + length_total - 64,
                                       static_cast<uint64_t>(length_total), y);
  Uint128 w = WeakHashLength32WithSeed(
      bytes + length_total - 32, static_cast<uint64_t>(length_total) * K1, K0);
  z += ShiftMix(v.second) * K1;
  x = RotateRight(z + x, 39) * K1;
  y = RotateRight(y, 33) * K1;

  size_t offset = 0;
  size_t length = (length_total - 1) & ~static_cast<size_t>(63);
  do {
    x = RotateRight(x + y + v.first + Load64(bytes + offset + 16), 37) * K1;
    y = RotateRight(y + v.second + Load64(bytes + offset + 48), 42) * K1;
    x ^= w.second;
    y ^= v.first;
    z = RotateRight(z ^ w.first, 33);
    v = WeakHashLength32WithSeed(bytes + offset, v.second * K1, x + w.first);
    w = WeakHashLength32WithSeed(bytes + offset + 32, z + w.second, y);
    std::swap(z, x);
    offset += 64;
    length -= 64;
  } while (length != 0);

  return Hash128To64(Hash128To64(v.first, w.first) + ShiftMix(y) * K1 + z,
                     Hash128To64(v.second, w.second) + x);
}

}  // namespace

uint64_t Fingerprint2011(const char* bytes, size_t length) {
  uint64_t result;
  if (length <= 32) {
    result = MurmurHash64WithSeed(bytes, length, K0 ^ K1 ^ K2);
  } else if (length <= 64) {
    result = HashLength33To64(bytes, length);
  } else {
    result = FullFingerprint(bytes, length);
  }

  uint64_t u = (length >= 8) ? Load64(bytes) : K0;
  uint64_t v = (length >= 9) ? Load64(bytes + length - 8) : K0;
  result = Hash128To64(result + v, u);
  if (result == 0 || result == 1) {
    return result + ~static_cast<uint64_t>(1);
  }
  return result;
}

}  // namespace zetasketch
// NOLINTEND
