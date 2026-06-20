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
  return make_strview(nullptr, 0, charport_enc::CE_NA);
}

inline charport_strview empty_record(charport_enc enc) noexcept {
  return make_strview(empty_data(), 0, enc);
}

inline size_t initial_slice_heuristic(size_t vector_len) noexcept {
  if(vector_len <= 1) {
    return 0;
  }
  const size_t scaled = next_power_of_two(vector_len * vector_len_scale());
  return std::min(std::max(min_multi_string_slice_bytes(), scaled), max_initial_slice_bytes());
}

struct strview_array {
  std::unique_ptr<charport_strview[]> data_;
  size_t n_;

  strview_array() noexcept : data_(), n_(0) {}

  explicit strview_array(size_t n) : data_(n ? new charport_strview[n] : nullptr), n_(n) {
    for(size_t i = 0; i < n; ++i) {
      data_[i] = na_record();
    }
  }

  strview_array(const strview_array & other)
    : data_(other.n_ ? new charport_strview[other.n_] : nullptr), n_(other.n_) {
    std::copy(other.data_.get(), other.data_.get() + n_, data_.get());
  }

  strview_array & operator=(const strview_array & other) {
    strview_array tmp(other);
    *this = std::move(tmp);
    return *this;
  }

  strview_array(strview_array && other) noexcept : data_(std::move(other.data_)), n_(other.n_) {
    other.n_ = 0;
  }

  strview_array & operator=(strview_array && other) noexcept {
    data_ = std::move(other.data_);
    n_ = other.n_;
    other.n_ = 0;
    return *this;
  }

  size_t size() const noexcept { return n_; }
  charport_strview * data() noexcept { return data_.get(); }
  const charport_strview * data() const noexcept { return data_.get(); }
  charport_strview & operator[](size_t i) noexcept { return data_[i]; }
  const charport_strview & operator[](size_t i) const noexcept { return data_[i]; }
  charport_strview * begin() noexcept { return data_.get(); }
  charport_strview * end() noexcept { return data_.get() + n_; }
  const charport_strview * begin() const noexcept { return data_.get(); }
  const charport_strview * end() const noexcept { return data_.get() + n_; }
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

inline char * fill_record(charvec_shard & shard, charport_strview * records, size_t n,
                          size_t idx, size_t len, charport_enc enc) {
  if(records == nullptr) {
    throw std::runtime_error("charvec builder: no records");
  }
  if(idx >= n) {
    throw std::runtime_error("charvec builder: assignment out of bounds");
  }
  if(!check_r_string_len(len)) {
    throw std::runtime_error("stored string length exceeds R string size");
  }
  const uint32_t stored_len = static_cast<uint32_t>(len);
  if(enc == charport_enc::CE_NA) {
    records[idx] = na_record();
    return nullptr;
  }
  if(stored_len == 0) {
    records[idx] = empty_record(enc);
    return const_cast<char*>(empty_data());
  }
  char * dest = shard_allocate_bytes(shard, stored_len, n);
  records[idx] = make_strview(dest, stored_len, enc);
  return dest;
}

inline void copy_record(charvec_shard & shard, charport_strview * records, size_t n,
                        size_t idx, const char * ptr, size_t len, charport_enc enc) {
  if(enc != charport_enc::CE_NA && ptr == nullptr && len > 0) {
    throw std::runtime_error("cannot assign non-NA null bytes");
  }
  char * dest = fill_record(shard, records, n, idx, len, enc);
  if(dest != nullptr && len > 0) {
    std::memcpy(dest, ptr, len);
  }
}

inline void copy_record(charvec_shard & shard, charport_strview * records, size_t n,
                        size_t idx, const charport_strview & value) {
  copy_record(shard, records, n, idx, value.ptr, static_cast<size_t>(value.len), value.enc);
}

struct charvec_data {
  char * slice_head;
  strview_array records;

  charvec_data() noexcept : slice_head(nullptr), records() {}
  explicit charvec_data(size_t len) : slice_head(nullptr), records(len) {}
  explicit charvec_data(strview_array && recs) noexcept
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
  const charport_strview & view(size_t idx) const { return records[idx]; }

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
    records = strview_array();
  }

  void compact() {
    rebuild_from(records);
  }

  char * reserve(size_t idx, size_t len, charport_enc enc) {
    if(!check_r_string_len(len)) {
      throw std::runtime_error("stored string length exceeds R string size");
    }
    const uint32_t stored_len = static_cast<uint32_t>(len);
    if(idx >= records.size()) {
      throw std::runtime_error("charvec store assignment out of bounds");
    }

    const charport_strview current = records[idx];
    if(enc == charport_enc::CE_NA) {
      records[idx] = na_record();
      return nullptr;
    }
    if(stored_len == 0) {
      records[idx] = empty_record(enc);
      return const_cast<char*>(empty_data());
    }
    if(!current.is_na() && current.len >= stored_len) {
      records[idx] = make_strview(current.ptr, stored_len, enc);
      return const_cast<char*>(current.ptr);
    }
    char * dest = push_slice(stored_len);
    records[idx] = make_strview(dest, stored_len, enc);
    return dest;
  }

  void assign(size_t idx, const char * ptr, size_t len, charport_enc enc) {
    if(enc != charport_enc::CE_NA && ptr == nullptr && len > 0) {
      throw std::runtime_error("cannot assign non-NA null bytes");
    }
    char * dest = reserve(idx, len, enc);
    if(dest != nullptr && len > 0) {
      std::memmove(dest, ptr, len);
    }
  }

  void assign(size_t idx, const charport_strview & value) {
    assign(idx, value.ptr, static_cast<size_t>(value.len), value.enc);
  }

  void rebuild_from(const strview_array & source) {
    std::vector<std::unique_ptr<char[]>> blocks;
    strview_array out(source.size());
    const size_t max_block = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
    size_t i = 0;

    while(i < source.size()) {
      size_t block_bytes = 0;
      size_t j = i;
      while(j < source.size()) {
        const charport_strview & rec = source[j];
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
          const charport_strview & rec = source[k];
          if(rec.is_na()) {
            continue;
          }
          if(rec.len == 0) {
            out[k] = empty_record(rec.enc);
            continue;
          }
          std::memcpy(write_ptr, rec.ptr, static_cast<size_t>(rec.len));
          out[k] = make_strview(write_ptr, rec.len, rec.enc);
          write_ptr += rec.len;
        }
        blocks.push_back(std::move(block));
      } else {
        for(size_t k = i; k < j; ++k) {
          const charport_strview & rec = source[k];
          if(!rec.is_na() && rec.len == 0) {
            out[k] = empty_record(rec.enc);
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
