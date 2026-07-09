#ifndef CHARPORT_CHARVEC_STORE_H
#define CHARPORT_CHARVEC_STORE_H

// Native charvec payload: fixed record array plus owned payload slices.

#include "detail.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace charport {
namespace charvec {
namespace components {

constexpr size_t min_multi_string_slice_bytes() noexcept { return 64; }
constexpr size_t max_initial_slice_bytes() noexcept { return 16U << 10; }
constexpr size_t max_slice_bytes() noexcept { return 256U << 10; }
constexpr size_t slice_alignment() noexcept { return 64; }
constexpr size_t vector_len_scale() noexcept { return 4; }
inline const char * empty_data() noexcept {
  static const char empty = '\0';
  return &empty;
}

inline charport_strview na_record() noexcept {
  return make_strview(nullptr, NA_INTEGER, cetype_ext_t::CE_NA);
}

inline charport_strview empty_record(cetype_ext_t enc) noexcept {
  return make_strview(empty_data(), 0, enc);
}

inline int checked_string_size(size_t len, const char * what = "string length") {
  if(!internal::check_r_string_len(len)) {
    throw std::runtime_error(std::string(what) + " exceeds R string size");
  }
  return static_cast<int>(len);
}

inline size_t initial_slice_heuristic(size_t vector_len) noexcept {
  if(vector_len <= 1) {
    return 0;
  }
  const size_t scaled = internal::next_power_of_two(vector_len * vector_len_scale());
  return std::min(std::max(min_multi_string_slice_bytes(), scaled), max_initial_slice_bytes());
}

class SliceChain {
  struct Slice {
    Slice * next;
  };

  Slice * head_ = nullptr;

  static Slice * allocate(size_t payload_bytes, Slice * next) {
#if defined(SIZE_MAX) && defined(UINT32_MAX) && SIZE_MAX == UINT32_MAX
    if(payload_bytes > SIZE_MAX - sizeof(Slice)) {
      throw std::bad_alloc();
    }
#endif
    void * mem = ::operator new(sizeof(Slice) + payload_bytes);
    return new(mem) Slice{next};
  }

  static void deallocate(Slice * slice) noexcept {
    slice->~Slice();
    ::operator delete(slice);
  }

  static char * data(Slice * slice) noexcept {
    return reinterpret_cast<char *>(slice) + sizeof(Slice);
  }

public:
  SliceChain() noexcept = default;
  SliceChain(const SliceChain &) = delete;
  SliceChain & operator=(const SliceChain &) = delete;

  SliceChain(SliceChain && other) noexcept : head_(other.head_) {
    other.head_ = nullptr;
  }

  SliceChain & operator=(SliceChain && other) noexcept {
    if(this != &other) {
      clear();
      head_ = other.head_;
      other.head_ = nullptr;
    }
    return *this;
  }

  ~SliceChain() { clear(); }

  bool empty() const noexcept { return head_ == nullptr; }

  char * front_data() noexcept {
    return head_ == nullptr ? nullptr : data(head_);
  }

  char * push_front(size_t payload_bytes) {
    Slice * slice = allocate(payload_bytes, head_);
    head_ = slice;
    return data(slice);
  }

  char * data_slice(size_t idx) noexcept {
    Slice * slice = head_;
    while(slice != nullptr && idx > 0) {
      slice = slice->next;
      --idx;
    }
    return slice == nullptr ? nullptr : data(slice);
  }

  size_t count() const noexcept {
    size_t count = 0;
    for(Slice * slice = head_; slice != nullptr; slice = slice->next) {
      ++count;
    }
    return count;
  }

  void clear() noexcept {
    while(head_ != nullptr) {
      Slice * next = head_->next;
      deallocate(head_);
      head_ = next;
    }
  }

  void prepend(SliceChain & other) noexcept {
    Slice * head = other.head_;
    if(head == nullptr) {
      return;
    }
    other.head_ = nullptr;
    Slice * tail = head;
    while(tail->next != nullptr) {
      tail = tail->next;
    }
    tail->next = head_;
    head_ = head;
  }
};

struct RecordTable {
  std::unique_ptr<const char *[]> ptrs_;
  std::unique_ptr<int[]> lens_;
  std::unique_ptr<cetype_ext_t[]> encs_;
  size_t vector_length_;

  RecordTable() noexcept : ptrs_(), lens_(), encs_(), vector_length_(0) {}

  explicit RecordTable(size_t vector_length)
    : ptrs_(vector_length ? new const char *[vector_length] : nullptr),
      lens_(vector_length ? new int[vector_length] : nullptr),
      encs_(vector_length ? new cetype_ext_t[vector_length] : nullptr),
      vector_length_(vector_length) {
    for(size_t i = 0; i < vector_length; ++i) {
      set_na(i);
    }
  }

  RecordTable(const RecordTable & other)
    : ptrs_(other.vector_length_ ? new const char *[other.vector_length_] : nullptr),
      lens_(other.vector_length_ ? new int[other.vector_length_] : nullptr),
      encs_(other.vector_length_ ? new cetype_ext_t[other.vector_length_] : nullptr),
      vector_length_(other.vector_length_) {
    if(vector_length_ > 0) {
      std::copy(other.ptrs_.get(), other.ptrs_.get() + vector_length_, ptrs_.get());
      std::copy(other.lens_.get(), other.lens_.get() + vector_length_, lens_.get());
      std::copy(other.encs_.get(), other.encs_.get() + vector_length_, encs_.get());
    }
  }

  RecordTable & operator=(const RecordTable & other) {
    RecordTable tmp(other);
    *this = std::move(tmp);
    return *this;
  }

  RecordTable(RecordTable && other) noexcept
    : ptrs_(std::move(other.ptrs_)),
      lens_(std::move(other.lens_)),
      encs_(std::move(other.encs_)),
      vector_length_(other.vector_length_) {
    other.vector_length_ = 0;
  }

  RecordTable & operator=(RecordTable && other) noexcept {
    ptrs_ = std::move(other.ptrs_);
    lens_ = std::move(other.lens_);
    encs_ = std::move(other.encs_);
    vector_length_ = other.vector_length_;
    other.vector_length_ = 0;
    return *this;
  }

  size_t size() const noexcept { return vector_length_; }
  const char ** ptrs() noexcept { return ptrs_.get(); }
  const char * const * ptrs() const noexcept { return ptrs_.get(); }
  int * lengths() noexcept { return lens_.get(); }
  const int * lengths() const noexcept { return lens_.get(); }
  cetype_ext_t * encodings() noexcept { return encs_.get(); }
  const cetype_ext_t * encodings() const noexcept { return encs_.get(); }

  charport_byteview byteview(size_t i) const noexcept {
    return make_byteview(ptrs_[i], lens_[i]);
  }

  charport_strview view(size_t i) const noexcept {
    return make_strview(ptrs_[i], lens_[i], encs_[i]);
  }

  int length(size_t i) const noexcept { return lens_[i]; }
  cetype_ext_t encoding(size_t i) const noexcept { return encs_[i]; }

  void set(size_t i, const char * ptr, int len, cetype_ext_t enc) noexcept {
    ptrs_[i] = ptr;
    lens_[i] = len;
    encs_[i] = enc;
  }

  void set_na(size_t i) noexcept {
    set(i, nullptr, NA_INTEGER, cetype_ext_t::CE_NA);
  }
};

struct BuilderShard {
  SliceChain slices;
  size_t allocated_bytes = 0;
  uint32_t current_slice_used = 0;
  uint32_t current_slice_capacity = 0;

  BuilderShard() noexcept = default;
  BuilderShard(const BuilderShard &) = delete;
  BuilderShard & operator=(const BuilderShard &) = delete;
  BuilderShard(BuilderShard &&) noexcept = default;
  BuilderShard & operator=(BuilderShard &&) noexcept = default;
};

inline char * shard_allocate_bytes(BuilderShard & shard, uint32_t len, size_t vector_length_hint) {
  if(len == 0) {
    return const_cast<char*>(empty_data());
  }
  if(shard.slices.empty() || shard.current_slice_capacity - shard.current_slice_used < len) {
    const size_t initial = initial_slice_heuristic(vector_length_hint);
    const size_t regular = std::min(
      internal::round_up(std::max(initial, shard.allocated_bytes / 2), slice_alignment()),
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

inline char * shard_push_slice(BuilderShard & shard, size_t len) {
  if(len == 0) {
    return const_cast<char*>(empty_data());
  }
  const uint32_t cap = internal::checked_u32(len, "slice size");
  char * data = shard.slices.push_front(static_cast<size_t>(cap));
  shard.current_slice_used = cap;
  shard.current_slice_capacity = cap;
  shard.allocated_bytes += static_cast<size_t>(cap);
  return data;
}

inline char * shard_data_slice(BuilderShard & shard, size_t idx) noexcept {
  return shard.slices.data_slice(idx);
}

inline char * fill_record(BuilderShard & shard, RecordTable & records,
                          size_t idx, size_t len, cetype_ext_t enc) {
  const size_t vector_length = records.size();
  if(idx >= vector_length) {
    throw std::runtime_error("charvec builder: assignment out of bounds");
  }
  const int stored_len = checked_string_size(len, "stored string length");
  if(enc == cetype_ext_t::CE_NA) {
    records.set_na(idx);
    return nullptr;
  }
  if(stored_len == 0) {
    records.set(idx, empty_data(), 0, enc);
    return const_cast<char*>(empty_data());
  }
  char * dest = shard_allocate_bytes(shard, static_cast<uint32_t>(stored_len), vector_length);
  records.set(idx, dest, stored_len, enc);
  return dest;
}

inline void copy_record(BuilderShard & shard, RecordTable & records,
                        size_t idx, const char * ptr, size_t len, cetype_ext_t enc) {
  if(enc != cetype_ext_t::CE_NA && ptr == nullptr && len > 0) {
    throw std::runtime_error("cannot assign non-NA null bytes");
  }
  char * dest = fill_record(shard, records, idx, len, enc);
  if(dest != nullptr && len > 0) {
    std::memcpy(dest, ptr, len);
  }
}

inline void copy_record(BuilderShard & shard, RecordTable & records,
                        size_t idx, const charport_strview & value) {
  const size_t len = value.is_na() ? 0 : static_cast<size_t>(value.len);
  copy_record(shard, records, idx, value.ptr, len, value.enc);
}

} // namespace components

class Store {
  components::SliceChain slices_;

public:
  components::RecordTable records;

  Store() noexcept : slices_(), records() {}
  explicit Store(size_t len) : slices_(), records(len) {}
  explicit Store(components::RecordTable && recs) noexcept
    : slices_(), records(std::move(recs)) {}

  Store(const Store & other) : slices_(), records() {
    rebuild_from(other.records);
  }

  Store(Store &&) noexcept = default;

  Store & operator=(const Store & other) {
    if(this == &other) return *this;
    Store tmp(other);
    *this = std::move(tmp);
    return *this;
  }

  Store & operator=(Store &&) noexcept = default;

  size_t size() const noexcept { return records.size(); }
  charport_strview view(size_t idx) const noexcept { return records.view(idx); }
  charport_byteview byteview(size_t idx) const noexcept { return records.byteview(idx); }
  int length(size_t idx) const noexcept { return records.length(idx); }
  cetype_ext_t encoding(size_t idx) const noexcept { return records.encoding(idx); }

  void free_slices() noexcept {
    slices_.clear();
  }

  size_t slice_count() const noexcept {
    return slices_.count();
  }

  char * push_slice(uint32_t len) {
    return slices_.push_front(static_cast<size_t>(len));
  }

  void adopt_chain(components::BuilderShard & shard) noexcept {
    slices_.prepend(shard.slices);
  }

  void clear() {
    free_slices();
    records = components::RecordTable();
  }

  void compact() {
    rebuild_from(records);
  }

  char * reserve(size_t idx, size_t len, cetype_ext_t enc) {
    if(!internal::check_r_string_len(len)) {
      throw std::runtime_error("stored string length exceeds R string size");
    }
    const int stored_len = components::checked_string_size(len, "stored string length");
    if(idx >= records.size()) {
      throw std::runtime_error("charvec store assignment out of bounds");
    }

    const charport_strview current = records.view(idx);
    if(enc == cetype_ext_t::CE_NA) {
      records.set_na(idx);
      return nullptr;
    }
    if(stored_len == 0) {
      records.set(idx, components::empty_data(), 0, enc);
      return const_cast<char*>(components::empty_data());
    }
    if(!current.is_na() && current.len >= stored_len) {
      records.set(idx, current.ptr, stored_len, enc);
      return const_cast<char*>(current.ptr);
    }
    char * dest = push_slice(static_cast<uint32_t>(stored_len));
    records.set(idx, dest, stored_len, enc);
    return dest;
  }

  void assign(size_t idx, const char * ptr, size_t len, cetype_ext_t enc) {
    if(enc != cetype_ext_t::CE_NA && ptr == nullptr && len > 0) {
      throw std::runtime_error("cannot assign non-NA null bytes");
    }
    char * dest = reserve(idx, len, enc);
    if(dest != nullptr && len > 0) {
      std::memmove(dest, ptr, len);
    }
  }

  void assign(size_t idx, const charport_strview & value) {
    const size_t len = value.is_na() ? 0 : static_cast<size_t>(value.len);
    assign(idx, value.ptr, len, value.enc);
  }

  void rebuild_from(const components::RecordTable & source) {
    components::SliceChain slices;
    components::RecordTable out(source.size());
    const size_t max_block = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
    size_t i = 0;

    while(i < source.size()) {
      size_t block_bytes = 0;
      size_t j = i;
      while(j < source.size()) {
        const charport_strview rec = source.view(j);
        const size_t len = rec.is_na() ? 0 : static_cast<size_t>(rec.len);
        if(len > max_block - block_bytes) {
          break;
        }
        block_bytes += len;
        ++j;
      }

      if(block_bytes > 0) {
        char * write_ptr = slices.push_front(block_bytes);
        for(size_t k = i; k < j; ++k) {
          const charport_strview rec = source.view(k);
          if(rec.is_na()) {
            continue;
          }
          if(rec.len == 0) {
            out.set(k, components::empty_data(), 0, rec.enc);
            continue;
          }
          std::memcpy(write_ptr, rec.ptr, static_cast<size_t>(rec.len));
          out.set(k, write_ptr, rec.len, rec.enc);
          write_ptr += rec.len;
        }
      } else {
        for(size_t k = i; k < j; ++k) {
          const charport_strview rec = source.view(k);
          if(!rec.is_na() && rec.len == 0) {
            out.set(k, components::empty_data(), 0, rec.enc);
          }
        }
      }
      i = (j > i) ? j : i + 1;
    }

    slices_ = std::move(slices);
    records = std::move(out);
  }
};

} // namespace charvec
} // namespace charport

#endif
