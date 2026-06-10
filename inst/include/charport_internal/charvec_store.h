#ifndef CHARPORT_INTERNAL_CHARVEC_STORE_H
#define CHARPORT_INTERNAL_CHARVEC_STORE_H

#include "base.h"
#include <memory>
#include <vector>

// charvec_data is the payload type for the charvec ALTREP class.
//
// Records are strview-shaped: storage is a `records` array (one
// charport_strview per element -- the record type IS the ABI element type, so
// the registered get_strview is `return records[i]`) plus `slices`: stable
// memory blocks, bump-allocated, geometric growth capped per block. Blocks
// never move on append, so record pointers are stable under growth.
//
// charvec_shard is for multi-threaded construction (the caller brings the
// threading framework): shards share one records vector but own private
// slices, merged by block-move via the
// charvec_data(std::vector<charport_strview>&&, Shards&) constructor. Each
// record index must be written by exactly one shard, exactly once.
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

struct charvec_shard {
  std::vector<std::unique_ptr<char[]>> slices;
  std::vector<charport_strview> * records;
  size_t allocated_bytes;
  uint32_t current_slice_used;
  uint32_t current_slice_capacity;

  charvec_shard() :
    slices(),
    records(nullptr),
    allocated_bytes(0),
    current_slice_used(0),
    current_slice_capacity(0) {}

  explicit charvec_shard(std::vector<charport_strview> & records) :
    slices(),
    records(&records),
    allocated_bytes(0),
    current_slice_used(0),
    current_slice_capacity(0) {}

  charvec_shard(const charvec_shard &) = delete;
  charvec_shard & operator=(const charvec_shard &) = delete;
  charvec_shard(charvec_shard &&) noexcept = default;
  charvec_shard & operator=(charvec_shard &&) noexcept = default;

  size_t next_regular_slice_size() const noexcept {
    const size_t initial = initial_slice_heuristic(records == nullptr ? 0 : records->size());
    const size_t grown = round_up(std::max(initial, allocated_bytes / 2), slice_alignment());
    return std::min(grown, max_slice_bytes());
  }

  void allocate_slice(uint32_t capacity) {
    slices.emplace_back(new char[capacity]);
    current_slice_used = 0;
    current_slice_capacity = capacity;
  }

  char * allocate_bytes(uint32_t len) {
    if(len == 0) {
      return const_cast<char*>(empty_data());
    }
    if(slices.empty() || current_slice_capacity - current_slice_used < len) {
      const size_t next_size = round_up(std::max(next_regular_slice_size(), static_cast<size_t>(len)), slice_alignment());
      allocate_slice(checked_u32(next_size, "slice size"));
    }
    char * dest = slices.back().get() + current_slice_used;
    current_slice_used += len;
    allocated_bytes += static_cast<size_t>(len);
    return dest;
  }

  char * reserve(size_t idx, size_t len, charport_enc enc) {
    if(records == nullptr) {
      throw std::runtime_error("charvec_shard has no records");
    }
    if(idx >= records->size()) {
      throw std::runtime_error("charvec_shard assignment out of bounds");
    }
    if(!check_r_string_len(len)) {
      throw std::runtime_error("stored string length exceeds R string size");
    }
    const uint32_t stored_len = static_cast<uint32_t>(len);
    if(enc == charport_enc::CE_NA) {
      (*records)[idx] = na_record();
      return nullptr;
    }
    if(stored_len == 0) {
      (*records)[idx] = empty_record(enc);
      return const_cast<char*>(empty_data());
    }
    char * dest = allocate_bytes(stored_len);
    (*records)[idx] = make_strview(dest, stored_len, enc);
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
  std::vector<std::unique_ptr<char[]>> slices;
  std::vector<charport_strview> records;
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
    slices.emplace_back(new char[capacity]);
    current_slice_used = 0;
    current_slice_capacity = capacity;
  }

  char * allocate_bytes(uint32_t len) {
    if(len == 0) {
      return const_cast<char*>(empty_data());
    }
    if(slices.empty()) {
      const size_t next_size = round_up(
        std::max(initial_slice_heuristic(records.size()), static_cast<size_t>(len)),
        slice_alignment()
      );
      allocate_slice(checked_u32(next_size, "slice size"));
    } else if(current_slice_capacity - current_slice_used < len) {
      const size_t next_size = round_up(std::max(next_regular_slice_size(), static_cast<size_t>(len)), slice_alignment());
      allocate_slice(checked_u32(next_size, "slice size"));
    }
    char * dest = slices.back().get() + current_slice_used;
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

  charvec_data() :
    slices(),
    records(),
    allocated_bytes(0),
    dead_bytes(0),
    current_slice_used(0),
    current_slice_capacity(0) {}

  explicit charvec_data(size_t len) : charvec_data() {
    records.resize(len, na_record());
  }

  // initial_slice_size > 0 preallocates the first slice (rounded up to
  // slice_alignment); 0 defers to the first-write heuristic. The hint is
  // honored here and deliberately not stored: the store is per-vector
  // overhead (think a large list of small string vectors), so it carries
  // no field whose only job is remembering a construction argument.
  charvec_data(size_t len, size_t initial_slice_size) : charvec_data() {
    records.resize(len, na_record());
    const size_t initial = normalize_initial_slice_size(initial_slice_size);
    if(initial > 0) {
      allocate_slice(static_cast<uint32_t>(initial));
    }
  }

  // Copy lands exact-fit: the live payload size is known, so pre-allocate one
  // slice for all of it instead of replaying the growth heuristic record by
  // record (a compaction for free; falls back to growth in the >4 GiB case).
  charvec_data(const charvec_data & other) : charvec_data() {
    records.resize(other.records.size(), na_record());
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
  // blocks without copying payload. The shard with the most remaining tail
  // capacity is appended last so the merged store keeps the roomiest tail.
  template<typename Shards>
  charvec_data(std::vector<charport_strview> && source_records, Shards & shards) :
    slices(),
    records(std::move(source_records)),
    allocated_bytes(0),
    dead_bytes(0),
    current_slice_used(0),
    current_slice_capacity(0) {
    bool have_best = false;
    uint32_t best_remaining = 0;
    const void * best_shard = nullptr;
    for(auto & shard : shards) {
      if(shard.slices.empty()) {
        continue;
      }
      const uint32_t remaining = shard.current_slice_capacity - shard.current_slice_used;
      if(!have_best || remaining > best_remaining) {
        have_best = true;
        best_remaining = remaining;
        best_shard = static_cast<const void*>(&shard);
      }
    }

    auto append_shard = [&](charvec_shard & shard) {
      if(shard.slices.empty()) {
        return;
      }
      allocated_bytes += shard.allocated_bytes;
      for(auto & slice : shard.slices) {
        slices.push_back(std::move(slice));
      }
      current_slice_used = shard.current_slice_used;
      current_slice_capacity = shard.current_slice_capacity;
    };

    for(auto & shard : shards) {
      if(have_best && static_cast<const void*>(&shard) == best_shard) {
        continue;
      }
      append_shard(shard);
    }
    if(have_best) {
      for(auto & shard : shards) {
        if(static_cast<const void*>(&shard) == best_shard) {
          append_shard(shard);
          break;
        }
      }
    } else {
      current_slice_used = 0;
      current_slice_capacity = 0;
    }
  }

  charvec_data(charvec_data &&) noexcept = default;

  charvec_data & operator=(const charvec_data & other) {
    if(this == &other) {
      return *this;
    }
    charvec_data tmp(other);
    *this = std::move(tmp);
    return *this;
  }

  charvec_data & operator=(charvec_data &&) noexcept = default;

  size_t size() const noexcept {
    return records.size();
  }

  const charport_strview & view(size_t idx) const {
    return records[idx];
  }

  void reserve_records(size_t n) {
    records.reserve(n);
  }

  void resize(size_t n) {
    // shrinking drops records without retiring their bytes; grow-only in practice
    records.resize(n, na_record());
  }

  void clear() {
    slices.clear();
    records.clear();
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
      slices.clear();
      allocated_bytes = 0;
      dead_bytes = 0;
      current_slice_used = 0;
      current_slice_capacity = 0;
      return;
    }

    std::vector<charport_strview> compacted_records(records);
    std::vector<std::unique_ptr<char[]>> compacted_slices;
    uint32_t last_block_size = 0;
    size_t total_emitted = 0;
    const size_t max_block_bytes = static_cast<size_t>(std::numeric_limits<uint32_t>::max());

    auto emit_block = [&](size_t start, size_t end, size_t block_size) {
      if(block_size == 0) {
        return;
      }
      uint32_t compacted_size = checked_u32(block_size, "compacted slice size");
      std::unique_ptr<char[]> slice(new char[compacted_size]);
      char * write_ptr = slice.get();
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
      compacted_slices.push_back(std::move(slice));
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

    slices = std::move(compacted_slices);
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

  void append(const char * ptr, size_t len, charport_enc enc) {
    if(!check_r_string_len(len)) {
      throw std::runtime_error("stored string length exceeds R string size");
    }
    const uint32_t stored_len = static_cast<uint32_t>(len);
    if(enc == charport_enc::CE_NA || ptr == nullptr) {
      records.emplace_back(na_record());
      return;
    }
    if(stored_len == 0) {
      records.emplace_back(empty_record(enc));
      return;
    }
    char * dest = allocate_bytes(stored_len);
    std::memcpy(dest, ptr, static_cast<size_t>(stored_len));
    records.emplace_back(make_strview(dest, stored_len, enc));
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
