#ifndef CHARPORT_CHARVEC_STORE_H
#define CHARPORT_CHARVEC_STORE_H

// Native charvec payload: fixed record array plus owned payload slices.

#include "detail.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace charport {
namespace internal {

constexpr size_t min_multi_string_slice_bytes() noexcept { return 64; }
constexpr size_t max_initial_slice_bytes() noexcept { return 16U << 10; }
constexpr size_t max_slice_bytes() noexcept { return 256U << 10; }
constexpr size_t slice_alignment() noexcept { return 64; }
constexpr size_t vector_len_scale() noexcept { return 4; }
constexpr size_t slice_header_bytes() noexcept { return sizeof(char *); }

inline char * slice_next(char * block) noexcept {
  char * next;
  std::memcpy(&next, block, sizeof(next));
  return next;
}

inline void slice_set_next(char * block, char * next) noexcept {
  std::memcpy(block, &next, sizeof(next));
}

inline char * slice_payload(char * block) noexcept {
  return block + slice_header_bytes();
}

inline void free_slice_chain(char * head) noexcept {
  while(head != nullptr) {
    char * next = slice_next(head);
    delete[] head;
    head = next;
  }
}

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
  if(!check_r_string_len(len)) {
    throw std::runtime_error(std::string(what) + " exceeds R string size");
  }
  return static_cast<int>(len);
}

inline size_t initial_slice_heuristic(size_t vector_len) noexcept {
  if(vector_len <= 1) {
    return 0;
  }
  const size_t scaled = next_power_of_two(vector_len * vector_len_scale());
  return std::min(std::max(min_multi_string_slice_bytes(), scaled), max_initial_slice_bytes());
}

struct charvec_records {
  std::unique_ptr<const char *[]> ptrs_;
  std::unique_ptr<int[]> lens_;
  std::unique_ptr<cetype_ext_t[]> encs_;
  size_t n_;

  charvec_records() noexcept : ptrs_(), lens_(), encs_(), n_(0) {}

  explicit charvec_records(size_t n)
    : ptrs_(n ? new const char *[n] : nullptr),
      lens_(n ? new int[n] : nullptr),
      encs_(n ? new cetype_ext_t[n] : nullptr),
      n_(n) {
    for(size_t i = 0; i < n; ++i) {
      set_na(i);
    }
  }

  charvec_records(const charvec_records & other)
    : ptrs_(other.n_ ? new const char *[other.n_] : nullptr),
      lens_(other.n_ ? new int[other.n_] : nullptr),
      encs_(other.n_ ? new cetype_ext_t[other.n_] : nullptr),
      n_(other.n_) {
    if(n_ > 0) {
      std::copy(other.ptrs_.get(), other.ptrs_.get() + n_, ptrs_.get());
      std::copy(other.lens_.get(), other.lens_.get() + n_, lens_.get());
      std::copy(other.encs_.get(), other.encs_.get() + n_, encs_.get());
    }
  }

  charvec_records & operator=(const charvec_records & other) {
    charvec_records tmp(other);
    *this = std::move(tmp);
    return *this;
  }

  charvec_records(charvec_records && other) noexcept
    : ptrs_(std::move(other.ptrs_)),
      lens_(std::move(other.lens_)),
      encs_(std::move(other.encs_)),
      n_(other.n_) {
    other.n_ = 0;
  }

  charvec_records & operator=(charvec_records && other) noexcept {
    ptrs_ = std::move(other.ptrs_);
    lens_ = std::move(other.lens_);
    encs_ = std::move(other.encs_);
    n_ = other.n_;
    other.n_ = 0;
    return *this;
  }

  size_t size() const noexcept { return n_; }
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

struct charvec_shard {
  char * slice_head = nullptr;
  size_t allocated_bytes = 0;
  uint32_t current_slice_used = 0;
  uint32_t current_slice_capacity = 0;

  charvec_shard() noexcept = default;
  charvec_shard(const charvec_shard &) = delete;
  charvec_shard & operator=(const charvec_shard &) = delete;

  charvec_shard(charvec_shard && other) noexcept
    : slice_head(other.slice_head),
      allocated_bytes(other.allocated_bytes),
      current_slice_used(other.current_slice_used),
      current_slice_capacity(other.current_slice_capacity) {
    other.slice_head = nullptr;
  }

  charvec_shard & operator=(charvec_shard && other) noexcept {
    if(this != &other) {
      free_slice_chain(slice_head);
      slice_head = other.slice_head;
      allocated_bytes = other.allocated_bytes;
      current_slice_used = other.current_slice_used;
      current_slice_capacity = other.current_slice_capacity;
      other.slice_head = nullptr;
    }
    return *this;
  }

  ~charvec_shard() { free_slice_chain(slice_head); }
};

inline char * shard_allocate_bytes(charvec_shard & shard, uint32_t len, size_t n_hint) {
  if(len == 0) {
    return const_cast<char*>(empty_data());
  }
  if(shard.slice_head == nullptr || shard.current_slice_capacity - shard.current_slice_used < len) {
    const size_t initial = initial_slice_heuristic(n_hint);
    const size_t regular = std::min(
      round_up(std::max(initial, shard.allocated_bytes / 2), slice_alignment()),
      max_slice_bytes());
    const size_t next_size = round_up(std::max(regular, static_cast<size_t>(len)),
                                      slice_alignment());
    const uint32_t cap = checked_u32(next_size, "slice size");
    char * block = new char[slice_header_bytes() + cap];
    slice_set_next(block, shard.slice_head);
    shard.slice_head = block;
    shard.current_slice_used = 0;
    shard.current_slice_capacity = cap;
  }
  char * dest = slice_payload(shard.slice_head) + shard.current_slice_used;
  shard.current_slice_used += len;
  shard.allocated_bytes += static_cast<size_t>(len);
  return dest;
}

inline char * shard_push_slice(charvec_shard & shard, size_t len) {
  if(len == 0) {
    return const_cast<char*>(empty_data());
  }
  const uint32_t cap = checked_u32(len, "slice size");
  char * block = new char[slice_header_bytes() + static_cast<size_t>(cap)];
  slice_set_next(block, shard.slice_head);
  shard.slice_head = block;
  shard.current_slice_used = cap;
  shard.current_slice_capacity = cap;
  shard.allocated_bytes += static_cast<size_t>(cap);
  return slice_payload(block);
}

inline char * shard_data_slice(charvec_shard & shard, size_t idx) noexcept {
  char * block = shard.slice_head;
  while(block != nullptr && idx > 0) {
    block = slice_next(block);
    --idx;
  }
  return block == nullptr ? nullptr : slice_payload(block);
}

inline char * fill_record(charvec_shard & shard, charvec_records & records,
                          size_t idx, size_t len, cetype_ext_t enc) {
  const size_t n = records.size();
  if(idx >= n) {
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
  char * dest = shard_allocate_bytes(shard, static_cast<uint32_t>(stored_len), n);
  records.set(idx, dest, stored_len, enc);
  return dest;
}

inline void copy_record(charvec_shard & shard, charvec_records & records,
                        size_t idx, const char * ptr, size_t len, cetype_ext_t enc) {
  if(enc != cetype_ext_t::CE_NA && ptr == nullptr && len > 0) {
    throw std::runtime_error("cannot assign non-NA null bytes");
  }
  char * dest = fill_record(shard, records, idx, len, enc);
  if(dest != nullptr && len > 0) {
    std::memcpy(dest, ptr, len);
  }
}

inline void copy_record(charvec_shard & shard, charvec_records & records,
                        size_t idx, const charport_strview & value) {
  const size_t len = value.is_na() ? 0 : static_cast<size_t>(value.len);
  copy_record(shard, records, idx, value.ptr, len, value.enc);
}

struct charvec_data {
  char * slice_head;
  charvec_records records;

  charvec_data() noexcept : slice_head(nullptr), records() {}
  explicit charvec_data(size_t len) : slice_head(nullptr), records(len) {}
  explicit charvec_data(charvec_records && recs) noexcept
    : slice_head(nullptr), records(std::move(recs)) {}

  charvec_data(const charvec_data & other) : slice_head(nullptr), records() {
    rebuild_from(other.records);
  }

  charvec_data(charvec_data && other) noexcept
    : slice_head(other.slice_head), records(std::move(other.records)) {
    other.slice_head = nullptr;
  }

  charvec_data & operator=(const charvec_data & other) {
    if(this == &other) return *this;
    charvec_data tmp(other);
    *this = std::move(tmp);
    return *this;
  }

  charvec_data & operator=(charvec_data && other) noexcept {
    if(this != &other) {
      free_slices();
      slice_head = other.slice_head;
      records = std::move(other.records);
      other.slice_head = nullptr;
    }
    return *this;
  }

  ~charvec_data() { free_slices(); }

  size_t size() const noexcept { return records.size(); }
  charport_strview view(size_t idx) const noexcept { return records.view(idx); }
  charport_byteview byteview(size_t idx) const noexcept { return records.byteview(idx); }
  int length(size_t idx) const noexcept { return records.length(idx); }
  cetype_ext_t encoding(size_t idx) const noexcept { return records.encoding(idx); }

  void free_slices() noexcept {
    free_slice_chain(slice_head);
    slice_head = nullptr;
  }

  size_t slice_count() const noexcept {
    size_t n = 0;
    for(char * block = slice_head; block != nullptr; block = slice_next(block)) {
      ++n;
    }
    return n;
  }

  char * push_slice(uint32_t len) {
    char * block = new char[slice_header_bytes() + static_cast<size_t>(len)];
    slice_set_next(block, slice_head);
    slice_head = block;
    return slice_payload(block);
  }

  void adopt_chain(charvec_shard & shard) noexcept {
    char * head = shard.slice_head;
    if(head == nullptr) {
      return;
    }
    shard.slice_head = nullptr;
    char * tail = head;
    while(slice_next(tail) != nullptr) {
      tail = slice_next(tail);
    }
    slice_set_next(tail, slice_head);
    slice_head = head;
  }

  void clear() {
    free_slices();
    records = charvec_records();
  }

  void compact() {
    rebuild_from(records);
  }

  char * reserve(size_t idx, size_t len, cetype_ext_t enc) {
    if(!check_r_string_len(len)) {
      throw std::runtime_error("stored string length exceeds R string size");
    }
    const int stored_len = checked_string_size(len, "stored string length");
    if(idx >= records.size()) {
      throw std::runtime_error("charvec store assignment out of bounds");
    }

    const charport_strview current = records.view(idx);
    if(enc == cetype_ext_t::CE_NA) {
      records.set_na(idx);
      return nullptr;
    }
    if(stored_len == 0) {
      records.set(idx, empty_data(), 0, enc);
      return const_cast<char*>(empty_data());
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

  void rebuild_from(const charvec_records & source) {
    std::vector<std::unique_ptr<char[]>> blocks;
    charvec_records out(source.size());
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
        std::unique_ptr<char[]> block(new char[slice_header_bytes() + block_bytes]);
        char * write_ptr = slice_payload(block.get());
        for(size_t k = i; k < j; ++k) {
          const charport_strview rec = source.view(k);
          if(rec.is_na()) {
            continue;
          }
          if(rec.len == 0) {
            out.set(k, empty_data(), 0, rec.enc);
            continue;
          }
          std::memcpy(write_ptr, rec.ptr, static_cast<size_t>(rec.len));
          out.set(k, write_ptr, rec.len, rec.enc);
          write_ptr += rec.len;
        }
        blocks.push_back(std::move(block));
      } else {
        for(size_t k = i; k < j; ++k) {
          const charport_strview rec = source.view(k);
          if(!rec.is_na() && rec.len == 0) {
            out.set(k, empty_data(), 0, rec.enc);
          }
        }
      }
      i = (j > i) ? j : i + 1;
    }

    free_slices();
    char * head = nullptr;
    for(std::unique_ptr<char[]> & block : blocks) {
      char * raw = block.release();
      slice_set_next(raw, head);
      head = raw;
    }
    slice_head = head;
    records = std::move(out);
  }
};

} // namespace internal
} // namespace charport

#endif
