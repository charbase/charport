#ifndef CHARPORT_CHARVEC_BUILDER_H
#define CHARPORT_CHARVEC_BUILDER_H

// Header-only charvec builders. Include charport.h from packages.

#include "../interop/reader.h"
#include "store.h"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace charport {
namespace charvec {

namespace builder_detail {

inline charport_charvec_wrap_t wrap_callable() {
  static charport_charvec_wrap_t fn = nullptr;
  if(fn == nullptr) {
    fn = reinterpret_cast<charport_charvec_wrap_t>(
      charport::detail::fetch("charport_charvec_wrap")
    );
  }
  return fn;
}

template<typename Backend, typename Release>
inline SEXP wrap_store(Release && release) {
  std::unique_ptr<Store> store;
  return Backend::call([&]() -> SEXP {
    charport_charvec_wrap_t wrap = wrap_callable();
    store = std::forward<Release>(release)();
    return wrap(store.release());
  });
}

} // namespace builder_detail

template<typename Backend>
class BasicBuilder {
public:
  explicit BasicBuilder(R_xlen_t n) {
    reset(n);
  }

  BasicBuilder(const BasicBuilder &) = delete;
  BasicBuilder & operator=(const BasicBuilder &) = delete;
  BasicBuilder(BasicBuilder &&) noexcept = default;
  BasicBuilder & operator=(BasicBuilder &&) noexcept = default;

  void reset(R_xlen_t n) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    records_ = components::RecordTable(static_cast<size_t>(n));
    shard_ = components::BuilderShard();
  }

  void set(R_xlen_t i, const char * ptr, size_t len, cetype_ext_t enc) {
    if(ptr == nullptr || enc == cetype_ext_t::CE_NA) {
      components::copy_record(shard_, records_,
                              static_cast<size_t>(i), nullptr, 0, cetype_ext_t::CE_NA);
      return;
    }
    components::copy_record(shard_, records_, static_cast<size_t>(i), ptr, len, enc);
  }

  void set(R_xlen_t i, const StrView & value) {
    if(value.is_na()) {
      set_na(i);
      return;
    }
    set(i, value.ptr, static_cast<size_t>(value.len), value.enc);
  }

  void set_na(R_xlen_t i) {
    set(i, nullptr, 0, cetype_ext_t::CE_NA);
  }

  char * reserve(R_xlen_t i, size_t len, cetype_ext_t enc) {
    return components::fill_record(shard_, records_, static_cast<size_t>(i), len, enc);
  }

  std::unique_ptr<Store> release_store() {
    auto store = internal::make_unique<Store>(std::move(records_));
    store->adopt_chain(shard_);
    return store;
  }

  SEXP to_sexp() {
    return builder_detail::wrap_store<Backend>([&]() { return release_store(); });
  }

  template<typename Fill>
  static std::unique_ptr<Store> build_store(R_xlen_t n, Fill && fill) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    components::RecordTable records(static_cast<size_t>(n));
    components::BuilderShard shard;
    fill(shard, records);
    auto store = internal::make_unique<Store>(std::move(records));
    store->adopt_chain(shard);
    return store;
  }

private:
  components::RecordTable records_;
  components::BuilderShard shard_;
};

template<typename Backend>
class BasicDirectBuilder {
public:
  explicit BasicDirectBuilder(R_xlen_t n, size_t total_bytes = 0) {
    reset(n, total_bytes);
  }

  BasicDirectBuilder(const BasicDirectBuilder &) = delete;
  BasicDirectBuilder & operator=(const BasicDirectBuilder &) = delete;
  BasicDirectBuilder(BasicDirectBuilder &&) noexcept = default;
  BasicDirectBuilder & operator=(BasicDirectBuilder &&) noexcept = default;

  void reset(R_xlen_t n, size_t total_bytes = 0) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    records_ = components::RecordTable(static_cast<size_t>(n));
    shard_ = components::BuilderShard();
    if(total_bytes > 0) {
      allocate_next_slice(total_bytes);
    }
  }

  const char ** ptrs() noexcept {
    return records_.ptrs();
  }

  int * lengths() noexcept {
    return records_.lengths();
  }

  cetype_ext_t * encodings() noexcept {
    return records_.encodings();
  }

  const char * const * ptrs() const noexcept {
    return records_.ptrs();
  }

  const int * lengths() const noexcept {
    return records_.lengths();
  }

  const cetype_ext_t * encodings() const noexcept {
    return records_.encodings();
  }

  char * data_slice(size_t i = 0) {
    char * ptr = components::shard_data_slice(shard_, i);
    if(ptr == nullptr) {
      throw std::runtime_error("charvec direct builder: data slice index out of range");
    }
    return ptr;
  }

  char * allocate_next_slice(size_t bytes) {
    return components::shard_push_slice(shard_, bytes);
  }

  std::unique_ptr<Store> release_store() {
    auto store = internal::make_unique<Store>(std::move(records_));
    store->adopt_chain(shard_);
    return store;
  }

  SEXP to_sexp() {
    return builder_detail::wrap_store<Backend>([&]() { return release_store(); });
  }

private:
  components::RecordTable records_;
  components::BuilderShard shard_;
};

template<typename Backend>
class BasicParallelBuilder {
public:
  BasicParallelBuilder(R_xlen_t n, size_t n_shards) {
    reset(n, n_shards);
  }

  BasicParallelBuilder(const BasicParallelBuilder &) = delete;
  BasicParallelBuilder & operator=(const BasicParallelBuilder &) = delete;
  BasicParallelBuilder(BasicParallelBuilder &&) noexcept = default;
  BasicParallelBuilder & operator=(BasicParallelBuilder &&) noexcept = default;

  void reset(R_xlen_t n, size_t n_shards) {
    if(n < 0) {
      throw std::runtime_error("charvec builder: negative length");
    }
    if(n_shards == 0) {
      throw std::runtime_error("charvec builder: n_shards must be >= 1");
    }
    records_ = components::RecordTable(static_cast<size_t>(n));
    shards_.clear();
    shards_.reserve(n_shards);
    for(size_t i = 0; i < n_shards; ++i) {
      shards_.emplace_back();
    }
  }

  size_t n_shards() const noexcept { return shards_.size(); }

  void set(size_t shard, R_xlen_t i, const char * ptr, size_t len, cetype_ext_t enc) {
    components::BuilderShard & s = shard_at(shard);
    if(ptr == nullptr || enc == cetype_ext_t::CE_NA) {
      components::copy_record(s, records_,
                              static_cast<size_t>(i), nullptr, 0, cetype_ext_t::CE_NA);
      return;
    }
    components::copy_record(s, records_, static_cast<size_t>(i), ptr, len, enc);
  }

  void set(size_t shard, R_xlen_t i, const StrView & value) {
    if(value.is_na()) {
      set_na(shard, i);
      return;
    }
    set(shard, i, value.ptr, static_cast<size_t>(value.len), value.enc);
  }

  void set_na(size_t shard, R_xlen_t i) {
    set(shard, i, nullptr, 0, cetype_ext_t::CE_NA);
  }

  char * reserve(size_t shard, R_xlen_t i, size_t len, cetype_ext_t enc) {
    return components::fill_record(shard_at(shard), records_, static_cast<size_t>(i), len, enc);
  }

  std::unique_ptr<Store> release_store() {
    auto store = internal::make_unique<Store>(std::move(records_));
    for(components::BuilderShard & shard : shards_) {
      store->adopt_chain(shard);
    }
    shards_.clear();
    return store;
  }

  SEXP to_sexp() {
    return builder_detail::wrap_store<Backend>([&]() { return release_store(); });
  }

private:
  components::BuilderShard & shard_at(size_t shard) {
    if(shard >= shards_.size()) {
      throw std::runtime_error("charvec builder: shard index out of range");
    }
    return shards_[shard];
  }

  components::RecordTable records_;
  std::vector<components::BuilderShard> shards_;
};

template<typename Backend>
inline SEXP build_scalar_with(const StrView & value) {
  return builder_detail::wrap_store<Backend>([&]() {
    BasicBuilder<Backend> b(1);
    b.set(0, value);
    return b.release_store();
  });
}

} // namespace charvec
} // namespace charport

#endif
