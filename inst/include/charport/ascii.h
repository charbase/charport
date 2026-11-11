#ifndef CHARPORT_ASCII_H
#define CHARPORT_ASCII_H

// R-free; usable from any thread.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace charport {

// True when no byte has its high bit set. One function for every platform:
// 16-byte GCC/Clang vectors, which are SSE2 on x86-64 and NEON on arm64.
//
// Bytes are OR-ed 64 at a time with one test per block, so there is no branch
// per byte and non-ASCII text returns within one block of its first non-ASCII
// byte. The remainder is OR-ed 16 bytes at a time, ending with the last 16
// bytes of the string, which may overlap bytes already covered. Strings of 8
// to 15 bytes are two overlapping words. Each of these was measured; so was a
// run-time AVX2 path, which gained only above 1 KiB and was dropped.
inline bool is_ascii(const char * data, size_t len) noexcept {
  typedef uint64_t vec __attribute__((vector_size(16)));
  const uint64_t high = UINT64_C(0x8080808080808080);
  const unsigned char * p = reinterpret_cast<const unsigned char *>(data);

  if(len < 16) {
    if(len >= 8) {
      uint64_t first, last;
      std::memcpy(&first, p, 8);
      std::memcpy(&last, p + len - 8, 8);
      return ((first | last) & high) == 0;
    }
    unsigned char acc = 0;
    for(size_t i = 0; i < len; ++i) {
      acc = static_cast<unsigned char>(acc | p[i]);
    }
    return (acc & 0x80U) == 0;
  }

  size_t i = 0;
  for(; len - i >= 64; i += 64) {
    vec a, b, c, d;
    std::memcpy(&a, p + i, 16);
    std::memcpy(&b, p + i + 16, 16);
    std::memcpy(&c, p + i + 32, 16);
    std::memcpy(&d, p + i + 48, 16);
    const vec block = (a | b) | (c | d);
    if(((block[0] | block[1]) & high) != 0) return false;
  }

  vec acc;
  std::memcpy(&acc, p + len - 16, 16);
  for(; len - i >= 16; i += 16) {
    vec v;
    std::memcpy(&v, p + i, 16);
    acc |= v;
  }
  return ((acc[0] | acc[1]) & high) == 0;
}

} // namespace charport

#endif
