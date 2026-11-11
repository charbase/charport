#ifndef CHARPORT_CHARVEC_DETAIL_H
#define CHARPORT_CHARVEC_DETAIL_H

// R-free helpers used by charvec storage and builders.

#include "../interop/types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace charport {
namespace internal {

constexpr uint32_t r_string_size_max() noexcept {
  return static_cast<uint32_t>(std::numeric_limits<int>::max());
}

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
  const size_t rem = value % multiple;
  return rem == 0 ? value : value + (multiple - rem);
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
