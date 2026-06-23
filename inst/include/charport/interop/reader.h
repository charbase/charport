#ifndef CHARPORT_INTEROP_READER_H
#define CHARPORT_INTEROP_READER_H

// Reader and ALTREP registration ABI. Include charport.h from packages.

#define R_NO_REMAP
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <R_ext/Rdynload.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "types.h"

#define CHARPORT_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef charport_strview (*charport_get_strview_fn)(void * state, R_xlen_t i);
typedef void * (*charport_init_fn)(SEXP x);

#ifdef __cplusplus
enum class charport_reader_kind : uint8_t {
    PLAIN              = 0,
    MATERIALIZED_ALTREP = 1,
    REGISTERED_ALTREP = 2,
    FALLBACK_ALTREP   = 3
};
#else
typedef uint8_t charport_reader_kind;
enum {
    CHARPORT_READER_PLAIN              = 0,
    CHARPORT_READER_MATERIALIZED_ALTREP = 1,
    CHARPORT_READER_REGISTERED_ALTREP = 2,
    CHARPORT_READER_FALLBACK_ALTREP   = 3
};
#endif

typedef struct charport_reader {
    R_xlen_t n;
    void * state;
    charport_get_strview_fn get;
    bool view_persistence;
    bool thread_safe_access;
    charport_reader_kind kind;
} charport_reader;

typedef void (*charport_register_altrep_t)(R_altrep_class_t cls,
                                           charport_init_fn init,
                                           charport_get_strview_fn get_strview,
                                           bool view_persistence,
                                           bool thread_safe_access);
typedef void (*charport_unregister_altrep_t)(R_altrep_class_t cls);
typedef charport_reader (*charport_resolve_t)(SEXP x);
typedef int (*charport_abi_version_t)(void);
typedef SEXP (*charport_charvec_wrap_t)(void * store);

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus

#include <iterator>

namespace charport {

using StrView = charport_strview;
using ReaderKind = charport_reader_kind;

namespace detail {

inline DL_FUNC fetch(const char * name) {
  return R_GetCCallable("charport", name);
}

} // namespace detail

inline charport_reader resolve(SEXP x) {
  static charport_resolve_t fn =
    reinterpret_cast<charport_resolve_t>(detail::fetch("charport_resolve"));
  return fn(x);
}

inline void register_altrep(R_altrep_class_t cls, charport_init_fn init,
                            charport_get_strview_fn get_strview,
                            bool view_persistence,
                            bool thread_safe_access) {
  static charport_register_altrep_t fn =
    reinterpret_cast<charport_register_altrep_t>(detail::fetch("charport_register_altrep"));
  fn(cls, init, get_strview, view_persistence, thread_safe_access);
}

inline void unregister_altrep(R_altrep_class_t cls) {
  static charport_unregister_altrep_t fn =
    reinterpret_cast<charport_unregister_altrep_t>(detail::fetch("charport_unregister_altrep"));
  fn(cls);
}

inline int abi_version() {
  static charport_abi_version_t fn =
    reinterpret_cast<charport_abi_version_t>(detail::fetch("charport_abi_version"));
  return fn();
}

inline void check_abi() {
  const int loaded = abi_version();
  if(loaded != CHARPORT_ABI_VERSION) {
    Rf_error("charport ABI mismatch: this package was compiled against charport ABI %d "
             "but the installed charport provides ABI %d; please reinstall this package",
             CHARPORT_ABI_VERSION, loaded);
  }
}

class Reader {
public:
  explicit Reader(SEXP x) : r_(resolve(x)) {}
  explicit Reader(const charport_reader & r) noexcept : r_(r) {}

  R_xlen_t size() const noexcept { return r_.n; }
  bool view_persistence() const noexcept { return r_.view_persistence; }
  bool thread_safe_access() const noexcept { return r_.thread_safe_access; }
  bool reentrant() const noexcept { return view_persistence() && thread_safe_access(); }
  ReaderKind kind() const noexcept { return r_.kind; }
  StrView operator[](R_xlen_t i) const { return r_.get(r_.state, i); }
  const charport_reader & raw() const noexcept { return r_; }

  class const_iterator {
  public:
    using value_type = StrView;
    using reference = StrView;
    using pointer = void;
    using difference_type = R_xlen_t;
    using iterator_category = std::input_iterator_tag;

    const_iterator(const charport_reader * r, R_xlen_t i) noexcept : r_(r), i_(i) {}
    StrView operator*() const { return r_->get(r_->state, i_); }
    const_iterator & operator++() noexcept { ++i_; return *this; }
    const_iterator operator++(int) noexcept { const_iterator tmp = *this; ++i_; return tmp; }
    bool operator==(const const_iterator & other) const noexcept { return i_ == other.i_; }
    bool operator!=(const const_iterator & other) const noexcept { return i_ != other.i_; }

  private:
    const charport_reader * r_;
    R_xlen_t i_;
  };

  const_iterator begin() const noexcept { return const_iterator(&r_, 0); }
  const_iterator end() const noexcept { return const_iterator(&r_, r_.n); }

private:
  charport_reader r_;
};

} // namespace charport

#endif // __cplusplus

#endif
