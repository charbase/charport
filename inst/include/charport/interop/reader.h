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

typedef void * (*charport_reader_init_fn)(SEXP x);
typedef charport_strview (*charport_reader_get_fn)(void * state, R_xlen_t i);
typedef void (*charport_reader_release_fn)(void * state);

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
    charport_reader_get_fn get;
    charport_reader_release_fn release;
    bool view_persistence;
    bool thread_safe_access;
    charport_reader_kind kind;
} charport_reader;

/* End a raw C reader borrow. C++ users should prefer charport::Reader. */
static inline void charport_reader_release(charport_reader * r) {
    if(r != NULL && r->release != NULL) {
        r->release(r->state);
    }
    if(r != NULL) {
        r->state = NULL;
        r->release = NULL;
    }
}

typedef void (*charport_register_altrep_t)(R_altrep_class_t cls,
                                           charport_reader_init_fn reader_init,
                                           charport_reader_get_fn reader_get,
                                           charport_reader_release_fn reader_release,
                                           bool view_persistence,
                                           bool thread_safe_access);
typedef void (*charport_unregister_altrep_t)(R_altrep_class_t cls);
typedef charport_reader (*charport_resolve_t)(SEXP x);
typedef charport_strview (*charport_read_scalar_t)(SEXP x);
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

inline int loaded_abi_version() {
  static charport_abi_version_t fn =
    reinterpret_cast<charport_abi_version_t>(fetch("charport_abi_version"));
  return fn();
}

} // namespace detail

inline charport_reader resolve(SEXP x) {
  static charport_resolve_t fn =
    reinterpret_cast<charport_resolve_t>(detail::fetch("charport_resolve"));
  return fn(x);
}

inline void register_altrep(R_altrep_class_t cls,
                            charport_reader_init_fn reader_init,
                            charport_reader_get_fn reader_get,
                            charport_reader_release_fn reader_release,
                            bool view_persistence,
                            bool thread_safe_access) {
  static charport_register_altrep_t fn =
    reinterpret_cast<charport_register_altrep_t>(detail::fetch("charport_register_altrep"));
  fn(cls, reader_init, reader_get, reader_release, view_persistence, thread_safe_access);
}

inline void unregister_altrep(R_altrep_class_t cls) {
  static charport_unregister_altrep_t fn =
    reinterpret_cast<charport_unregister_altrep_t>(detail::fetch("charport_unregister_altrep"));
  fn(cls);
}

inline bool check_abi() {
  return detail::loaded_abi_version() == CHARPORT_ABI_VERSION;
}

class Reader {
public:
  static StrView read_scalar(SEXP x) {
    static charport_read_scalar_t fn =
      reinterpret_cast<charport_read_scalar_t>(detail::fetch("charport_read_scalar"));
    return fn(x);
  }

  explicit Reader(SEXP x) : r_(resolve(x)) {}
  explicit Reader(charport_reader r) noexcept : r_(r) {}
  ~Reader() { release_owned(); }

  Reader(const Reader &) = delete;
  Reader & operator=(const Reader &) = delete;

  Reader(Reader && other) noexcept : r_(other.r_) { other.disown(); }
  Reader & operator=(Reader && other) noexcept {
    if(this != &other) {
      release_owned();
      r_ = other.r_;
      other.disown();
    }
    return *this;
  }

  R_xlen_t size() const noexcept { return r_.n; }
  bool view_persistence() const noexcept { return r_.view_persistence; }
  bool thread_safe_access() const noexcept { return r_.thread_safe_access; }
  bool reentrant() const noexcept { return view_persistence() && thread_safe_access(); }
  ReaderKind kind() const noexcept { return r_.kind; }
  StrView operator[](R_xlen_t i) const { return r_.get(r_.state, i); }

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
  void release_owned() noexcept {
    if(r_.release != nullptr) {
      r_.release(r_.state);
    }
  }

  void disown() noexcept {
    r_.state = nullptr;
    r_.release = nullptr;
  }

  charport_reader r_;
};

} // namespace charport

#endif // __cplusplus

#endif
