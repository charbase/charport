#ifndef CHARPORT_BUILDER_SINGLE_THREADED_H
#define CHARPORT_BUILDER_SINGLE_THREADED_H

// cp::charvec::Builder -- serial construction of a charvec, never creating
// CHARSXPs. One vector, one thread, one bump-allocated slice chain. There is
// NO shard concept and no handle type here: you call set() directly on the
// Builder.
//
//   cp::charvec::Builder b(n);          // n elements, all start NA
//   b.set(0, "hello", 5, charport_enc::CE_UTF8);
//   b.set_na(1);
//   SEXP out = b.finish();              // wraps the store as a charvec SEXP
//
// finish() is single-shot. reset(n) re-arms a finished (or fresh) Builder for
// another vector, reusing the object across a tight loop. For parallel /
// multi-threaded construction use cp::charvec::BuilderMT
// (builder_multi_threaded.h) instead.
//
// The store is built in the consumer's own compiled code; only finish()
// crosses into charport (charport_charvec_wrap), which takes ownership and
// wraps the store verbatim (no encoding policy -- records are kept as given).

#include "base.h"
#include "../charport_internal/charvec_store.h"

#include <array>
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
  Builder(Builder &&) noexcept = default;
  Builder & operator=(Builder &&) noexcept = default;

  // Re-arm a finished (or fresh) Builder for another vector without
  // reconstructing it. The previous vector's records array and slice blocks
  // belong to its finished store and are NOT reused. records_'s heap buffer
  // keeps a stable address across Builder moves (the unique_ptr inside
  // strview_array moves by pointer), so the shard's records pointer stays
  // valid; any unfinished slice chain (reset before finish()) is freed.
  void reset(R_xlen_t n) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    records_ = charport::internal::strview_array(static_cast<size_t>(n));
    shard_ = charport::internal::charvec_shard(records_.data(), records_.size());
    finished_ = false;
  }

  // Write element i. The store keeps the encoding as given -- it applies no
  // encoding policy (that belongs a layer above). The one convention: ptr ==
  // NULL is NA, regardless of the encoding passed. ptr only needs to live for
  // the call -- the bytes are copied into the builder's slices. Throws only in
  // the genuinely-broken cases (index out of range, len > INT_MAX).
  void set(R_xlen_t i, const char * ptr, size_t len, charport_enc enc) {
    if(ptr == nullptr || enc == charport_enc::CE_NA) {
      shard_.assign(static_cast<size_t>(i), nullptr, 0, charport_enc::CE_NA);
      return;
    }
    shard_.assign(static_cast<size_t>(i), ptr, len, enc);
  }
  void set(R_xlen_t i, const StrView & v) {
    set(i, v.ptr, static_cast<size_t>(v.len), v.enc);
  }
  void set_na(R_xlen_t i) {
    set(i, nullptr, 0, charport_enc::CE_NA);
  }

  // Zero-copy write: reserve len bytes for element i with encoding enc and
  // point the record at them, returning the buffer for the caller to fill with
  // exactly len bytes (no intermediate copy). The encoding is kept as given;
  // use set_na for NA. len == 0 returns a valid pointer to zero bytes. The
  // returned pointer stays valid until finish() (slice blocks never move);
  // writing past len, or reserving the same index twice, is a bug. Throws only
  // on index out of range / len > INT_MAX.
  char * reserve(R_xlen_t i, size_t len, charport_enc enc) {
    return shard_.reserve(static_cast<size_t>(i), len, enc);
  }

  // Wrap the finished store in a charvec SEXP; single-shot. Main R thread.
  SEXP finish() {
    if(finished_) {
      throw std::runtime_error("charvec builder: already finished");
    }
    std::array<charport::internal::charvec_shard, 1> shards{{std::move(shard_)}};
    auto store = charport::internal::make_unique<charport::internal::charvec_data>(
      std::move(records_), shards);
    finished_ = true;
    static charport_charvec_wrap_t wrap =
      reinterpret_cast<charport_charvec_wrap_t>(cp::detail::fetch("charport_charvec_wrap"));
    return wrap(store.release());  // charport owns the store from here
  }

private:
  charport::internal::strview_array records_;
  charport::internal::charvec_shard shard_;
  bool finished_ = false;
};

} // namespace charvec
} // namespace cp

#endif
