#ifndef CHARPORT_CHARVEC_BUILDER_H
#define CHARPORT_CHARVEC_BUILDER_H

// Header-only charvec builders. Include charport.h from packages.

#include "../interop/reader.h"
#include "store.h"

#include <memory>
#include <stdexcept>
#include <vector>

namespace charport {
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

  void reset(R_xlen_t n) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    records_ = internal::strview_array(static_cast<size_t>(n));
    shard_ = internal::charvec_shard();
    finished_ = false;
  }

  void set(R_xlen_t i, const char * ptr, size_t len, charport_enc enc) {
    if(ptr == nullptr || enc == charport_enc::CE_NA) {
      internal::copy_record(shard_, records_.data(), records_.size(),
                            static_cast<size_t>(i), nullptr, 0, charport_enc::CE_NA);
      return;
    }
    internal::copy_record(shard_, records_.data(), records_.size(),
                          static_cast<size_t>(i), ptr, len, enc);
  }

  void set(R_xlen_t i, const StrView & value) {
    set(i, value.ptr, static_cast<size_t>(value.len), value.enc);
  }

  void set_na(R_xlen_t i) {
    set(i, nullptr, 0, charport_enc::CE_NA);
  }

  char * reserve(R_xlen_t i, size_t len, charport_enc enc) {
    return internal::fill_record(shard_, records_.data(), records_.size(),
                                 static_cast<size_t>(i), len, enc);
  }

  std::unique_ptr<internal::charvec_data> release_store() {
    if(finished_) {
      throw std::runtime_error("charvec builder: already finished");
    }
    auto store = internal::make_unique<internal::charvec_data>(std::move(records_));
    store->adopt_chain(shard_);
    finished_ = true;
    return store;
  }

  SEXP to_charvec() {
    static charport_charvec_wrap_t wrap =
      reinterpret_cast<charport_charvec_wrap_t>(detail::fetch("charport_charvec_wrap"));
    return wrap(release_store().release());
  }

  template<typename Fill>
  static std::unique_ptr<internal::charvec_data> build_store(R_xlen_t n, Fill && fill) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    internal::strview_array records(static_cast<size_t>(n));
    internal::charvec_shard shard;
    fill(shard, records.data(), records.size());
    auto store = internal::make_unique<internal::charvec_data>(std::move(records));
    store->adopt_chain(shard);
    return store;
  }

private:
  internal::strview_array records_;
  internal::charvec_shard shard_;
  bool finished_ = false;
};

class BuilderMT {
public:
  BuilderMT(R_xlen_t n, size_t n_shards) {
    reset(n, n_shards);
  }

  BuilderMT(const BuilderMT &) = delete;
  BuilderMT & operator=(const BuilderMT &) = delete;
  BuilderMT(BuilderMT &&) noexcept = default;
  BuilderMT & operator=(BuilderMT &&) noexcept = default;

  void reset(R_xlen_t n, size_t n_shards) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    if(n_shards == 0) {
      throw std::runtime_error("charvec builder: n_shards must be >= 1");
    }
    records_ = internal::strview_array(static_cast<size_t>(n));
    shards_.clear();
    shards_.reserve(n_shards);
    for(size_t i = 0; i < n_shards; ++i) {
      shards_.emplace_back();
    }
    finished_ = false;
  }

  size_t n_shards() const noexcept { return shards_.size(); }

  void set(size_t shard, R_xlen_t i, const char * ptr, size_t len, charport_enc enc) {
    internal::charvec_shard & s = shard_at(shard);
    if(ptr == nullptr || enc == charport_enc::CE_NA) {
      internal::copy_record(s, records_.data(), records_.size(),
                            static_cast<size_t>(i), nullptr, 0, charport_enc::CE_NA);
      return;
    }
    internal::copy_record(s, records_.data(), records_.size(),
                          static_cast<size_t>(i), ptr, len, enc);
  }

  void set(size_t shard, R_xlen_t i, const StrView & value) {
    set(shard, i, value.ptr, static_cast<size_t>(value.len), value.enc);
  }

  void set_na(size_t shard, R_xlen_t i) {
    set(shard, i, nullptr, 0, charport_enc::CE_NA);
  }

  char * reserve(size_t shard, R_xlen_t i, size_t len, charport_enc enc) {
    return internal::fill_record(shard_at(shard), records_.data(), records_.size(),
                                 static_cast<size_t>(i), len, enc);
  }

  std::unique_ptr<internal::charvec_data> release_store() {
    if(finished_) {
      throw std::runtime_error("charvec builder: already finished");
    }
    auto store = internal::make_unique<internal::charvec_data>(std::move(records_));
    for(internal::charvec_shard & shard : shards_) {
      store->adopt_chain(shard);
    }
    finished_ = true;
    shards_.clear();
    return store;
  }

  SEXP to_charvec() {
    static charport_charvec_wrap_t wrap =
      reinterpret_cast<charport_charvec_wrap_t>(detail::fetch("charport_charvec_wrap"));
    return wrap(release_store().release());
  }

private:
  internal::charvec_shard & shard_at(size_t shard) {
    if(shard >= shards_.size()) {
      throw std::runtime_error("charvec builder: shard index out of range");
    }
    return shards_[shard];
  }

  internal::strview_array records_;
  std::vector<internal::charvec_shard> shards_;
  bool finished_ = false;
};

} // namespace charvec
} // namespace charport

#endif
