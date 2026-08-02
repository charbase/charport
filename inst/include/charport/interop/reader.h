#ifndef CHARPORT_INTEROP_READER_H
#define CHARPORT_INTEROP_READER_H

// Reader and ALTREP registration ABI. Include charport.h from packages.

#ifndef R_NO_REMAP
#define R_NO_REMAP
#endif
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
typedef void (*charport_reader_release_fn)(void * state);
/*
 * Access callbacks must not call R or let a C++ exception cross this ABI.
 *
 * Bounds are a caller precondition. Let n be charport_reader.n. Range calls
 * require 0 <= start <= n and 0 <= size <= n - start. Indexed calls require
 * size >= 0 and 0 <= indices[j] < n for every j. An empty range may start at
 * n. These conditions avoid evaluating start + size, which could overflow.
 *
 * Providers may validate these bounds and return
 * CHARPORT_STATUS_OUT_OF_RANGE. The providers shipped with charport do not
 * validate them, so an invalid request to those providers has undefined
 * behavior.
 *
 * Return zero on success. Output arrays are unspecified after a nonzero
 * return.
 */
typedef int (*charport_reader_strviews_range_fn)(
    void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
    int * out_lens, cetype_ext_t * out_encs);
typedef int (*charport_reader_strviews_index_fn)(
    void * state, const R_xlen_t * indices, R_xlen_t size,
    const char ** out_ptrs, int * out_lens, cetype_ext_t * out_encs);
typedef int (*charport_reader_byteviews_range_fn)(
    void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
    int * out_lens);
typedef int (*charport_reader_byteviews_index_fn)(
    void * state, const R_xlen_t * indices, R_xlen_t size,
    const char ** out_ptrs, int * out_lens);
typedef int (*charport_reader_lengths_range_fn)(
    void * state, R_xlen_t start, R_xlen_t size, int * out_lens);
typedef int (*charport_reader_lengths_index_fn)(
    void * state, const R_xlen_t * indices, R_xlen_t size, int * out_lens);
typedef int (*charport_reader_encodings_range_fn)(
    void * state, R_xlen_t start, R_xlen_t size, cetype_ext_t * out_encs);
typedef int (*charport_reader_encodings_index_fn)(
    void * state, const R_xlen_t * indices, R_xlen_t size,
    cetype_ext_t * out_encs);

typedef struct charport_reader_state_fns {
    charport_reader_init_fn init;
    charport_reader_release_fn release;
} charport_reader_state_fns;

typedef struct charport_reader_range_fns {
    charport_reader_strviews_range_fn strviews;
    charport_reader_byteviews_range_fn byteviews;
    charport_reader_lengths_range_fn lengths;
    charport_reader_encodings_range_fn encodings;
} charport_reader_range_fns;

typedef struct charport_reader_index_fns {
    charport_reader_strviews_index_fn strviews;
    charport_reader_byteviews_index_fn byteviews;
    charport_reader_lengths_index_fn lengths;
    charport_reader_encodings_index_fn encodings;
} charport_reader_index_fns;

typedef struct charport_reader_capabilities {
    bool persistent_views;
    bool concurrent_access;
} charport_reader_capabilities;

typedef struct charport_reader {
    R_xlen_t n;
    void * state;
    charport_reader_release_fn release;
    charport_reader_range_fns range;
    charport_reader_index_fns index;
    charport_reader_capabilities capabilities;
} charport_reader;

typedef struct charport_sexp_info {
    bool is_strsxp;
    R_xlen_t length;
    bool is_altrep;
    bool is_materialized;
    bool is_registered;
    bool persistent_views;
    bool concurrent_access;
    bool stateful_reader;
    const char * altrep_class_name;
    const char * altrep_class_package;

#ifdef __cplusplus
    inline bool reentrant() const noexcept {
      return persistent_views && concurrent_access;
    }
#endif
} charport_sexp_info;

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
                                           charport_reader_state_fns state_fns,
                                           charport_reader_range_fns range_fns,
                                           charport_reader_index_fns index_fns,
                                           charport_reader_capabilities capabilities);
typedef void (*charport_unregister_altrep_t)(R_altrep_class_t cls);
typedef charport_reader (*charport_resolve_t)(SEXP x);
typedef charport_sexp_info (*charport_sexp_info_t)(SEXP x);
typedef int (*charport_abi_version_t)(void);

/*
 * Store conversion hook used by the C++ charvec API. It moves from a
 * caller-owned Store into a new Store owned by charport. It may raise an R
 * error and does not throw a C++ exception.
 */
typedef SEXP (*charport_charvec_wrap_t)(void * store);
typedef SEXP (*charport_charvec_from_views_t)(
    R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings);

#ifndef __cplusplus
static inline charport_reader charport_resolve(SEXP x) {
  static charport_resolve_t fn = NULL;
  if(fn == NULL) {
    fn = (charport_resolve_t) R_GetCCallable("charport", "charport_resolve");
  }
  return fn(x);
}

static inline charport_sexp_info charport_get_sexp_info(SEXP x) {
  static charport_sexp_info_t fn = NULL;
  if(fn == NULL) {
    fn = (charport_sexp_info_t)
      R_GetCCallable("charport", "charport_sexp_info");
  }
  return fn(x);
}

static inline int charport_abi_version(void) {
  static charport_abi_version_t fn = NULL;
  if(fn == NULL) {
    fn = (charport_abi_version_t)
      R_GetCCallable("charport", "charport_abi_version");
  }
  return fn();
}
#endif

static inline SEXP charport_charvec_from_views(
    R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings) {
  static charport_charvec_from_views_t fn = NULL;
  if(fn == NULL) {
#ifdef __cplusplus
    fn = reinterpret_cast<charport_charvec_from_views_t>(
      R_GetCCallable("charport", "charport_charvec_from_views")
    );
#else
    fn = (charport_charvec_from_views_t)
      R_GetCCallable("charport", "charport_charvec_from_views");
#endif
  }
  return fn(n, ptrs, lengths, encodings);
}

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus

#include <cstdio>
#include <exception>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_MSVC_LANG)
#  if _MSVC_LANG >= 201703L
#    define CHARPORT_READER_NODISCARD [[nodiscard]]
#  else
#    define CHARPORT_READER_NODISCARD
#  endif
#elif __cplusplus >= 201703L
#  define CHARPORT_READER_NODISCARD [[nodiscard]]
#else
#  define CHARPORT_READER_NODISCARD
#endif

namespace charport {

using StrView = charport_strview;
using ByteView = charport_byteview;
using SexpInfo = charport_sexp_info;

namespace detail {

inline DL_FUNC fetch(const char * name) {
  return R_GetCCallable("charport", name);
}

inline int loaded_abi_version() {
  static charport_abi_version_t fn = nullptr;
  if(fn == nullptr) {
    fn = reinterpret_cast<charport_abi_version_t>(fetch("charport_abi_version"));
  }
  return fn();
}

[[noreturn]] inline void throw_access_error(int status) {
  if(status == CHARPORT_STATUS_NO_MEMORY) {
    throw std::bad_alloc();
  }
  if(status == CHARPORT_STATUS_OUT_OF_RANGE) {
    throw std::out_of_range("charport Reader access is out of range");
  }
  throw std::runtime_error("charport Reader access failed");
}

inline void check_access(int status) {
  if(status != CHARPORT_STATUS_OK) {
    throw_access_error(status);
  }
}

} // namespace detail

// Translate the active exception from inside a catch handler. A call with no
// active exception terminates the process.
inline int convert_current_exception_to_status() noexcept {
  try {
    throw;
  } catch(const std::bad_alloc &) {
    return CHARPORT_STATUS_NO_MEMORY;
  } catch(const std::out_of_range &) {
    return CHARPORT_STATUS_OUT_OF_RANGE;
  } catch(...) {
    return CHARPORT_STATUS_ERROR;
  }
}

CHARPORT_READER_NODISCARD
inline charport_reader resolve(SEXP x) {
  static charport_resolve_t fn = nullptr;
  if(fn == nullptr) {
    fn = reinterpret_cast<charport_resolve_t>(detail::fetch("charport_resolve"));
  }
  return fn(x);
}

namespace detail {

#if defined(RCPP_VERSION) || defined(CHARPORT_CPP11_INCLUDED)
// Flatten the active exception into a fixed buffer, naming its type.
//
// The obvious carrier here is std::exception_ptr, but it cannot be used. Its
// refcount helpers are exported only by libc++abi, while
// __cxa_allocate_exception is exported by libstdc++ too. When a package built
// against libc++ is loaded into an R built against libstdc++ -- the
// configuration of r-hub's clang containers -- the two disagree about the
// __cxa_exception header and freeing the exception corrupts the heap. Copying
// the text out holds no exception object across R's C frames. Nothing is lost:
// the failure only ever becomes an R condition, which carries no C++ type.
inline void describe_current_exception(char * out, size_t size) noexcept {
  try {
    throw;
  } catch(const std::bad_alloc & error) {
    std::snprintf(out, size, "std::bad_alloc: %s", error.what());
  } catch(const std::out_of_range & error) {
    std::snprintf(out, size, "std::out_of_range: %s", error.what());
  } catch(const std::logic_error & error) {
    std::snprintf(out, size, "std::logic_error: %s", error.what());
  } catch(const std::runtime_error & error) {
    std::snprintf(out, size, "std::runtime_error: %s", error.what());
  } catch(const std::exception & error) {
    std::snprintf(out, size, "std::exception: %s", error.what());
  } catch(...) {
    std::snprintf(out, size, "unknown C++ exception");
  }
}

template<typename Fn>
struct framework_call_state {
  Fn * fn;
  bool failed;
  char message[512];
};

template<typename Fn>
inline SEXP framework_call_body(void * data) noexcept {
  framework_call_state<Fn> * state =
    static_cast<framework_call_state<Fn> *>(data);
  try {
    return (*state->fn)();
  } catch(...) {
    // Framework unwind callbacks cross R's C frames before returning to C++,
    // so describe the failure here and raise it again once we are back.
    state->failed = true;
    describe_current_exception(state->message, sizeof(state->message));
    return R_NilValue;
  }
}

template<typename Fn>
inline void finish_framework_call(framework_call_state<Fn> & state) {
  if(state.failed) {
    throw std::runtime_error(state.message);
  }
}
#endif

#if defined(RCPP_VERSION)
template<typename Fn>
inline SEXP call_with_rcpp(Fn && fn) {
  typedef typename std::remove_reference<Fn>::type fun_type;
  framework_call_state<fun_type> state{&fn, false, {}};
  SEXP out = Rcpp::unwindProtect(&framework_call_body<fun_type>, &state);
  finish_framework_call(state);
  return out;
}

inline charport_reader resolve_with_rcpp(SEXP x) {
  charport_reader out{};
  call_with_rcpp([&]() -> SEXP {
    out = resolve(x);
    return R_NilValue;
  });
  return out;
}
#endif

#if defined(CHARPORT_CPP11_INCLUDED)
template<typename Fn>
inline SEXP call_with_cpp11(Fn && fn) {
  typedef typename std::remove_reference<Fn>::type fun_type;
  framework_call_state<fun_type> state{&fn, false, {}};
  SEXP out = cpp11::unwind_protect([&]() -> SEXP {
    return framework_call_body<fun_type>(&state);
  });
  finish_framework_call(state);
  return out;
}

inline charport_reader resolve_with_cpp11(SEXP x) {
  charport_reader out{};
  call_with_cpp11([&]() -> SEXP {
    out = resolve(x);
    return R_NilValue;
  });
  return out;
}
#endif

} // namespace detail

inline void register_altrep(R_altrep_class_t cls,
                            charport_reader_state_fns state_fns,
                            charport_reader_range_fns range_fns,
                            charport_reader_index_fns index_fns,
                            charport_reader_capabilities capabilities) {
  static charport_register_altrep_t fn = nullptr;
  if(fn == nullptr) {
    fn = reinterpret_cast<charport_register_altrep_t>(detail::fetch("charport_register_altrep"));
  }
  fn(cls, state_fns, range_fns, index_fns, capabilities);
}

inline void unregister_altrep(R_altrep_class_t cls) {
  static charport_unregister_altrep_t fn = nullptr;
  if(fn == nullptr) {
    fn = reinterpret_cast<charport_unregister_altrep_t>(detail::fetch("charport_unregister_altrep"));
  }
  fn(cls);
}

inline bool check_abi() {
  return detail::loaded_abi_version() == CHARPORT_ABI_VERSION;
}

inline SexpInfo sexp_info(SEXP x) {
  static charport_sexp_info_t fn = nullptr;
  if(fn == nullptr) {
    fn = reinterpret_cast<charport_sexp_info_t>(detail::fetch("charport_sexp_info"));
  }
  return fn(x);
}

class ByteViews {
public:
  explicit ByteViews(R_xlen_t size = 0) { resize(size); }

  void resize(R_xlen_t size) {
    if(size < 0) {
      throw std::runtime_error("charport::ByteViews: negative size");
    }
    ptrs_.resize(static_cast<size_t>(size));
    lens_.resize(static_cast<size_t>(size));
  }

  R_xlen_t size() const noexcept {
    return static_cast<R_xlen_t>(ptrs_.size());
  }

  const char ** ptrs() noexcept { return ptrs_.data(); }
  int * lengths() noexcept { return lens_.data(); }
  const char * const * ptrs() const noexcept { return ptrs_.data(); }
  const int * lengths() const noexcept { return lens_.data(); }

  ByteView operator[](R_xlen_t i) const noexcept {
    return make_byteview(ptrs_[static_cast<size_t>(i)],
                         lens_[static_cast<size_t>(i)]);
  }

private:
  std::vector<const char *> ptrs_;
  std::vector<int> lens_;
};

class StrViews {
public:
  explicit StrViews(R_xlen_t size = 0) { resize(size); }

  void resize(R_xlen_t size) {
    if(size < 0) {
      throw std::runtime_error("charport::StrViews: negative size");
    }
    ptrs_.resize(static_cast<size_t>(size));
    lens_.resize(static_cast<size_t>(size));
    encs_.resize(static_cast<size_t>(size));
  }

  R_xlen_t size() const noexcept {
    return static_cast<R_xlen_t>(ptrs_.size());
  }

  const char ** ptrs() noexcept { return ptrs_.data(); }
  int * lengths() noexcept { return lens_.data(); }
  cetype_ext_t * encodings() noexcept { return encs_.data(); }
  const char * const * ptrs() const noexcept { return ptrs_.data(); }
  const int * lengths() const noexcept { return lens_.data(); }
  const cetype_ext_t * encodings() const noexcept { return encs_.data(); }

  StrView operator[](R_xlen_t i) const noexcept {
    const size_t j = static_cast<size_t>(i);
    return make_strview(ptrs_[j], lens_[j], encs_[j]);
  }

private:
  std::vector<const char *> ptrs_;
  std::vector<int> lens_;
  std::vector<cetype_ext_t> encs_;
};

/*
 * Accessors have the same bounds preconditions as the C callbacks above.
 * Reader forwards requests without checking them, then translates any
 * nonzero status reported by the provider to a C++ exception.
 */
class Reader {
public:
  Reader() noexcept : r_{} {}

  // Ordinary construction follows R error semantics during resolution.
  explicit Reader(SEXP x) : r_(resolve(x)) {}
  explicit Reader(charport_reader r) noexcept : r_(r) {}
  ~Reader() noexcept { release_owned(); }

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

  // Resolve before replacing the current borrow. An R error leaves the
  // current borrow unchanged.
  void reset(SEXP x) {
    Reader next(x);
    *this = std::move(next);
  }

#if defined(RCPP_VERSION)
  static Reader with_rcpp(SEXP x);
#endif
#if defined(CHARPORT_CPP11_INCLUDED)
  static Reader with_cpp11(SEXP x);
#endif

  R_xlen_t size() const noexcept { return r_.n; }
  bool persistent_views() const noexcept { return r_.capabilities.persistent_views; }
  bool concurrent_access() const noexcept { return r_.capabilities.concurrent_access; }
  bool reentrant() const noexcept { return persistent_views() && concurrent_access(); }

  ByteView byteview(R_xlen_t i) const {
    const char * ptr = nullptr;
    int len = NA_INTEGER;
    const int status = r_.range.byteviews(r_.state, i, 1, &ptr, &len);
    detail::check_access(status);
    return make_byteview(ptr, len);
  }
  int length(R_xlen_t i) const {
    int len = NA_INTEGER;
    const int status = r_.range.lengths(r_.state, i, 1, &len);
    detail::check_access(status);
    return len;
  }
  cetype_ext_t encoding(R_xlen_t i) const {
    cetype_ext_t enc = CETYPE_EXT_NA;
    const int status = r_.range.encodings(r_.state, i, 1, &enc);
    detail::check_access(status);
    return enc;
  }
  StrView view(R_xlen_t i) const {
    const char * ptr = nullptr;
    int len = NA_INTEGER;
    cetype_ext_t enc = CETYPE_EXT_NA;
    const int status =
      r_.range.strviews(r_.state, i, 1, &ptr, &len, &enc);
    detail::check_access(status);
    return make_strview(ptr, len, enc);
  }
  StrView operator[](R_xlen_t i) const { return view(i); }

  void views(R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
             int * out_lens, cetype_ext_t * out_encs) const {
    const int status = r_.range.strviews(
      r_.state, start, size, out_ptrs, out_lens, out_encs
    );
    detail::check_access(status);
  }
  void views(R_xlen_t start, R_xlen_t size, StrViews & out) const {
    out.resize(size);
    views(start, size, out.ptrs(), out.lengths(), out.encodings());
  }
#ifdef LONG_VECTOR_SUPPORT
  // These overloads disambiguate a literal 0 from the indexed pointer forms.
  // Without long-vector support, R_xlen_t is int and they would be duplicates.
  void views(int start, R_xlen_t size, const char ** out_ptrs,
             int * out_lens, cetype_ext_t * out_encs) const {
    views(static_cast<R_xlen_t>(start), size, out_ptrs, out_lens, out_encs);
  }
  void views(int start, R_xlen_t size, StrViews & out) const {
    views(static_cast<R_xlen_t>(start), size, out);
  }
#endif

  void byteviews(R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
                 int * out_lens) const {
    const int status =
      r_.range.byteviews(r_.state, start, size, out_ptrs, out_lens);
    detail::check_access(status);
  }
  void byteviews(R_xlen_t start, R_xlen_t size, ByteViews & out) const {
    out.resize(size);
    byteviews(start, size, out.ptrs(), out.lengths());
  }
#ifdef LONG_VECTOR_SUPPORT
  void byteviews(int start, R_xlen_t size, const char ** out_ptrs,
                 int * out_lens) const {
    byteviews(static_cast<R_xlen_t>(start), size, out_ptrs, out_lens);
  }
  void byteviews(int start, R_xlen_t size, ByteViews & out) const {
    byteviews(static_cast<R_xlen_t>(start), size, out);
  }
#endif

  void lengths(R_xlen_t start, R_xlen_t size, int * out) const {
    const int status = r_.range.lengths(r_.state, start, size, out);
    detail::check_access(status);
  }
  void encodings(R_xlen_t start, R_xlen_t size, cetype_ext_t * out) const {
    const int status = r_.range.encodings(r_.state, start, size, out);
    detail::check_access(status);
  }
#ifdef LONG_VECTOR_SUPPORT
  void lengths(int start, R_xlen_t size, int * out) const {
    lengths(static_cast<R_xlen_t>(start), size, out);
  }
  void encodings(int start, R_xlen_t size, cetype_ext_t * out) const {
    encodings(static_cast<R_xlen_t>(start), size, out);
  }
#endif

  void views(const R_xlen_t * indices, R_xlen_t size, const char ** out_ptrs,
             int * out_lens, cetype_ext_t * out_encs) const {
    const int status = r_.index.strviews(
      r_.state, indices, size, out_ptrs, out_lens, out_encs
    );
    detail::check_access(status);
  }
  void views(const R_xlen_t * indices, R_xlen_t size, StrViews & out) const {
    out.resize(size);
    views(indices, size, out.ptrs(), out.lengths(), out.encodings());
  }

  void byteviews(const R_xlen_t * indices, R_xlen_t size, const char ** out_ptrs,
                 int * out_lens) const {
    const int status =
      r_.index.byteviews(r_.state, indices, size, out_ptrs, out_lens);
    detail::check_access(status);
  }
  void byteviews(const R_xlen_t * indices, R_xlen_t size, ByteViews & out) const {
    out.resize(size);
    byteviews(indices, size, out.ptrs(), out.lengths());
  }
  void lengths(const R_xlen_t * indices, R_xlen_t size, int * out) const {
    const int status = r_.index.lengths(r_.state, indices, size, out);
    detail::check_access(status);
  }
  void encodings(const R_xlen_t * indices, R_xlen_t size, cetype_ext_t * out) const {
    const int status = r_.index.encodings(r_.state, indices, size, out);
    detail::check_access(status);
  }

  class const_iterator {
  public:
    using value_type = StrView;
    using reference = StrView;
    using pointer = void;
    using difference_type = R_xlen_t;
    using iterator_category = std::input_iterator_tag;

    const_iterator(const charport_reader * r, R_xlen_t i) noexcept : r_(r), i_(i) {}
    StrView operator*() const {
      const char * ptr = nullptr;
      int len = NA_INTEGER;
      cetype_ext_t enc = CETYPE_EXT_NA;
      const int status =
        r_->range.strviews(r_->state, i_, 1, &ptr, &len, &enc);
      detail::check_access(status);
      return make_strview(ptr, len, enc);
    }
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

#if defined(RCPP_VERSION)
inline Reader Reader::with_rcpp(SEXP x) {
  return Reader(detail::resolve_with_rcpp(x));
}
#endif

#if defined(CHARPORT_CPP11_INCLUDED)
inline Reader Reader::with_cpp11(SEXP x) {
  return Reader(detail::resolve_with_cpp11(x));
}
#endif

} // namespace charport

#undef CHARPORT_READER_NODISCARD

#endif // __cplusplus

#endif
