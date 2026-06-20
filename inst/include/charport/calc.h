#ifndef CHARPORT_CALC_H
#define CHARPORT_CALC_H

// R-free foundation for the charvec store: byte/size helpers on top of the
// public ABI element types (charport_enc, charport_strview, which live in
// charport/strview.h). Nothing in this header may include R headers; R
// interop lives in r_interop.h.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "strview.h"

namespace charport {
namespace internal {

#if defined (__AVX2__)
} // namespace internal
} // namespace charport
#include <immintrin.h>
namespace charport {
namespace internal {
inline bool check_ascii(const void * ptr, size_t len) noexcept {
  const uint8_t * p8 = reinterpret_cast<const uint8_t*>(ptr);
  size_t i = 0;
  if(len >= 32) {
    __m256i sum = _mm256_setzero_si256();
    for(; i + 32 <= len; i += 32) {
      __m256i load = _mm256_lddqu_si256(reinterpret_cast<const __m256i*>(p8 + i));
      sum = _mm256_or_si256(sum, load);
    }
    int msb = _mm256_movemask_epi8(sum);
    if(msb != 0) return false;
  }
  if(len >= i + 16) {
    __m128i load = _mm_lddqu_si128(reinterpret_cast<const __m128i*>(p8 + i));
    int msb = _mm_movemask_epi8(load);
    if(msb != 0) return false;
    i += 16;
  }
  for(; i < len; ++i) {
    if(p8[i] > 127) {
      return false;
    }
  }
  return true;
}
#else
inline bool check_ascii(const void * ptr, size_t len) noexcept {
  const uint8_t * p8 = reinterpret_cast<const uint8_t*>(ptr);
  for(size_t j = 0; j < len; j++) {
    if(p8[j] > 127) {
      return false;
    }
  }
  return true;
}
#endif

// CHARSXP byte length is capped at INT_MAX -- a base-R bound.
constexpr uint32_t r_string_size_max() noexcept {
  return static_cast<uint32_t>(std::numeric_limits<int>::max());
}

// Forward to std::make_unique when the standard provides it (C++14+); fall
// back to the equivalent hand-rolled version on the C++11 floor. Both wrap the
// new inside a function, so both give the same exception-safety as
// std::make_unique -- the forward is a conformance nicety, not a safety change.
#if defined(__cpp_lib_make_unique) || (defined(__cplusplus) && __cplusplus >= 201402L)
using std::make_unique;
#else
template<typename T, typename... Args>
inline std::unique_ptr<T> make_unique(Args&&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
#endif

template<typename POD>
inline bool check_r_string_len_impl(const POD value, std::true_type) noexcept {
  return value >= 0 && static_cast<uint64_t>(value) <= static_cast<uint64_t>(r_string_size_max());
}

template<typename POD>
inline bool check_r_string_len_impl(const POD value, std::false_type) noexcept {
  return static_cast<uint64_t>(value) <= static_cast<uint64_t>(r_string_size_max());
}

template<typename POD>
inline bool check_r_string_len(const POD value) noexcept {
  return check_r_string_len_impl(value, typename std::is_signed<POD>::type());
}

inline uint32_t checked_u32(size_t size, const char * what = "size") {
  if(size > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    throw std::runtime_error(std::string(what) + " exceeds uint32_t storage");
  }
  return static_cast<uint32_t>(size);
}

inline size_t checked_add_size(size_t lhs, size_t rhs, const char * what) {
  if(lhs > std::numeric_limits<size_t>::max() - rhs) {
    throw std::runtime_error(std::string(what) + " overflow");
  }
  return lhs + rhs;
}

inline size_t checked_mul_size(size_t lhs, size_t rhs, const char * what) {
  if(lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
    throw std::runtime_error(std::string(what) + " overflow");
  }
  return lhs * rhs;
}

inline size_t round_up(size_t value, size_t multiple) noexcept {
  if(value == 0 || multiple == 0) {
    return value;
  }
  size_t rem = value % multiple;
  return rem == 0 ? value : (value + (multiple - rem));
}

inline size_t next_power_of_two(size_t value) noexcept {
  if(value <= 1) {
    return 1;
  }
  size_t out = 1;
  while(out < value && out <= (std::numeric_limits<size_t>::max() >> 1)) {
    out <<= 1;
  }
  return out < value ? value : out;
}

} // namespace internal
} // namespace charport

#endif
