#ifndef CHARPORT_BASE_H
#define CHARPORT_BASE_H

// charport base: the reader / registration ABI plus the header-only consumer
// helpers. Aggregated (with the two builder headers) by <charport.h>.
//
//   * The raw ABI -- element type, charport_reader, the function-pointer and
//     R_GetCCallable symbol typedefs -- is plain C in an extern "C" block, so
//     a C package can resolve a vector and loop over it.
//   * The cp:: helpers (resolve / register_backend / check_abi / Reader) and
//     the shared builder emission-policy check are C++ only; they sit behind
//     #ifdef __cplusplus.
//
// Compatibility model: consumers compile against these headers (LinkingTo)
// and recompile when charport's headers change -- the R source-package norm.
// A layout or contract change bumps CHARPORT_ABI_VERSION, and cp::check_abi()
// in the consumer's load hook turns a stale binary into a clean "recompile"
// error instead of corruption. The R_GetCCallable symbol set itself stays
// append-only (a breaking signature change ships a new symbol name, e.g.
// charport_resolve_v2). Nothing is frozen until the first CRAN release.

#define R_NO_REMAP
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <R_ext/Rdynload.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "strview.h"

#define CHARPORT_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

// ========================================================================
// Reader ABI (C and C++)
// ========================================================================

// Per-element accessor: a pure function of backend state + index, valid for
// i in [0, n). No SEXP, no R API -- when registered reentrant = true it may
// be called concurrently from any thread (see the reentrant contract below).
typedef charport_strview (*charport_get_strview_fn)(void * state, R_xlen_t i);

// Per-vector state lookup: called once by charport_resolve, on the main R
// thread. Returns the backend's instance state -- a borrow, typically of
// what already hangs off R_altrep_data1(x); the broker never frees it. A
// backend that must build state lazily (an index, a cache) builds it here,
// on the main thread, and stashes it in R_altrep_data1/data2 so GC owns the
// lifetime. May Rf_error. Returning NULL means "cannot serve this vector"
// (e.g. the backend's store has been released after materialization): the
// broker then falls back to materialize + direct, same as an unregistered
// class.
typedef void * (*charport_init_fn)(SEXP x);

// The per-vector handle: an SEXP-free by-value POD -- nothing to close,
// nothing to free, safe to hand to worker threads when reentrant. The
// reader is a borrow of x: while any copy of it (or any returned strview)
// is in use, the consumer keeps x protected and does not touch x through
// the R API -- no writes, no DATAPTR/STRING_PTR_RO, and no element access
// either (an ALTREP class may materialize eagerly on STRING_ELT, and
// mutation or materialization may move or free the viewed storage). state
// and every returned strview ptr are valid exactly that long.
typedef struct charport_reader {
    R_xlen_t n;                    // Rf_xlength(x), cached at resolve
    void * state;                  // opaque; meaningful only to get
    charport_get_strview_fn get;   // always non-null after resolve
    bool reentrant;                // see contract; direct path is always true
} charport_reader;

// ------------------------------------------------------------------------
// R_GetCCallable symbols ("charport", <name>):
//
//   charport_register_backend    charport_register_backend_t
//   charport_unregister_backend  charport_unregister_backend_t
//   charport_resolve             charport_resolve_t
//   charport_abi_version         charport_abi_version_t
//   charport_charvec_wrap        charport_charvec_wrap_t
//
// Registration is for backends (call from R_init_<pkg> / .onLoad; call
// unregister from .onUnload -- a stale entry holds function pointers into
// an unloaded DSO). Re-registering an already-registered class replaces its
// entry. Registration and resolve are main-R-thread-only; the registry is
// not locked.
//
// reentrant = true is a promise about get_strview: it performs no R
// allocation and cannot trigger GC, mutates no backend state, may be called
// concurrently from any thread, and the returned ptr remains valid for as
// long as x is alive and unmutated. false means: main R thread only,
// serial, and the consumer must consume/copy the bytes before the next
// get_strview call on the same vector.
//
// charport_resolve accepts STRSXP only (errors otherwise) and performs the
// single per-vector lookup: a registered, serveable backend yields its
// {state, get, reentrant}; everything else (plain vectors, materialized or
// unregistered ALTREP, init returned NULL) is served by the broker's own
// CHARSXP accessor over STRING_PTR_RO(x) -- forcing one-time
// materialization for unregistered ALTREP, the status-quo cost consumers
// pay today. After resolve, reading never touches R: the direct accessor's
// CHARSXP derivation (R_CHAR / getCharCE / charIsASCII) is plain reads of
// immutable materialized storage. A strview carries its own encoding
// (CE_ASCII / CE_UTF8 / CE_LATIN1 / CE_NATIVE / CE_BYTES / NA); the store
// keeps whatever it was given and the direct path reports the CHARSXP's mark
// as-is. Any encoding policy a consumer wants is the consumer's to apply.
//
// charport_charvec_wrap takes ownership of a finished
// charport::internal::charvec_data (allocated with new, headers matching
// the loaded charport per CHARPORT_ABI_VERSION -- all R packages on a
// platform share one toolchain and heap, so cross-package new/delete is
// sound) and returns it wrapped as a charvec SEXP, verbatim -- the store was
// built by the consumer's own code, so there is no policy to enforce (records
// are kept as given). Main R thread only; cp::charvec::Builder::finish (single
// threaded) or cp::charvec::BuilderMT::finish is the intended caller.

typedef void (*charport_register_backend_t)(R_altrep_class_t cls,
                                            charport_init_fn init,
                                            charport_get_strview_fn get_strview,
                                            bool reentrant);
typedef void (*charport_unregister_backend_t)(R_altrep_class_t cls);
typedef charport_reader (*charport_resolve_t)(SEXP x);
typedef int (*charport_abi_version_t)(void);
typedef SEXP (*charport_charvec_wrap_t)(void * store);

#ifdef __cplusplus
} // extern "C"
#endif

// ========================================================================
// namespace cp: header-only consumer helpers (C++ only)
// ========================================================================
#ifdef __cplusplus

#include <iterator>
#include <stdexcept>

namespace cp {

using StrView = charport_strview;

namespace detail {

// R_GetCCallable Rf_errors if the symbol is missing, so a successful fetch
// is always usable. Function-local statics cache per consumer DSO; every
// fetch happens on the main R thread (Reader construction, finish,
// registration) -- worker threads never touch R.
inline DL_FUNC fetch(const char * name) {
  return R_GetCCallable("charport", name);
}

} // namespace detail

// Raw ABI calls (main R thread only).
inline charport_reader resolve(SEXP x) {
  static charport_resolve_t fn =
    reinterpret_cast<charport_resolve_t>(detail::fetch("charport_resolve"));
  return fn(x);
}

inline void register_backend(R_altrep_class_t cls, charport_init_fn init,
                             charport_get_strview_fn get_strview, bool reentrant) {
  static charport_register_backend_t fn =
    reinterpret_cast<charport_register_backend_t>(detail::fetch("charport_register_backend"));
  fn(cls, init, get_strview, reentrant);
}

inline void unregister_backend(R_altrep_class_t cls) {
  static charport_unregister_backend_t fn =
    reinterpret_cast<charport_unregister_backend_t>(detail::fetch("charport_unregister_backend"));
  fn(cls);
}

inline int abi_version() {
  static charport_abi_version_t fn =
    reinterpret_cast<charport_abi_version_t>(detail::fetch("charport_abi_version"));
  return fn();
}

// Rf_error (call from .onLoad / R_init) if the loaded charport speaks a
// different ABI generation than this header was compiled against -- the
// guard that makes the recompile-on-update model safe for installed
// binaries.
inline void check_abi() {
  const int loaded = abi_version();
  if(loaded != CHARPORT_ABI_VERSION) {
    Rf_error("charport ABI mismatch: this package was compiled against charport ABI %d "
             "but the installed charport provides ABI %d; please reinstall this package",
             CHARPORT_ABI_VERSION, loaded);
  }
}

// One loop over any character vector. Construct on the main R thread (the
// resolve); copies of the underlying POD (raw()) may be handed to worker
// threads when reentrant(). The reader is a borrow: keep x protected and
// do not touch it through the R API while any copy is in use (see the
// charport_reader contract above).
class Reader {
public:
  explicit Reader(SEXP x) : r_(resolve(x)) {}
  explicit Reader(const charport_reader & r) noexcept : r_(r) {}  // adopt (e.g. on a worker)

  R_xlen_t size() const noexcept { return r_.n; }
  bool reentrant() const noexcept { return r_.reentrant; }
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
    bool operator==(const const_iterator & o) const noexcept { return i_ == o.i_; }
    bool operator!=(const const_iterator & o) const noexcept { return i_ != o.i_; }

  private:
    const charport_reader * r_;
    R_xlen_t i_;
  };

  const_iterator begin() const noexcept { return const_iterator(&r_, 0); }
  const_iterator end() const noexcept { return const_iterator(&r_, r_.n); }

private:
  charport_reader r_;
};

} // namespace cp

#endif // __cplusplus

#endif
