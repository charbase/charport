#ifndef CHARPORT_BUILDER_SINGLE_THREADED_H
#define CHARPORT_BUILDER_SINGLE_THREADED_H

// cp::charvec::Builder -- serial construction of a charvec, never creating
// CHARSXPs. The Builder owns the records array and one charvec_shard (its
// thread-local bump chain); the write logic lives in the free helpers
// fill_record / copy_record, which the Builder feeds its own records array.
//
//   cp::charvec::Builder b(n);          // n elements, all start NA
//   b.set(0, "hello", 5, charport_enc::CE_UTF8);
//   b.set_na(1);
//   SEXP out = b.to_charvec();          // wraps the store as a charvec SEXP
//
// Two exits, both single-shot: to_charvec() wraps the store as an R charvec
// (crossing into charport via charport_charvec_wrap); release_store() hands back
// the raw charport::internal::charvec_data so charport's own code can wrap it
// directly. reset(n) re-arms a spent (or fresh) Builder; after an exit it is
// spent, so reset before reuse (writing without reset first is undefined, not
// checked, to keep the per-element path branch-free). For parallel construction
// use cp::charvec::BuilderMT.

#include "base.h"
#include "charvec_store.h"

#include <memory>

namespace cp {
namespace charvec {

class Builder {
public:
  explicit Builder(R_xlen_t n) {
    reset(n);
  }

  Builder(const Builder &) = delete;
  Builder & operator=(const Builder &) = delete;
  Builder(Builder &&) noexcept = default;       // records_ + shard_ are move-only RAII
  Builder & operator=(Builder &&) noexcept = default;

  void reset(R_xlen_t n) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    records_ = charport::internal::strview_array(static_cast<size_t>(n));
    shard_ = charport::internal::charvec_shard();  // frees any prior chain
    finished_ = false;
  }

  // Write element i. No encoding policy (that belongs a layer above): the store
  // keeps the encoding as given, and ptr == NULL is NA regardless. ptr need
  // only live for the call. Throws only on index out of range or len > INT_MAX.
  void set(R_xlen_t i, const char * ptr, size_t len, charport_enc enc) {
    if(ptr == nullptr || enc == charport_enc::CE_NA) {
      charport::internal::copy_record(shard_, records_.data(), records_.size(),
                                      static_cast<size_t>(i), nullptr, 0, charport_enc::CE_NA);
      return;
    }
    charport::internal::copy_record(shard_, records_.data(), records_.size(),
                                    static_cast<size_t>(i), ptr, len, enc);
  }
  void set(R_xlen_t i, const StrView & v) {
    set(i, v.ptr, static_cast<size_t>(v.len), v.enc);
  }
  void set_na(R_xlen_t i) {
    set(i, nullptr, 0, charport_enc::CE_NA);
  }

  // Zero-copy write: reserve len bytes for element i with encoding enc and point
  // the record at them, returning the buffer for the caller to fill with exactly
  // len bytes. Encoding kept as given; use set_na for NA. The pointer is valid
  // until an exit (slice blocks never move). Throws as set() does.
  char * reserve(R_xlen_t i, size_t len, charport_enc enc) {
    return charport::internal::fill_record(shard_, records_.data(), records_.size(),
                                           static_cast<size_t>(i), len, enc);
  }

  // Hand back the finished store (the shard chain is spliced in here);
  // single-shot. The intended caller is charport's own code, which Makes the
  // store directly; external consumers should use to_charvec().
  std::unique_ptr<charport::internal::charvec_data> release_store() {
    if(finished_) {
      throw std::runtime_error("charvec builder: already finished");
    }
    auto store = charport::internal::make_unique<charport::internal::charvec_data>(
      std::move(records_));
    store->adopt_chain(shard_);
    finished_ = true;
    return store;
  }

  // Wrap the finished store as a charvec SEXP; single-shot. Main R thread.
  SEXP to_charvec() {
    static charport_charvec_wrap_t wrap =
      reinterpret_cast<charport_charvec_wrap_t>(cp::detail::fetch("charport_charvec_wrap"));
    return wrap(release_store().release());  // charport owns the store from here
  }

  // Callback convenience and the in-package construction primitive: fill a store
  // of length n through one shard and return the raw store (no wrap). The fill
  // receives the shard, the records pointer, and the count, hoisted once, so its
  // loop calls fill_record / copy_record with no per-element reload. Out-of-range
  // or never-written indices stay NA. On a fill exception the shard's RAII frees
  // its partial chain; on success the store adopts it. Charport's own paths build
  // with one call and Make the result; same bump-packing as an incremental
  // Builder.
  template<typename Fill>
  static std::unique_ptr<charport::internal::charvec_data> build_store(R_xlen_t n, Fill && fill) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    charport::internal::strview_array records(static_cast<size_t>(n));
    charport::internal::charvec_shard shard;
    fill(shard, records.data(), records.size());
    auto store = charport::internal::make_unique<charport::internal::charvec_data>(
      std::move(records));
    store->adopt_chain(shard);
    return store;
  }

private:
  charport::internal::strview_array records_;
  charport::internal::charvec_shard shard_;
  bool finished_ = false;
};

} // namespace charvec
} // namespace cp

#endif
