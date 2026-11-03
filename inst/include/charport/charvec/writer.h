#ifndef CHARPORT_CHARVEC_WRITER_H
#define CHARPORT_CHARVEC_WRITER_H

// In-place charvec fill: allocate a charvec of NA strings first, then write
// its strings later. For producers that must create the R object before its
// strings are available, such as deserializers that read an object tree
// before its data. Include charport.h from packages.

#include "builder.h"

#include <cstring>
#include <stdexcept>
#include <type_traits>

#if defined(_MSVC_LANG)
#  if _MSVC_LANG >= 201703L
#    define CHARPORT_WRITER_NODISCARD [[nodiscard]]
#  else
#    define CHARPORT_WRITER_NODISCARD
#  endif
#elif __cplusplus >= 201703L
#  define CHARPORT_WRITER_NODISCARD [[nodiscard]]
#else
#  define CHARPORT_WRITER_NODISCARD
#endif

namespace charport {
namespace charvec {

// A charvec of n NA strings. Follows R error semantics.
inline SEXP alloc_na(R_xlen_t n) {
  return charport_charvec_alloc_na(n);
}

// The Store of a charvec that is not materialized and has no cached elements.
// Returns nullptr otherwise, including for any vector that is not a charvec.
// Does not call R in a way that can raise an error.
//
// The check does not prove the vector is unread: a Reader leaves no trace.
// Use it only on a vector the caller owns, such as one just returned by
// alloc_na(), that no R code or Reader has seen. Nothing else may read the
// vector while it is filled, and every record must point into this Store's
// slices (Writer does this). The pointer is valid until the vector is read
// from R, materialized or garbage collected.
inline attribute_hidden Store * get_store(SEXP x) noexcept {
  static charport_charvec_get_store_t fn = nullptr;
  if(fn == nullptr) {
    fn = reinterpret_cast<charport_charvec_get_store_t>(
      charport::detail::fetch("charport_charvec_get_store_v1")
    );
  }
  return static_cast<Store *>(fn(x));
}

// Writes strings into a Store from get_store(), in any order. A slice belongs
// to the Store as soon as it is allocated, and a record changes only in set(),
// after its bytes are written, so an R error or C++ exception partway leaves
// a valid vector. Trivially destructible, so a longjmp past it leaks nothing.
class Writer {
public:
  explicit Writer(Store & store) noexcept
    : store_(store),
      initial_(builder_detail::initial_slice_heuristic(store.size())) {}

  // A copy would share the slice cursor, so two Writers would hand out the
  // same bytes.
  Writer(const Writer &) = delete;
  Writer & operator=(const Writer &) = delete;

  // Space for len bytes, to be filled by the caller and then passed to set().
  CHARPORT_WRITER_NODISCARD
  char * reserve(size_t len) {
    if(len == 0) {
      return const_cast<char *>(components::empty_data());
    }
    components::checked_string_size(len, "stored string length");
    if(len > left_) {
      const size_t regular = std::min(
        internal::round_up(std::max(initial_, used_ / 2), builder_detail::slice_alignment()),
        builder_detail::max_slice_bytes());
      const size_t bytes = internal::round_up(
        std::max(regular, len), builder_detail::slice_alignment());
      slice_ = store_.slices.push_front(bytes);
      left_ = bytes;
    }
    char * dest = slice_;
    slice_ += len;
    left_ -= len;
    used_ += len;
    return dest;
  }

  // Point record i at len bytes from reserve().
  void set(size_t i, const char * ptr, size_t len, cetype_ext_t enc) {
    check_index(i);
    if(ptr == nullptr || enc == CETYPE_EXT_NA) {
      store_.records.set_na(i);
      return;
    }
    const int stored_len = components::checked_string_size(len, "stored string length");
    store_.records.set(i, stored_len == 0 ? components::empty_data() : ptr, stored_len, enc);
  }

  void set_na(size_t i) {
    check_index(i);
    store_.records.set_na(i);
  }

  void set_empty(size_t i, cetype_ext_t enc) {
    set(i, components::empty_data(), 0, enc);
  }

  // reserve(), copy and set() in one call.
  void copy(size_t i, const char * src, size_t len, cetype_ext_t enc) {
    check_index(i);
    if(src == nullptr || enc == CETYPE_EXT_NA) {
      store_.records.set_na(i);
      return;
    }
    char * dest = reserve(len);
    if(len > 0) {
      std::memcpy(dest, src, len);
    }
    set(i, dest, len, enc);
  }

private:
  void check_index(size_t i) const {
    if(i >= store_.records.size()) {
      throw std::out_of_range("charvec Writer: index out of bounds");
    }
  }

  Store & store_;
  const size_t initial_;
  char * slice_ = nullptr;
  size_t left_ = 0;
  size_t used_ = 0;
};

static_assert(std::is_trivially_destructible<Writer>::value,
              "charvec Writer must stay trivially destructible");
static_assert(!std::is_copy_constructible<Writer>::value,
              "charvec Writer must not be copyable");

} // namespace charvec
} // namespace charport

#undef CHARPORT_WRITER_NODISCARD

#endif
