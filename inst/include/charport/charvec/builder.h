#ifndef CHARPORT_CHARVEC_BUILDER_H
#define CHARPORT_CHARVEC_BUILDER_H

// Header-only charvec builders. Include charport.h from packages.

#include "../interop/reader.h"
#include "store.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace charport {
namespace charvec {

namespace builder_detail {

constexpr size_t min_multi_string_slice_bytes() noexcept { return 64; }
constexpr size_t max_initial_slice_bytes() noexcept { return 16U << 10; }
constexpr size_t max_slice_bytes() noexcept { return 256U << 10; }
constexpr size_t slice_alignment() noexcept { return 64; }
constexpr size_t vector_len_scale() noexcept { return 4; }

inline size_t initial_slice_heuristic(size_t vector_len) noexcept {
  if(vector_len <= 1) {
    return 0;
  }
  const size_t scaled = internal::next_power_of_two(vector_len * vector_len_scale());
  return std::min(std::max(min_multi_string_slice_bytes(), scaled), max_initial_slice_bytes());
}

inline size_t growable_vector_length_hint() noexcept {
  return max_initial_slice_bytes() / vector_len_scale();
}

struct Shard {
  components::SliceChain slices;
  size_t allocated_bytes = 0;
  uint32_t current_slice_used = 0;
  uint32_t current_slice_capacity = 0;

  Shard() noexcept = default;
  Shard(const Shard &) = delete;
  Shard & operator=(const Shard &) = delete;
  Shard(Shard &&) noexcept = default;
  Shard & operator=(Shard &&) noexcept = default;
};

inline char * allocate_bytes(Shard & shard, uint32_t len, size_t vector_length_hint) {
  if(len == 0) {
    return const_cast<char*>(components::empty_data());
  }
  if(shard.slices.empty() || shard.current_slice_capacity - shard.current_slice_used < len) {
    const size_t initial = initial_slice_heuristic(vector_length_hint);
    const size_t regular = std::min(
      internal::round_up(
        std::max(initial, shard.allocated_bytes / 2), slice_alignment()),
      max_slice_bytes());
    const size_t next_size = internal::round_up(
      std::max(regular, static_cast<size_t>(len)), slice_alignment());
    const uint32_t cap = internal::checked_u32(next_size, "slice size");
    shard.slices.push_front(cap);
    shard.current_slice_used = 0;
    shard.current_slice_capacity = cap;
  }
  char * dest = shard.slices.front_data() + shard.current_slice_used;
  shard.current_slice_used += len;
  shard.allocated_bytes += static_cast<size_t>(len);
  return dest;
}

inline char * fill_record(Shard & shard, components::RecordTable & records,
                          size_t idx, size_t len, cetype_ext_t enc) {
  const size_t vector_length = records.size();
  if(idx >= vector_length) {
    throw std::runtime_error("charvec builder: assignment out of bounds");
  }
  const int stored_len = components::checked_string_size(len, "stored string length");
  if(enc == cetype_ext_t::CE_NA) {
    records.set_na(idx);
    return nullptr;
  }
  if(stored_len == 0) {
    records.set(idx, components::empty_data(), 0, enc);
    return const_cast<char*>(components::empty_data());
  }
  char * dest = allocate_bytes(shard, static_cast<uint32_t>(stored_len), vector_length);
  records.set(idx, dest, stored_len, enc);
  return dest;
}

inline void copy_record(Shard & shard, components::RecordTable & records,
                        size_t idx, const char * ptr, size_t len, cetype_ext_t enc) {
  char * dest = fill_record(shard, records, idx, len, enc);
  if(dest != nullptr && len > 0) {
    std::memcpy(dest, ptr, len);
  }
}

inline charport_charvec_wrap_t wrap_callable() {
  static charport_charvec_wrap_t fn = nullptr;
  if(fn == nullptr) {
    fn = reinterpret_cast<charport_charvec_wrap_t>(
      charport::detail::fetch("charport_charvec_wrap")
    );
  }
  return fn;
}

template<typename Backend>
inline SEXP wrap_store(Store && source) {
  std::unique_ptr<Store> store;
  return Backend::call([&]() -> SEXP {
    charport_charvec_wrap_t wrap = wrap_callable();
    store.reset(new Store(std::move(source)));
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
    shard_ = builder_detail::Shard();
  }

  void set(R_xlen_t i, const char * ptr, size_t len, cetype_ext_t enc) {
    if(ptr == nullptr || enc == cetype_ext_t::CE_NA) {
      builder_detail::copy_record(
        shard_, records_, static_cast<size_t>(i), nullptr, 0, cetype_ext_t::CE_NA);
      return;
    }
    builder_detail::copy_record(shard_, records_, static_cast<size_t>(i), ptr, len, enc);
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
    return builder_detail::fill_record(
      shard_, records_, static_cast<size_t>(i), len, enc);
  }

  Store release_store() {
    Store out(0, 0);
    out.records = std::move(records_);
    out.slices.prepend(shard_.slices);
    shard_ = builder_detail::Shard();
    return out;
  }

  SEXP to_sexp() {
    return builder_detail::wrap_store<Backend>(release_store());
  }

private:
  components::RecordTable records_;
  builder_detail::Shard shard_;
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
    builder_detail::Shard & s = shard_at(shard);
    if(ptr == nullptr || enc == cetype_ext_t::CE_NA) {
      builder_detail::copy_record(
        s, records_, static_cast<size_t>(i), nullptr, 0, cetype_ext_t::CE_NA);
      return;
    }
    builder_detail::copy_record(s, records_, static_cast<size_t>(i), ptr, len, enc);
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
    return builder_detail::fill_record(
      shard_at(shard), records_, static_cast<size_t>(i), len, enc);
  }

  Store release_store() {
    Store out(0, 0);
    out.records = std::move(records_);
    for(builder_detail::Shard & shard : shards_) {
      out.slices.prepend(shard.slices);
    }
    shards_.clear();
    return out;
  }

  SEXP to_sexp() {
    return builder_detail::wrap_store<Backend>(release_store());
  }

private:
  builder_detail::Shard & shard_at(size_t shard) {
    if(shard >= shards_.size()) {
      throw std::runtime_error("charvec builder: shard index out of range");
    }
    return shards_[shard];
  }

  components::RecordTable records_;
  std::vector<builder_detail::Shard> shards_;
};

template<typename Backend>
class BasicGrowableBuilder {
public:
  BasicGrowableBuilder() = default;

  BasicGrowableBuilder(const BasicGrowableBuilder &) = delete;
  BasicGrowableBuilder & operator=(const BasicGrowableBuilder &) = delete;
  BasicGrowableBuilder(BasicGrowableBuilder &&) noexcept = default;
  BasicGrowableBuilder & operator=(BasicGrowableBuilder &&) noexcept = default;

  void append(const StrView & value) {
    if(value.is_na()) {
      append(nullptr, 0, cetype_ext_t::CE_NA);
      return;
    }
    append(value.ptr, static_cast<size_t>(value.len), value.enc);
  }

  void append(const char * ptr, size_t len, cetype_ext_t enc) {
    if(ptr == nullptr || enc == cetype_ext_t::CE_NA) {
      append_reserve(0, cetype_ext_t::CE_NA);
      return;
    }
    char * dest = append_reserve(len, enc);
    if(len > 0) {
      std::memcpy(dest, ptr, len);
    }
  }

  char * append_reserve(size_t len, cetype_ext_t enc) {
    const int stored_len = components::checked_string_size(len, "stored string length");
    if(enc == cetype_ext_t::CE_NA) {
      records_.push_back(components::na_record());
      return nullptr;
    }
    if(stored_len == 0) {
      records_.push_back(components::empty_record(enc));
      return const_cast<char*>(components::empty_data());
    }

    char * dest = builder_detail::allocate_bytes(
      shard_, static_cast<uint32_t>(stored_len),
      builder_detail::growable_vector_length_hint());
    records_.push_back(make_strview(dest, stored_len, enc));
    return dest;
  }

  size_t size() const noexcept {
    return records_.size();
  }

  Store release_store() {
    Store out(records_.size(), 0);
    for(size_t i = 0; i < records_.size(); ++i) {
      const charport_strview record = records_[i];
      out.records.set(i, record.ptr, record.len, record.enc);
    }
    out.slices.prepend(shard_.slices);
    records_.clear();
    shard_ = builder_detail::Shard();
    return out;
  }

  SEXP to_sexp() {
    return builder_detail::wrap_store<Backend>(release_store());
  }

private:
  std::vector<charport_strview> records_;
  builder_detail::Shard shard_;
};

} // namespace charvec
} // namespace charport

#endif
