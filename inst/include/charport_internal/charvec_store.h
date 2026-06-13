#ifndef CHARPORT_INTERNAL_CHARVEC_STORE_H
#define CHARPORT_INTERNAL_CHARVEC_STORE_H

#include "base.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

// charvec_data is the payload type for the charvec ALTREP class. A finished
// store holds exactly two things: ownership of its payload and its content.
//
//   slice_head -- head of an intrusive singly-linked chain of payload blocks
//                 (each block is `new char[slice_header_bytes() + capacity]`,
//                 its first slice_header_bytes() bytes hold the next pointer,
//                 the payload starts at slice_payload(block)). The store frees
//                 the chain in its destructor; blocks never move, so record
//                 pointers stay valid for the store's life.
//   records    -- a fixed-size owning array (strview_array): one
//                 charport_strview per element, the ABI element type, so the
//                 registered get_strview is `return records[i]`. Allocated once
//                 at the element count and assigned by index -- no capacity
//                 word, 16 bytes, no spare per vector (the large-list-of-small-
//                 vectors case).
//
// The store carries NO build/mutate bookkeeping (no allocated/dead byte counts,
// no current-slice cursor). All bump-allocation state lives in charvec_shard,
// the transient build context: every construction-by-index path (the Builder,
// subset, deserialize, the bulk C build) fills a shard and the store adopts the
// shard's chain by splice (charvec_data(strview_array&&, Shards&)), copying no
// payload. build_charvec() packages that pattern.
//
// Mutation (post-construction Set_elt):
//   char * reserve(size_t idx, size_t len, charport_enc enc)
//   void   assign(size_t idx, const char * ptr, size_t len, charport_enc enc)
//   A shrink or same-size rewrite reuses the record's bytes in place (pointer
//   stable). A grow, or a write over an NA record, takes a fresh dedicated
//   slice (push_slice) and repoints the record; the old bytes are simply left
//   unreferenced -- there is no dead-byte accounting and no automatic
//   compaction. Only the rewritten element's pointer changes; every other
//   record is untouched.
//
// Compaction (manual):
//   compact() walks records and rewrites all live payload into fresh exact-fit
//   blocks (each <= UINT32_MAX), then frees the old chain. It is stateless --
//   derived entirely from records -- and reclaims whatever grows/overwrites
//   left unreferenced. It MOVES POINTERS, the concrete example behind the
//   reader contract's "valid while alive and unmutated" condition. It is never
//   triggered automatically; callers invoke it when they want the space back.

namespace charport {
namespace internal {

constexpr size_t min_multi_string_slice_bytes() noexcept { return 64; }
constexpr size_t max_initial_slice_bytes() noexcept { return 16U << 10; }  // 16 KiB
constexpr size_t max_slice_bytes() noexcept { return 256U << 10; }         // 256 KiB
constexpr size_t slice_alignment() noexcept { return 64; }
constexpr size_t vector_len_scale() noexcept { return 4; }

// Intrusive slice chain: each block reserves its first slice_header_bytes() for
// the next-block pointer; the payload follows. The link is read/written through
// memcpy so it is alignment- and aliasing-clean.
constexpr size_t slice_header_bytes() noexcept { return sizeof(char *); }

inline char * slice_next(char * block) noexcept {
  char * nxt;
  std::memcpy(&nxt, block, sizeof(nxt));
  return nxt;
}

inline void slice_set_next(char * block, char * nxt) noexcept {
  std::memcpy(block, &nxt, sizeof(nxt));
}

inline char * slice_payload(char * block) noexcept {
  return block + slice_header_bytes();
}

// Walk an intrusive slice chain, freeing every block. Safe on nullptr.
inline void free_slice_chain(char * head) noexcept {
  while(head != nullptr) {
    char * next = slice_next(head);
    delete[] head;
    head = next;
  }
}

// shared pointer target for all zero-length strings
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

// Fixed-size owning array of records. Allocated once at the store's element
// count and only assigned by index -- no capacity word, no growth -- so it is
// 16 bytes against std::vector's 24 and carries no spare bytes per vector. The
// read API mirrors the std::vector slice the store used to expose, so call
// sites stay `records.size()` / `records[i]` / range-for.
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

// Transient build context: owns a private slice chain and bump-allocates into
// it, pointing at a shared records array it does not own. Construction by index
// goes through one (serial) or several (parallel, disjoint ranges) of these,
// and charvec_data adopts the chains by splice. Each record index must be
// written by exactly one shard, exactly once.
struct charvec_shard {
  char * slice_head;                 // intrusive chain head == current bump block
  charport_strview * records;        // non-owning, shared across shards
  size_t n_records;
  size_t allocated_bytes;
  uint32_t current_slice_used;
  uint32_t current_slice_capacity;

  charvec_shard() noexcept :
    slice_head(nullptr),
    records(nullptr),
    n_records(0),
    allocated_bytes(0),
    current_slice_used(0),
    current_slice_capacity(0) {}

  charvec_shard(charport_strview * records, size_t n) noexcept :
    slice_head(nullptr),
    records(records),
    n_records(n),
    allocated_bytes(0),
    current_slice_used(0),
    current_slice_capacity(0) {}

  charvec_shard(const charvec_shard &) = delete;
  charvec_shard & operator=(const charvec_shard &) = delete;

  charvec_shard(charvec_shard && other) noexcept :
    slice_head(other.slice_head),
    records(other.records),
    n_records(other.n_records),
    allocated_bytes(other.allocated_bytes),
    current_slice_used(other.current_slice_used),
    current_slice_capacity(other.current_slice_capacity) {
    other.slice_head = nullptr;
  }

  charvec_shard & operator=(charvec_shard && other) noexcept {
    if(this != &other) {
      free_slice_chain(slice_head);
      slice_head = other.slice_head;
      records = other.records;
      n_records = other.n_records;
      allocated_bytes = other.allocated_bytes;
      current_slice_used = other.current_slice_used;
      current_slice_capacity = other.current_slice_capacity;
      other.slice_head = nullptr;
    }
    return *this;
  }

  ~charvec_shard() { free_slice_chain(slice_head); }

  // Tail slices grow using allocated_bytes / 2 as a conservative hint, rounded
  // up to slice_alignment and capped at max_slice_bytes; a string larger than
  // the growth target gets a slice at least as large as itself. The first slice
  // is sized from the element count (initial_slice_heuristic).
  size_t next_regular_slice_size() const noexcept {
    const size_t initial = initial_slice_heuristic(n_records);
    const size_t grown = round_up(std::max(initial, allocated_bytes / 2), slice_alignment());
    return std::min(grown, max_slice_bytes());
  }

  void allocate_slice(uint32_t capacity) {
    char * block = new char[slice_header_bytes() + capacity];
    slice_set_next(block, slice_head);  // prepend; head == current bump block
    slice_head = block;
    current_slice_used = 0;
    current_slice_capacity = capacity;
  }

  char * allocate_bytes(uint32_t len) {
    if(len == 0) {
      return const_cast<char*>(empty_data());
    }
    if(slice_head == nullptr || current_slice_capacity - current_slice_used < len) {
      const size_t next_size = round_up(std::max(next_regular_slice_size(), static_cast<size_t>(len)), slice_alignment());
      allocate_slice(checked_u32(next_size, "slice size"));
    }
    char * dest = slice_payload(slice_head) + current_slice_used;
    current_slice_used += len;
    allocated_bytes += static_cast<size_t>(len);
    return dest;
  }

  char * reserve(size_t idx, size_t len, charport_enc enc) {
    if(records == nullptr) {
      throw std::runtime_error("charvec_shard has no records");
    }
    if(idx >= n_records) {
      throw std::runtime_error("charvec_shard assignment out of bounds");
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
    char * dest = allocate_bytes(stored_len);
    records[idx] = make_strview(dest, stored_len, enc);
    return dest;
  }

  void assign(size_t idx, const char * ptr, size_t len, charport_enc enc) {
    if(enc != charport_enc::CE_NA && ptr == nullptr && len > 0) {
      throw std::runtime_error("cannot assign non-NA null bytes");
    }
    char * dest = reserve(idx, len, enc);
    if(dest != nullptr && len > 0) {
      std::memcpy(dest, ptr, len);
    }
  }

  void assign(size_t idx, const charport_strview & value) {
    assign(idx, value.ptr, static_cast<size_t>(value.len), value.enc);
  }
};

struct charvec_data {
  char * slice_head;                 // intrusive chain head (owned; freed in dtor)
  strview_array records;

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

  // Allocate one dedicated block holding exactly `len` payload bytes, prepend it
  // to the chain, return its payload. No bump state: each call is its own slice.
  // Used by the grow path of reserve(), the copy ctor, and compact().
  char * push_slice(uint32_t len) {
    char * block = new char[slice_header_bytes() + static_cast<size_t>(len)];
    slice_set_next(block, slice_head);
    slice_head = block;
    return slice_payload(block);
  }

  // Replace slice_head + records with a fresh exact-fit rebuild of `source`:
  // live payload packed into blocks (each <= UINT32_MAX), NA/empty records
  // reproduced. The new blocks are fully populated from `source` before the old
  // chain is freed, so it is safe for `source` to be this->records (compact) or
  // another store's records (copy). Strong exception guarantee: a failed
  // allocation leaves *this unchanged.
  void rebuild_from(const strview_array & source) {
    std::vector<std::unique_ptr<char[]>> blocks;
    strview_array out(source.size());  // all NA
    const size_t max_block = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
    size_t i = 0;
    while(i < source.size()) {
      size_t block_bytes = 0;
      size_t j = i;
      while(j < source.size()) {
        const charport_strview & rec = source[j];
        const size_t l = rec.is_na() ? 0 : static_cast<size_t>(rec.len);
        if(l > max_block - block_bytes) {
          break;  // close the block before this record
        }
        block_bytes += l;
        ++j;
      }
      if(block_bytes > 0) {
        std::unique_ptr<char[]> block(new char[slice_header_bytes() + block_bytes]);
        char * write_ptr = slice_payload(block.get());
        for(size_t k = i; k < j; ++k) {
          const charport_strview & rec = source[k];
          if(rec.is_na()) {
            continue;  // out[k] already NA
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
      i = (j > i) ? j : i + 1;  // first record always fits, so j > i for i < size
    }
    // commit: the old payload has been fully copied out; install the new chain
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

  charvec_data() noexcept : slice_head(nullptr), records() {}

  explicit charvec_data(size_t len) : slice_head(nullptr), records(len) {}

  // Copy is an exact-fit rebuild of the other store's live payload.
  charvec_data(const charvec_data & other) : slice_head(nullptr), records() {
    rebuild_from(other.records);
  }

  // Merge build-context output: adopt every shard's slice chain without copying
  // payload. Order is irrelevant -- records already point into the blocks, and
  // the finished store does no further bump allocation -- so the chains are
  // simply spliced and the shards emptied (their destructors must not free what
  // we now own).
  template<typename Shards>
  charvec_data(strview_array && source_records, Shards & shards) :
    slice_head(nullptr),
    records(std::move(source_records)) {
    char * tail = nullptr;
    for(charvec_shard & shard : shards) {
      char * head = shard.slice_head;
      if(head == nullptr) {
        continue;
      }
      shard.slice_head = nullptr;  // donated
      if(slice_head == nullptr) {
        slice_head = head;
      } else {
        slice_set_next(tail, head);
      }
      char * block = head;
      while(slice_next(block) != nullptr) {
        block = slice_next(block);
      }
      tail = block;
    }
  }

  charvec_data(charvec_data && other) noexcept :
    slice_head(other.slice_head),
    records(std::move(other.records)) {
    other.slice_head = nullptr;
  }

  charvec_data & operator=(const charvec_data & other) {
    if(this == &other) {
      return *this;
    }
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

  size_t size() const noexcept {
    return records.size();
  }

  const charport_strview & view(size_t idx) const {
    return records[idx];
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
      // shrink or same size: rewrite in place, pointer stable
      records[idx] = make_strview(current.ptr, stored_len, enc);
      return const_cast<char*>(current.ptr);
    }
    // grow, or overwrite of an NA record: fresh dedicated slice; any old bytes
    // are left unreferenced until a manual compact()
    char * dest = push_slice(stored_len);
    records[idx] = make_strview(dest, stored_len, enc);
    return dest;
  }

  // memmove, not memcpy: an in-place shrink may be fed its own bytes. The grow
  // path writes into a fresh slice and never moves existing payload, so a source
  // pointing into this store stays valid.
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
};

// Build a charvec_data of length n through a transient single shard (bump-packed
// slice chain), then merge -- so every fill-by-index construction path funnels
// through a shard rather than allocating into the store one dedicated slice per
// element. `fill` receives the shard and writes each index (via assign /
// assign_charsxp). Out-of-range or never-written indices stay NA.
template<typename Fill>
inline std::unique_ptr<charvec_data> build_charvec(size_t n, Fill && fill) {
  strview_array records(n);
  charvec_shard ctx(records.data(), records.size());
  fill(ctx);
  std::array<charvec_shard, 1> shards{{std::move(ctx)}};
  // qualified: an unqualified call would pull std::make_unique in by ADL on the
  // std::array argument and clash with this namespace's make_unique
  return charport::internal::make_unique<charvec_data>(std::move(records), shards);
}

} // namespace internal
} // namespace charport

#endif
