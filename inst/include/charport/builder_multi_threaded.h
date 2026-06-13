#ifndef CHARPORT_BUILDER_MULTI_THREADED_H
#define CHARPORT_BUILDER_MULTI_THREADED_H

// cp::charvec::BuilderMT -- parallel construction of a charvec, never creating
// CHARSXPs. The number of shards is fixed at construction; each shard owns a
// private bump-allocated slice chain, so distinct shards may be written
// concurrently from distinct threads with no synchronization. There is no
// implicit shard and no serial set() -- every write names its shard:
//
//   cp::charvec::BuilderMT b(n, k);          // n elements, k shards, all NA
//   // worker j (j in [0, k)) owns a disjoint index range [lo, hi):
//   for(R_xlen_t i = lo; i < hi; ++i) b.set(j, i, ...);
//   SEXP out = b.finish();                   // merge the k chains, wrap
//
// Threading contract: set(shard, ...) mutates only shards_[shard]'s chain and
// the named record slot. Two threads are safe iff they pass different shard
// indices AND write disjoint element indices (contiguous disjoint ranges are
// the intended pattern). Unwritten elements remain NA. Construct, hand each
// worker its shard index, join all workers, THEN finish() -- construction and
// finish() are main-R-thread only; the set() calls touch no R API.
//
// The store is built in the consumer's own compiled code; only finish()
// crosses into charport (charport_charvec_wrap), which takes ownership and
// wraps the store verbatim (no encoding policy -- records are kept as given).

#include "base.h"
#include "../charport_internal/charvec_store.h"

#include <memory>
#include <vector>

namespace cp {
namespace charvec {

class BuilderMT {
public:
  // n elements, n_shards (>= 1) independent write contexts.
  BuilderMT(R_xlen_t n, size_t n_shards) {
    reset(n, n_shards);
  }

  BuilderMT(const BuilderMT &) = delete;
  BuilderMT & operator=(const BuilderMT &) = delete;
  BuilderMT(BuilderMT &&) noexcept = default;
  BuilderMT & operator=(BuilderMT &&) noexcept = default;

  // Re-arm a finished (or fresh) builder for another vector. The shard table
  // is rebuilt at n_shards entries; its vector storage capacity is reused
  // across calls, so a loop over many vectors pays it once. The previous
  // vector's records array and slice blocks belong to its finished store and
  // are NOT reused. records_'s heap buffer keeps a stable address across
  // BuilderMT moves, so the shards' records pointers stay valid; any
  // unfinished slice chains (reset before finish()) are freed.
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
      shards_.emplace_back(records_.data(), records_.size());
    }
    finished_ = false;
  }

  size_t n_shards() const noexcept { return shards_.size(); }

  // Write element i through shard `shard`. Like the single-threaded Builder,
  // the store keeps the encoding as given (no encoding policy); ptr == NULL is
  // NA. No R API: safe on a worker thread. ptr only needs to live for the
  // call. Throws only on a bad shard index, index out of range, or len >
  // INT_MAX.
  void set(size_t shard, R_xlen_t i, const char * ptr, size_t len, charport_enc enc) {
    if(shard >= shards_.size()) {
      throw std::runtime_error("charvec builder: shard index out of range");
    }
    charport::internal::charvec_shard & s = shards_[shard];
    if(ptr == nullptr || enc == charport_enc::CE_NA) {
      s.assign(static_cast<size_t>(i), nullptr, 0, charport_enc::CE_NA);
      return;
    }
    s.assign(static_cast<size_t>(i), ptr, len, enc);
  }
  void set(size_t shard, R_xlen_t i, const StrView & v) {
    set(shard, i, v.ptr, static_cast<size_t>(v.len), v.enc);
  }
  void set_na(size_t shard, R_xlen_t i) {
    set(shard, i, nullptr, 0, charport_enc::CE_NA);
  }

  // Zero-copy write through shard `shard` (see Builder::reserve). Reserves len
  // bytes for element i in that shard's private chain and returns the buffer
  // for the caller to fill. Distinct shard indices reserve concurrently with
  // no synchronization. The encoding is kept as given; use set_na for NA.
  char * reserve(size_t shard, R_xlen_t i, size_t len, charport_enc enc) {
    if(shard >= shards_.size()) {
      throw std::runtime_error("charvec builder: shard index out of range");
    }
    return shards_[shard].reserve(static_cast<size_t>(i), len, enc);
  }

  // Merge the shard chains (block move, no payload copy) and wrap the store in
  // a charvec SEXP; single-shot. Main R thread, after every worker has joined.
  SEXP finish() {
    if(finished_) {
      throw std::runtime_error("charvec builder: already finished");
    }
    auto store = charport::internal::make_unique<charport::internal::charvec_data>(
      std::move(records_), shards_);
    finished_ = true;
    shards_.clear();
    static charport_charvec_wrap_t wrap =
      reinterpret_cast<charport_charvec_wrap_t>(cp::detail::fetch("charport_charvec_wrap"));
    return wrap(store.release());  // charport owns the store from here
  }

private:
  charport::internal::strview_array records_;
  std::vector<charport::internal::charvec_shard> shards_;
  bool finished_ = false;
};

} // namespace charvec
} // namespace cp

#endif
