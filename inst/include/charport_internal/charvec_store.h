#ifndef CHARPORT_INTERNAL_CHARVEC_STORE_H
#define CHARPORT_INTERNAL_CHARVEC_STORE_H

#include "base.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

// charvec_data is the payload type for the charvec ALTREP class.
//
// Records are strview-shaped: storage is a `records` array (one
// charport_strview per element -- the record type IS the ABI element type, so
// the registered get_strview is `return records[i]`). It is a fixed-size owning
// array (strview_array): allocated once at the element count and only assigned
// by index, so it carries no capacity word -- 16 bytes, no spare per vector
// (the large-list-of-small-vectors case). Payload lives in `slices`: stable
// memory blocks, bump-allocated, geometric growth capped per block. Blocks
// never move on append, so record pointers are stable under growth.
//
// Slices are an intrusive singly-linked chain rather than a vector of owning
// pointers: each block is `new char[slice_header_bytes() + capacity]`, its
// first slice_header_bytes() bytes hold the next-block pointer and the payload
// starts at slice_payload(block). The store keeps only an 8-byte head, which is
// also the current bump block (head == current invariant: new blocks prepend).
// This costs manual lifetime management (free_slices in the destructor, the
// hand-written moves below) in exchange for one fewer allocation per vector and
// O(1) slice bookkeeping regardless of block count.
//
// charvec_shard is for multi-threaded construction (the caller brings the
// threading framework): shards point at one shared records array but own
// private slice chains, spliced in (no payload copy) by the
// charvec_data(strview_array&&, Shards&) constructor. Each record index must be
// written by exactly one shard, exactly once.
//
// Slice size growth:
//   The initial slice size depends on the initialized vector length.
//   For vectors of length 0 or 1 allocation is deferred until assign()
//   actually needs bytes. For length >= 2 a small pooled initial slice:
//     min(max(64, next_power_of_two(len * 4)), max_initial_slice_bytes)
//   The charvec_data(size_t, size_t) overload overrides this heuristic.
//   Subsequent tail slices grow using allocated_bytes / 2 as a conservative
//   hint, rounded up to slice_alignment and capped at max_slice_bytes; a
//   single string larger than the growth target gets a slice at least as
//   large as itself.
//
// Mutation and dead-byte accounting:
//   char * reserve(size_t idx, size_t len, charport_enc enc)
//   void   assign(size_t idx, const char * ptr, size_t len, charport_enc enc)
//   Shrinking assignments rewrite in place (the shrink slack is counted as
//   dead bytes); growing ones relocate to the tail and the old bytes count as
//   dead. Replacing a live record with NA or "" also retires its bytes.
//   Invariant: allocated_bytes - dead_bytes == sum(records[i].len).
//   Counting shrink slack and NA/empty overwrites keeps that identity exact,
//   so compaction triggers when it should and reclaims exactly what the
//   accounting promises.
//
// Compaction:
//   When dead_bytes >= compact_dead_threshold AND dead_bytes >= half of live
//   bytes, compact() rewrites live payload into exact-fit blocks (each <=
//   UINT32_MAX) and resets dead_bytes to zero. Compaction MOVES POINTERS --
//   this is the concrete example behind the reader contract's "valid while
//   alive and unmutated" condition.

namespace charport {
namespace internal {

constexpr size_t min_multi_string_slice_bytes() noexcept { return 64; }
constexpr size_t max_initial_slice_bytes() noexcept { return 16U << 10; }  // 16 KiB
constexpr size_t max_slice_bytes() noexcept { return 256U << 10; }         // 256 KiB
constexpr size_t compact_dead_threshold() noexcept { return 1U << 20; }    // 1 MiB
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
};

struct charvec_data {
  char * slice_head;                 // intrusive chain head == current bump block
  strview_array records;
  size_t allocated_bytes;
  size_t dead_bytes;
  uint32_t current_slice_used;
  uint32_t current_slice_capacity;

  static size_t normalize_initial_slice_size(size_t value) {
    if(value == 0) {
      return 0;
    }
    return static_cast<size_t>(
      checked_u32(round_up(value, slice_alignment()), "initial slice size")
    );
  }

  size_t next_regular_slice_size() const noexcept {
    const size_t initial = initial_slice_heuristic(records.size());
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
    if(slice_head == nullptr) {
      const size_t next_size = round_up(
        std::max(initial_slice_heuristic(records.size()), static_cast<size_t>(len)),
        slice_alignment()
      );
      allocate_slice(checked_u32(next_size, "slice size"));
    } else if(current_slice_capacity - current_slice_used < len) {
      const size_t next_size = round_up(std::max(next_regular_slice_size(), static_cast<size_t>(len)), slice_alignment());
      allocate_slice(checked_u32(next_size, "slice size"));
    }
    char * dest = slice_payload(slice_head) + current_slice_used;
    current_slice_used += len;
    allocated_bytes += static_cast<size_t>(len);
    return dest;
  }

  inline bool should_compact() const noexcept {
    if(dead_bytes < compact_dead_threshold()) {
      return false;
    }
    if(allocated_bytes <= dead_bytes) {
      return false;
    }
    const size_t live = allocated_bytes - dead_bytes;
    return dead_bytes >= ((live + 1) / 2);
  }

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

  charvec_data() noexcept :
    slice_head(nullptr),
    records(),
    allocated_bytes(0),
    dead_bytes(0),
    current_slice_used(0),
    current_slice_capacity(0) {}

  explicit charvec_data(size_t len) : charvec_data() {
    records = strview_array(len);
  }

  // initial_slice_size > 0 preallocates the first slice (rounded up to
  // slice_alignment); 0 defers to the first-write heuristic. The hint is
  // honored here and deliberately not stored: the store is per-vector
  // overhead (think a large list of small string vectors), so it carries
  // no field whose only job is remembering a construction argument.
  charvec_data(size_t len, size_t initial_slice_size) : charvec_data(len) {
    const size_t initial = normalize_initial_slice_size(initial_slice_size);
    if(initial > 0) {
      allocate_slice(static_cast<uint32_t>(initial));
    }
  }

  // Copy lands exact-fit: the live payload size is known, so pre-allocate one
  // slice for all of it instead of replaying the growth heuristic record by
  // record (a compaction for free; falls back to growth in the >4 GiB case).
  charvec_data(const charvec_data & other) : charvec_data() {
    records = strview_array(other.records.size());
    size_t live = 0;
    for(const auto & rec : other.records) {
      if(!rec.is_na()) {
        live += static_cast<size_t>(rec.len);
      }
    }
    if(live > 0 && live <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - (slice_alignment() - 1)) {
      allocate_slice(checked_u32(round_up(live, slice_alignment()), "slice size"));
    }
    for(size_t i = 0; i < other.records.size(); ++i) {
      const auto & rec = other.records[i];
      if(rec.is_na()) {
        continue;  // records already initialized to NA
      }
      if(rec.len == 0) {
        records[i] = empty_record(rec.enc);
        continue;
      }
      char * dest = allocate_bytes(rec.len);
      std::memcpy(dest, rec.ptr, static_cast<size_t>(rec.len));
      records[i] = make_strview(dest, rec.len, rec.enc);
    }
  }

  // Merge sharded construction output: take ownership of every shard's slice
  // chain without copying payload. The shard with the most remaining tail
  // capacity becomes the merged head (head == current bump block invariant) so
  // the store keeps bumping into the roomiest tail; the rest are spliced after.
  template<typename Shards>
  charvec_data(strview_array && source_records, Shards & shards) :
    slice_head(nullptr),
    records(std::move(source_records)),
    allocated_bytes(0),
    dead_bytes(0),
    current_slice_used(0),
    current_slice_capacity(0) {
    charvec_shard * best = nullptr;
    uint32_t best_remaining = 0;
    for(auto & shard : shards) {
      if(shard.slice_head == nullptr) {
        continue;  // empty/NA-only shards contribute no bytes
      }
      allocated_bytes += shard.allocated_bytes;
      const uint32_t remaining = shard.current_slice_capacity - shard.current_slice_used;
      if(best == nullptr || remaining > best_remaining) {
        best = &shard;
        best_remaining = remaining;
      }
    }
    if(best != nullptr) {
      current_slice_used = best->current_slice_used;
      current_slice_capacity = best->current_slice_capacity;
    }

    // concatenate: best chain first (its head is our head/current block), then
    // the rest; each shard donates its chain and is emptied so its destructor
    // does not free what we now own.
    char * tail = nullptr;
    auto splice = [&](charvec_shard & shard) {
      char * head = shard.slice_head;
      if(head == nullptr) {
        return;
      }
      shard.slice_head = nullptr;
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
    };
    if(best != nullptr) {
      splice(*best);
    }
    for(auto & shard : shards) {
      if(&shard == best) {
        continue;
      }
      splice(shard);
    }
  }

  charvec_data(charvec_data && other) noexcept :
    slice_head(other.slice_head),
    records(std::move(other.records)),
    allocated_bytes(other.allocated_bytes),
    dead_bytes(other.dead_bytes),
    current_slice_used(other.current_slice_used),
    current_slice_capacity(other.current_slice_capacity) {
    other.slice_head = nullptr;
    other.allocated_bytes = 0;
    other.dead_bytes = 0;
    other.current_slice_used = 0;
    other.current_slice_capacity = 0;
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
      allocated_bytes = other.allocated_bytes;
      dead_bytes = other.dead_bytes;
      current_slice_used = other.current_slice_used;
      current_slice_capacity = other.current_slice_capacity;
      other.slice_head = nullptr;
      other.allocated_bytes = 0;
      other.dead_bytes = 0;
      other.current_slice_used = 0;
      other.current_slice_capacity = 0;
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
    allocated_bytes = 0;
    dead_bytes = 0;
    current_slice_used = 0;
    current_slice_capacity = 0;
  }

  void compact() {
    if(dead_bytes == 0) {
      return;
    }
    if(dead_bytes > allocated_bytes) {
      throw std::runtime_error("charvec store accounting corrupted (dead_bytes > allocated_bytes)");
    }
    if(allocated_bytes == dead_bytes) {
      // no live payload remains: only NA and zero-length records exist
      for(auto & rec : records) {
        if(!rec.is_na() && rec.len > 0) {
          throw std::runtime_error("charvec store accounting corrupted (live record with all bytes dead)");
        }
      }
      free_slices();
      allocated_bytes = 0;
      dead_bytes = 0;
      current_slice_used = 0;
      current_slice_capacity = 0;
      return;
    }

    strview_array compacted_records(records);
    // exception-safe staging: blocks stay RAII-owned until the whole rewrite
    // succeeds, then they are released into the intrusive chain
    std::vector<std::unique_ptr<char[]>> blocks;
    uint32_t last_block_size = 0;
    size_t total_emitted = 0;
    const size_t max_block_bytes = static_cast<size_t>(std::numeric_limits<uint32_t>::max());

    auto emit_block = [&](size_t start, size_t end, size_t block_size) {
      if(block_size == 0) {
        return;
      }
      uint32_t compacted_size = checked_u32(block_size, "compacted slice size");
      std::unique_ptr<char[]> block(new char[slice_header_bytes() + compacted_size]);
      char * write_ptr = slice_payload(block.get());
      char * block_start = write_ptr;
      for(size_t i = start; i < end; ++i) {
        const auto & rec = records[i];
        if(rec.is_na() || rec.len == 0) {
          continue;
        }
        std::memcpy(write_ptr, rec.ptr, static_cast<size_t>(rec.len));
        compacted_records[i] = make_strview(write_ptr, rec.len, rec.enc);
        write_ptr += rec.len;
      }
      if(static_cast<size_t>(write_ptr - block_start) != block_size) {
        throw std::runtime_error("charvec store compact size mismatch");
      }
      blocks.push_back(std::move(block));
      last_block_size = compacted_size;
      total_emitted += block_size;
    };

    size_t block_start = 0;
    size_t block_end = 0;
    size_t block_size = 0;
    while(block_end < records.size()) {
      const auto & rec = records[block_end];
      const size_t next_size = rec.is_na() ? 0 : static_cast<size_t>(rec.len);
      if(next_size > max_block_bytes - block_size) {
        emit_block(block_start, block_end, block_size);
        block_start = block_end;
        block_size = 0;
        continue;
      }
      block_size += next_size;
      ++block_end;
    }
    emit_block(block_start, block_end, block_size);

    // commit: release the staged blocks into the chain. Prepending in emit
    // order leaves the last-emitted block at the head -- the current bump block.
    free_slices();
    char * head = nullptr;
    for(auto & block : blocks) {
      char * raw = block.release();
      slice_set_next(raw, head);
      head = raw;
    }
    slice_head = head;
    records = std::move(compacted_records);
    // total_emitted == sum(records[i].len): exact by construction, where the
    // pre-compaction accounting identity could only promise it indirectly
    allocated_bytes = total_emitted;
    dead_bytes = 0;
    if(last_block_size == 0) {
      current_slice_used = 0;
      current_slice_capacity = 0;
    } else {
      current_slice_used = last_block_size;
      current_slice_capacity = last_block_size;
    }
  }

  char * reserve(size_t idx, size_t len, charport_enc enc) {
    if(!check_r_string_len(len)) {
      throw std::runtime_error("stored string length exceeds R string size");
    }
    const uint32_t stored_len = static_cast<uint32_t>(len);
    if(idx >= records.size()) {
      throw std::runtime_error("charvec store assignment out of bounds");
    }

    charport_strview current = records[idx];
    if(enc == charport_enc::CE_NA) {
      dead_bytes += static_cast<size_t>(current.len);  // 0 for NA/empty records
      records[idx] = na_record();
      return nullptr;
    }
    if(stored_len == 0) {
      dead_bytes += static_cast<size_t>(current.len);
      records[idx] = empty_record(enc);
      return const_cast<char*>(empty_data());
    }
    if(!current.is_na()) {
      if(current.len >= stored_len) {
        // in-place rewrite; the shrink slack is dead
        dead_bytes += static_cast<size_t>(current.len - stored_len);
        records[idx] = make_strview(current.ptr, stored_len, enc);
        return const_cast<char*>(current.ptr);
      }
      if(should_compact()) {
        compact();
        current = records[idx];  // compaction moved it
      }
      dead_bytes += static_cast<size_t>(current.len);
    }
    char * dest = allocate_bytes(stored_len);
    records[idx] = make_strview(dest, stored_len, enc);
    return dest;
  }

  // Safe for self-sourced bytes (ptr pointing into this store's own
  // slices): the in-place shrink path uses memmove, and any write that
  // could trigger compaction -- which moves payload -- stages the source
  // bytes through a temporary first. Staging only happens while the store
  // is compaction-eligible (>= 1 MiB dead), so the common path stays a
  // single copy.
  void assign(size_t idx, const char * ptr, size_t len, charport_enc enc) {
    if(enc != charport_enc::CE_NA && ptr == nullptr && len > 0) {
      throw std::runtime_error("cannot assign non-NA null bytes");
    }
    if(ptr != nullptr && len > 0 && enc != charport_enc::CE_NA && should_compact()) {
      std::unique_ptr<char[]> staged(new char[len]);
      std::memcpy(staged.get(), ptr, len);
      char * dest = reserve(idx, len, enc);
      if(dest != nullptr) {
        std::memcpy(dest, staged.get(), len);
      }
      return;
    }
    char * dest = reserve(idx, len, enc);
    if(dest != nullptr && len > 0) {
      // memmove, not memcpy: an in-place shrink may be fed its own bytes
      std::memmove(dest, ptr, len);
    }
  }

  void assign(size_t idx, const charport_strview & value) {
    assign(idx, value.ptr, static_cast<size_t>(value.len), value.enc);
  }
};

} // namespace internal
} // namespace charport

#endif
