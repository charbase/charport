#ifndef CHARPORT_BUILDER_MULTI_THREADED_H
#define CHARPORT_BUILDER_MULTI_THREADED_H

// cp::charvec::BuilderMT -- parallel construction of a charvec, never creating
// CHARSXPs. The BuilderMT owns the records array and a fixed vector of
// charvec_shards (one per worker, each a private thread-local bump chain); the
// write logic lives in the same free helpers the serial Builder uses
// (fill_record / copy_record), fed this builder's shared records array. A shard
// holds nothing shared, so distinct shards may be written concurrently from
// distinct threads with no synchronization. Every write names its shard:
//
//   cp::charvec::BuilderMT b(n, k);          // n elements, k shards, all NA
//   // worker j (j in [0, k)) owns a disjoint index range [lo, hi):
//   for(R_xlen_t i = lo; i < hi; ++i) b.set(j, i, ...);
//   SEXP out = b.to_charvec();               // merge the k chains, wrap
//
// Threading contract: set(shard, ...) / reserve(shard, ...) touch only
// shards_[shard] and the named records slot. Two threads are safe iff they pass
// different shard indices AND write disjoint element indices. Construct, hand
// each worker its shard index, join all workers, THEN exit -- construction and
// to_charvec()/release_store() are main-R-thread only; the set()/reserve() calls
// touch no R API. Unwritten elements remain NA. Spent after an exit; reset
// before reuse.

#include "base.h"
#include "charvec_store.h"

#include <memory>
#include <vector>

namespace cp {
namespace charvec {

class BuilderMT {
public:
  // n elements, n_shards (>= 1) independent thread-local write contexts.
  BuilderMT(R_xlen_t n, size_t n_shards) {
    reset(n, n_shards);
  }

  BuilderMT(const BuilderMT &) = delete;
  BuilderMT & operator=(const BuilderMT &) = delete;
  BuilderMT(BuilderMT &&) noexcept = default;       // vector<charvec_shard> is move-only RAII
  BuilderMT & operator=(BuilderMT &&) noexcept = default;

  // Re-arm for another vector. The shard vector's capacity is reused across
  // calls (a loop over many vectors pays it once); the RAII shards free any
  // prior chains as they are cleared.
  void reset(R_xlen_t n, size_t n_shards) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    if(n_shards == 0) {
      throw std::runtime_error("charvec builder: n_shards must be >= 1");
    }
    records_ = charport::internal::strview_array(static_cast<size_t>(n));
    shards_.clear();
    shards_.reserve(n_shards);
    for(size_t s = 0; s < n_shards; ++s) {
      shards_.emplace_back();
    }
    finished_ = false;
  }

  size_t n_shards() const noexcept { return shards_.size(); }

  // Write element i through shard `shard`. No encoding policy; ptr == NULL is
  // NA. No R API: safe on a worker thread. ptr need only live for the call.
  // Throws on bad shard index, index out of range, or len > INT_MAX.
  void set(size_t shard, R_xlen_t i, const char * ptr, size_t len, charport_enc enc) {
    charport::internal::charvec_shard & sh = shard_at(shard);
    if(ptr == nullptr || enc == charport_enc::CE_NA) {
      charport::internal::copy_record(sh, records_.data(), records_.size(),
                                      static_cast<size_t>(i), nullptr, 0, charport_enc::CE_NA);
      return;
    }
    charport::internal::copy_record(sh, records_.data(), records_.size(),
                                    static_cast<size_t>(i), ptr, len, enc);
  }
  void set(size_t shard, R_xlen_t i, const StrView & v) {
    set(shard, i, v.ptr, static_cast<size_t>(v.len), v.enc);
  }
  void set_na(size_t shard, R_xlen_t i) {
    set(shard, i, nullptr, 0, charport_enc::CE_NA);
  }

  // Zero-copy write through shard `shard` (see Builder::reserve). Distinct shard
  // indices reserve concurrently with no synchronization.
  char * reserve(size_t shard, R_xlen_t i, size_t len, charport_enc enc) {
    return charport::internal::fill_record(shard_at(shard), records_.data(), records_.size(),
                                           static_cast<size_t>(i), len, enc);
  }

  // Hand back the finished store (every shard chain spliced in); single-shot.
  std::unique_ptr<charport::internal::charvec_data> release_store() {
    if(finished_) {
      throw std::runtime_error("charvec builder: already finished");
    }
    auto store = charport::internal::make_unique<charport::internal::charvec_data>(
      std::move(records_));
    for(charport::internal::charvec_shard & sh : shards_) {
      store->adopt_chain(sh);
    }
    finished_ = true;
    shards_.clear();
    return store;
  }

  // Wrap the finished store as a charvec SEXP; single-shot. Main R thread,
  // after every worker has joined.
  SEXP to_charvec() {
    static charport_charvec_wrap_t wrap =
      reinterpret_cast<charport_charvec_wrap_t>(cp::detail::fetch("charport_charvec_wrap"));
    return wrap(release_store().release());
  }

private:
  charport::internal::charvec_shard & shard_at(size_t shard) {
    if(shard >= shards_.size()) {
      throw std::runtime_error("charvec builder: shard index out of range");
    }
    return shards_[shard];
  }

  // records_'s heap buffer is address-stable across BuilderMT moves, and shards_
  // is reserved at construction so it never reallocates; both let workers index
  // straight in while the builder stays movable.
  charport::internal::strview_array records_;
  std::vector<charport::internal::charvec_shard> shards_;
  bool finished_ = false;
};

} // namespace charvec
} // namespace cp

#endif
