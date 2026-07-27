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

#ifdef __cplusplus
#include "unwind.h"
#endif

#define CHARPORT_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef void * (*charport_reader_init_fn)(SEXP x);
typedef void (*charport_reader_release_fn)(void * state);
typedef void (*charport_reader_strviews_range_fn)(void * state,
                                                  R_xlen_t start,
                                                  R_xlen_t size,
                                                  const char ** out_ptrs,
                                                  int * out_lens,
                                                  cetype_ext_t * out_encs);
typedef void (*charport_reader_strviews_index_fn)(void * state,
                                                  const R_xlen_t * indices,
                                                  R_xlen_t size,
                                                  const char ** out_ptrs,
                                                  int * out_lens,
                                                  cetype_ext_t * out_encs);
typedef void (*charport_reader_byteviews_range_fn)(void * state,
                                                   R_xlen_t start,
                                                   R_xlen_t size,
                                                   const char ** out_ptrs,
                                                   int * out_lens);
typedef void (*charport_reader_byteviews_index_fn)(void * state,
                                                   const R_xlen_t * indices,
                                                   R_xlen_t size,
                                                   const char ** out_ptrs,
                                                   int * out_lens);
typedef void (*charport_reader_lengths_range_fn)(void * state,
                                                 R_xlen_t start,
                                                 R_xlen_t size,
                                                 int * out_lens);
typedef void (*charport_reader_lengths_index_fn)(void * state,
                                                 const R_xlen_t * indices,
                                                 R_xlen_t size,
                                                 int * out_lens);
typedef void (*charport_reader_encodings_range_fn)(void * state,
                                                   R_xlen_t start,
                                                   R_xlen_t size,
                                                   cetype_ext_t * out_encs);
typedef void (*charport_reader_encodings_index_fn)(void * state,
                                                   const R_xlen_t * indices,
                                                   R_xlen_t size,
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
typedef SEXP (*charport_charvec_wrap_t)(void * store);

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus

#include <iterator>
#include <stdexcept>
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

} // namespace detail

CHARPORT_READER_NODISCARD
inline charport_reader resolve(SEXP x) {
  static charport_resolve_t fn = nullptr;
  if(fn == nullptr) {
    fn = reinterpret_cast<charport_resolve_t>(detail::fetch("charport_resolve"));
  }
  return fn(x);
}

namespace detail {

template<typename Backend>
inline charport_reader resolve_with(SEXP x) {
  charport_reader out{};
  Backend::call([&]() -> SEXP {
    out = resolve(x);
    return R_NilValue;
  });
  return out;
}

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

template<typename Backend>
class BasicReader {
public:
  explicit BasicReader(SEXP x) : r_(detail::resolve_with<Backend>(x)) {}
  explicit BasicReader(charport_reader r) noexcept : r_(r) {}
  ~BasicReader() noexcept { release_owned(); }

  BasicReader(const BasicReader &) = delete;
  BasicReader & operator=(const BasicReader &) = delete;

  BasicReader(BasicReader && other) noexcept : r_(other.r_) { other.disown(); }
  BasicReader & operator=(BasicReader && other) noexcept {
    if(this != &other) {
      release_owned();
      r_ = other.r_;
      other.disown();
    }
    return *this;
  }

  R_xlen_t size() const noexcept { return r_.n; }
  bool persistent_views() const noexcept { return r_.capabilities.persistent_views; }
  bool concurrent_access() const noexcept { return r_.capabilities.concurrent_access; }
  bool reentrant() const noexcept { return persistent_views() && concurrent_access(); }

  ByteView byteview(R_xlen_t i) const {
    const char * ptr = nullptr;
    int len = NA_INTEGER;
    r_.range.byteviews(r_.state, i, 1, &ptr, &len);
    return make_byteview(ptr, len);
  }
  int length(R_xlen_t i) const {
    int len = NA_INTEGER;
    r_.range.lengths(r_.state, i, 1, &len);
    return len;
  }
  cetype_ext_t encoding(R_xlen_t i) const {
    cetype_ext_t enc = cetype_ext_t::CE_NA;
    r_.range.encodings(r_.state, i, 1, &enc);
    return enc;
  }
  StrView view(R_xlen_t i) const {
    const char * ptr = nullptr;
    int len = NA_INTEGER;
    cetype_ext_t enc = cetype_ext_t::CE_NA;
    r_.range.strviews(r_.state, i, 1, &ptr, &len, &enc);
    return make_strview(ptr, len, enc);
  }
  StrView operator[](R_xlen_t i) const { return view(i); }

  void views(R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
             int * out_lens, cetype_ext_t * out_encs) const {
    r_.range.strviews(r_.state, start, size, out_ptrs, out_lens, out_encs);
  }
  // The int-start overloads exist because a literal 0 start would otherwise
  // be ambiguous between R_xlen_t and the indexed const R_xlen_t * overloads.
  void views(int start, R_xlen_t size, const char ** out_ptrs,
             int * out_lens, cetype_ext_t * out_encs) const {
    views(static_cast<R_xlen_t>(start), size, out_ptrs, out_lens, out_encs);
  }
  void views(R_xlen_t start, R_xlen_t size, StrViews & out) const {
    out.resize(size);
    views(start, size, out.ptrs(), out.lengths(), out.encodings());
  }
  void views(int start, R_xlen_t size, StrViews & out) const {
    views(static_cast<R_xlen_t>(start), size, out);
  }

  void byteviews(R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
                 int * out_lens) const {
    r_.range.byteviews(r_.state, start, size, out_ptrs, out_lens);
  }
  void byteviews(int start, R_xlen_t size, const char ** out_ptrs,
                 int * out_lens) const {
    byteviews(static_cast<R_xlen_t>(start), size, out_ptrs, out_lens);
  }
  void byteviews(R_xlen_t start, R_xlen_t size, ByteViews & out) const {
    out.resize(size);
    byteviews(start, size, out.ptrs(), out.lengths());
  }
  void byteviews(int start, R_xlen_t size, ByteViews & out) const {
    byteviews(static_cast<R_xlen_t>(start), size, out);
  }

  void lengths(R_xlen_t start, R_xlen_t size, int * out) const {
    r_.range.lengths(r_.state, start, size, out);
  }
  void lengths(int start, R_xlen_t size, int * out) const {
    lengths(static_cast<R_xlen_t>(start), size, out);
  }
  void encodings(R_xlen_t start, R_xlen_t size, cetype_ext_t * out) const {
    r_.range.encodings(r_.state, start, size, out);
  }
  void encodings(int start, R_xlen_t size, cetype_ext_t * out) const {
    encodings(static_cast<R_xlen_t>(start), size, out);
  }

  void views(const R_xlen_t * indices, R_xlen_t size, const char ** out_ptrs,
             int * out_lens, cetype_ext_t * out_encs) const {
    r_.index.strviews(r_.state, indices, size, out_ptrs, out_lens, out_encs);
  }
  void views(const R_xlen_t * indices, R_xlen_t size, StrViews & out) const {
    out.resize(size);
    views(indices, size, out.ptrs(), out.lengths(), out.encodings());
  }

  void byteviews(const R_xlen_t * indices, R_xlen_t size, const char ** out_ptrs,
                 int * out_lens) const {
    r_.index.byteviews(r_.state, indices, size, out_ptrs, out_lens);
  }
  void byteviews(const R_xlen_t * indices, R_xlen_t size, ByteViews & out) const {
    out.resize(size);
    byteviews(indices, size, out.ptrs(), out.lengths());
  }
  void lengths(const R_xlen_t * indices, R_xlen_t size, int * out) const {
    r_.index.lengths(r_.state, indices, size, out);
  }
  void encodings(const R_xlen_t * indices, R_xlen_t size, cetype_ext_t * out) const {
    r_.index.encodings(r_.state, indices, size, out);
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
      cetype_ext_t enc = cetype_ext_t::CE_NA;
      r_->range.strviews(r_->state, i_, 1, &ptr, &len, &enc);
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

} // namespace charport

#undef CHARPORT_READER_NODISCARD

#endif // __cplusplus

#endif
