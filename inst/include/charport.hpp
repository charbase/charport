#ifndef CHARPORT_HPP
#define CHARPORT_HPP

// charport -- interconnect for ALTREP character vector backends.
//
// C++-only public header (C++11-compatible; no pinned R CXX standard). Layout:
//
//   1. the raw ABI: element type (charport/strview.h), reader, registration
//      and resolve signatures, and the charvec store wrap;
//   2. namespace cp: header-only consumer wrappers (cp::Reader,
//      cp::charvec::Builder / cp::charvec::BuilderShard) that reach charport
//      via R_GetCCallable.
//
// Compatibility model: consumers compile against these headers (LinkingTo)
// and recompile when charport's headers change -- the R source-package
// norm. The builder types compile *into* the consumer, so an installed
// consumer binary must match the loaded charport's vintage; a layout or
// contract change bumps CHARPORT_ABI_VERSION, and cp::check_abi() in the
// consumer's load hook turns a stale binary into a clean "recompile" error
// instead of corruption. The R_GetCCallable symbol set itself stays
// append-only (a breaking signature change ships a new symbol name, e.g.
// charport_resolve_v2). Nothing is frozen until the first CRAN release.

#define R_NO_REMAP
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <R_ext/Rdynload.h>

#include <cstddef>
#include <deque>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

#include "charport/strview.h"
// the charvec store engine; reached through cp::charvec below, layout
// guarded by CHARPORT_ABI_VERSION rather than promised stable
#include "charport_internal/charvec_store.h"

#define CHARPORT_ABI_VERSION 2

extern "C" {

// ========================================================================
// Reader ABI
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
struct charport_reader {
    R_xlen_t n;                    // Rf_xlength(x), cached at resolve
    void * state;                  // opaque; meaningful only to get
    charport_get_strview_fn get;   // always non-null after resolve
    bool reentrant;                // see contract; direct path is always true
};

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
// immutable materialized storage. Note the direct path can surface
// CE_NATIVE / CE_LATIN1 from old CHARSXPs; registered backends should emit
// only CE_ASCII / CE_UTF8 / CE_ASCII_OR_UTF8 / CE_BYTES / NA.
//
// charport_charvec_wrap takes ownership of a finished
// charport::internal::charvec_data (allocated with new, headers matching
// the loaded charport per CHARPORT_ABI_VERSION -- all R packages on a
// platform share one toolchain and heap, so cross-package new/delete is
// sound) and returns it wrapped as a charvec SEXP. It validates the
// emission policy over the records (Rf_error on violation, after freeing
// the store) so a charvec can never enter R holding records the ABI
// forbids. Main R thread only; cp::charvec::Builder::finish is the
// intended caller.

typedef void (*charport_register_backend_t)(R_altrep_class_t cls,
                                            charport_init_fn init,
                                            charport_get_strview_fn get_strview,
                                            bool reentrant);
typedef void (*charport_unregister_backend_t)(R_altrep_class_t cls);
typedef charport_reader (*charport_resolve_t)(SEXP x);
typedef int (*charport_abi_version_t)(void);
typedef SEXP (*charport_charvec_wrap_t)(void * store);

} // extern "C"

// ========================================================================
// namespace cp: header-only consumer wrappers
// ========================================================================

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

namespace charvec {

// Write handle for one shard of a Builder: a copyable, non-owning handle,
// dead after finish() / Builder destruction. set() copies the bytes into
// the shard's slices (ptr only needs to live for the call), enforces the
// emission policy (CE_ASCII / CE_UTF8 / CE_ASCII_OR_UTF8 / CE_BYTES;
// translate latin1/native to UTF-8 first) and the NA convention
// (ptr == NULL is NA and must come as {NULL, 0, CE_NA}), and throws
// std::runtime_error on any failure -- it touches no R API, so it is safe
// on worker threads under a framework that propagates exceptions to the
// join point (RcppParallel does; with raw std::thread, catch in the
// worker).
//
// Threading: distinct shards may set() concurrently from any threads as
// long as no two shards write the same index; contiguous disjoint ranges
// are the intended pattern. Unwritten elements remain NA.
class BuilderShard {
public:
  BuilderShard() noexcept = default;

  void set(R_xlen_t i, const char * ptr, size_t len, charport_enc enc) const {
    if(ptr == nullptr || enc == charport_enc::CE_NA) {
      if(ptr != nullptr || len != 0) {
        throw std::runtime_error("charvec builder: NA must be {NULL, 0, CE_NA}");
      }
      s_->assign(static_cast<size_t>(i), nullptr, 0, charport_enc::CE_NA);
      return;
    }
    switch(enc) {  // backend emission policy, enforced at the write boundary
    case charport_enc::CE_ASCII:
    case charport_enc::CE_UTF8:
    case charport_enc::CE_ASCII_OR_UTF8:
    case charport_enc::CE_BYTES:
      break;
    default:
      throw std::runtime_error(
        "charvec builder: encoding must be CE_ASCII, CE_UTF8, CE_ASCII_OR_UTF8, or "
        "CE_BYTES (translate latin1/native to UTF-8 first)");
    }
    if(i < 0) {
      throw std::runtime_error("charvec builder: negative index");
    }
    s_->assign(static_cast<size_t>(i), ptr, len, enc);  // throws on OOB / len > INT_MAX
  }
  void set(R_xlen_t i, const StrView & v) const {
    set(i, v.ptr, static_cast<size_t>(v.len), v.enc);
  }
  void set_na(R_xlen_t i) const {
    set(i, nullptr, 0, charport_enc::CE_NA);
  }

private:
  friend class Builder;
  explicit BuilderShard(charport::internal::charvec_shard * s) noexcept : s_(s) {}
  charport::internal::charvec_shard * s_ = nullptr;
};

// Append-style construction of a charvec, never creating CHARSXPs. Serial
// use: Builder b(n); b.set(...); SEXP out = b.finish(). Parallel use:
// request one BuilderShard per thread (on the main thread, or externally
// synchronized), let each worker set() its own disjoint index range, then
// finish() on the main thread -- the merge block-moves shard slices, no
// payload copy. The store is built in the consumer's own compiled code;
// only finish() crosses into charport (charport_charvec_wrap), which takes
// ownership and re-validates the emission policy.
class Builder {
public:
  explicit Builder(R_xlen_t n) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    records_ = charport::internal::make_unique<std::vector<charport_strview>>(
      static_cast<size_t>(n), charport::internal::na_record());
  }

  Builder(const Builder &) = delete;
  Builder & operator=(const Builder &) = delete;
  Builder(Builder &&) noexcept = default;
  Builder & operator=(Builder &&) noexcept = default;

  // main R thread (or externally synchronized); handles stay valid until
  // finish() / destruction (deque: slots never move)
  BuilderShard shard() {
    if(records_ == nullptr) {
      throw std::runtime_error("charvec builder: already finished");
    }
    shards_.emplace_back(*records_);
    return BuilderShard(&shards_.back());
  }

  // serial convenience over an implicit shard
  void set(R_xlen_t i, const char * ptr, size_t len, charport_enc enc) {
    serial_shard().set(i, ptr, len, enc);
  }
  void set(R_xlen_t i, const StrView & v) {
    serial_shard().set(i, v);
  }
  void set_na(R_xlen_t i) {
    serial_shard().set_na(i);
  }

  // merge the shards and wrap the store in a charvec SEXP; single-shot,
  // invalidates every shard handle. Main R thread.
  SEXP finish() {
    if(records_ == nullptr) {
      throw std::runtime_error("charvec builder: already finished");
    }
    std::vector<charport::internal::charvec_shard> merged;
    merged.reserve(shards_.size());
    for(charport::internal::charvec_shard & s : shards_) {
      merged.push_back(std::move(s));
    }
    auto store = charport::internal::make_unique<charport::internal::charvec_data>(
      std::move(*records_), merged);
    records_.reset();
    shards_.clear();
    serial_ = BuilderShard();
    static charport_charvec_wrap_t wrap =
      reinterpret_cast<charport_charvec_wrap_t>(detail::fetch("charport_charvec_wrap"));
    return wrap(store.release());  // charport owns the store from here
  }

private:
  BuilderShard & serial_shard() {
    if(serial_.s_ == nullptr) {
      serial_ = shard();
    }
    return serial_;
  }

  // records_ lives behind a stable address (shards point into it), keeping
  // the Builder movable
  std::unique_ptr<std::vector<charport_strview>> records_;
  std::deque<charport::internal::charvec_shard> shards_;
  BuilderShard serial_;
};

} // namespace charvec
} // namespace cp

#endif
