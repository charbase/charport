#ifndef CHARPORT_CHARPACK_ALTREP_H
#define CHARPORT_CHARPACK_ALTREP_H

#include "charvec_altrep.h"

#include <cstdint>
#include <cstring>
#include <limits>

// data1 is one RAWSXP in native byte order:
//
//   [u64 head][end offset x n][u8 cetype_ext x n][bytes]
//
// head holds n in its low 63 bits. Bit 63 clear means u32 end offsets, set
// means u64; u64 is used only when the payload exceeds 2^32 - 1 bytes.
// Record i spans bytes [end[i-1], end[i]), with end[-1] = 0, so any record is
// found in constant time. NA (CETYPE_EXT_NA) and "" span zero bytes. The
// payload is end[n-1] bytes (0 when n is 0) and data1 ends exactly there.
//
// The serialized state is not data1 but charvec's packed body,
// [u64 n][u32 size x n][u8 cetype_ext x n][bytes] in little-endian order, so
// the in-memory layout is free to change. Its parser rejects a count with bit
// 63 set, which leaves that bit to mark a future format.
//
// States:
//   unfilled      data1 and data2 nil; length 0
//   filled        data1 is the RAWSXP, immutable once installed
//   materialized  data2 is a STRSXP; data1 nil
//
// A fill moves unfilled to filled (fill_materialized_na: to materialized).
// Reads and duplicates never yield an unfilled vector.

namespace charpack_layout {

constexpr size_t header_bytes = sizeof(uint64_t);
constexpr uint64_t wide_bit = uint64_t(1) << 63;

inline size_t offset_bytes(bool wide) noexcept {
  return wide ? sizeof(uint64_t) : sizeof(uint32_t);
}

inline bool needs_wide(size_t payload) noexcept {
  return static_cast<uint64_t>(payload) > std::numeric_limits<uint32_t>::max();
}

// Bytes of a data1 holding n records and payload bytes, or 0 when that
// exceeds R's maximum vector length.
inline size_t total_bytes(size_t n, size_t payload, bool wide) noexcept {
  const size_t max = static_cast<size_t>(R_XLEN_T_MAX);
  const size_t per_record = offset_bytes(wide) + 1;
  if(n > (max - header_bytes) / per_record) {
    return 0;
  }
  const size_t meta = header_bytes + n * per_record;
  return payload > max - meta ? 0 : meta + payload;
}

struct view {
  size_t n;
  bool wide;
  const unsigned char * ends;
  const unsigned char * tags;
  const char * bytes;
};

inline view parse(const unsigned char * raw) noexcept {
  uint64_t head;
  std::memcpy(&head, raw, sizeof(head));
  view v;
  v.wide = (head & wide_bit) != 0;
  v.n = static_cast<size_t>(head & ~wide_bit);
  v.ends = raw + header_bytes;
  v.tags = v.ends + offset_bytes(v.wide) * v.n;
  v.bytes = reinterpret_cast<const char *>(v.tags + v.n);
  return v;
}

inline view parse(SEXP raw) noexcept {
  return parse(RAW(raw));
}

template<typename Offset>
inline uint64_t end_of(const unsigned char * ends, size_t i) noexcept {
  Offset out;
  std::memcpy(&out, ends + sizeof(Offset) * i, sizeof(out));
  return out;
}

inline uint64_t end_at(const view & v, size_t i) noexcept {
  return v.wide ? end_of<uint64_t>(v.ends, i) : end_of<uint32_t>(v.ends, i);
}

inline size_t payload(const view & v) noexcept {
  return v.n == 0 ? 0 : static_cast<size_t>(end_at(v, v.n - 1));
}

inline R_xlen_t length(SEXP raw) noexcept {
  return static_cast<R_xlen_t>(parse(raw).n);
}

inline void write_head(unsigned char * raw, size_t n, bool wide) noexcept {
  const uint64_t head = static_cast<uint64_t>(n) | (wide ? wide_bit : 0);
  std::memcpy(raw, &head, sizeof(head));
}

inline void write_end(unsigned char * ends, bool wide, size_t i, uint64_t end) noexcept {
  if(wide) {
    std::memcpy(ends + sizeof(uint64_t) * i, &end, sizeof(end));
  } else {
    const uint32_t narrow = static_cast<uint32_t>(end);
    std::memcpy(ends + sizeof(uint32_t) * i, &narrow, sizeof(narrow));
  }
}

} // namespace charpack_layout

struct charpack_altrep {
  static R_altrep_class_t class_t;

  static bool is(SEXP x) {
    return TYPEOF(x) == STRSXP && ALTREP(x) == TRUE && R_altrep_inherits(x, class_t) == TRUE;
  }

  static bool is_unfilled(SEXP x) {
    return R_altrep_data1(x) == R_NilValue && R_altrep_data2(x) == R_NilValue;
  }

  static R_xlen_t Length(SEXP x) {
    SEXP data2 = R_altrep_data2(x);
    if(data2 != R_NilValue) {
      return Rf_xlength(data2);
    }
    SEXP data1 = R_altrep_data1(x);
    return data1 == R_NilValue ? 0 : charpack_layout::length(data1);
  }

  static const char * state_name(SEXP x) {
    if(R_altrep_data2(x) != R_NilValue) return "materialized";
    if(R_altrep_data1(x) != R_NilValue) return "filled";
    return "unfilled";
  }

  static Rboolean Inspect(SEXP x, int /* pre */, int /* deep */, int /* pvec */,
                          void (*/* inspect_subtree */)(SEXP, int, int, int)) {
    Rprintf("charport charpack (len=%llu, %s)\n",
            static_cast<unsigned long long int>(Length(x)), state_name(x));
    return TRUE;
  }

  // The strings of a filled data1 as a new character vector. Sizes were
  // checked against R's string limit when the vector was filled, so the only
  // R errors here are allocation failures and embedded NULs.
  static SEXP strings(SEXP data1) {
    const R_xlen_t n = charpack_layout::length(data1);
    SEXP out = PROTECT(Rf_allocVector(STRSXP, n));
    // data1 is not moved by the allocations below, so v stays valid.
    const charpack_layout::view v = charpack_layout::parse(data1);
    uint64_t begin = 0;
    for(size_t i = 0; i < v.n; ++i) {
      const cetype_ext_t enc = charport_cetype_ext(v.tags[i]);
      const uint64_t end = charpack_layout::end_at(v, i);
      if(enc == CETYPE_EXT_NA) {
        SET_STRING_ELT(out, static_cast<R_xlen_t>(i), NA_STRING);
      } else {
        SET_STRING_ELT(out, static_cast<R_xlen_t>(i),
                       Rf_mkCharLenCE(v.bytes + begin, static_cast<int>(end - begin),
                                      cpi::to_base_encoding(enc)));
      }
      begin = end;
    }
    UNPROTECT(1);
    return out;
  }

  static bool has_native(SEXP data1) noexcept {
    const charpack_layout::view v = charpack_layout::parse(data1);
    return v.n > 0 && std::memchr(v.tags, CETYPE_EXT_NATIVE.value, v.n) != nullptr;
  }

  // data2 is installed only once complete.
  static SEXP Materialize(SEXP x) {
    SEXP data2 = R_altrep_data2(x);
    if(data2 != R_NilValue) {
      return data2;
    }
    SEXP data1 = R_altrep_data1(x);
    data2 = PROTECT(data1 == R_NilValue ? Rf_allocVector(STRSXP, 0) : strings(data1));
    R_set_altrep_data2(x, data2);
    R_set_altrep_data1(x, R_NilValue);
    UNPROTECT(1);
    return data2;
  }

  static SEXP Elt(SEXP x, R_xlen_t i) {
    return STRING_ELT(Materialize(x), i);
  }

  static void Set_elt(SEXP x, R_xlen_t i, SEXP v) {
    SET_STRING_ELT(Materialize(x), i, v);
  }

  static void * Dataptr(SEXP x, Rboolean /* writeable */) {
    return const_cast<void *>(static_cast<const void *>(STRING_PTR_RO(Materialize(x))));
  }

  static const void * Dataptr_or_null(SEXP x) {
    SEXP data2 = R_altrep_data2(x);
    return data2 == R_NilValue ? nullptr : static_cast<const void *>(STRING_PTR_RO(data2));
  }

  // A filled data1 never changes, so an unmaterialized copy shares it. R's
  // duplicate copies attributes onto the result.
  static SEXP Duplicate(SEXP x, Rboolean /* deep */) {
    SEXP data2 = R_altrep_data2(x);
    if(data2 != R_NilValue) {
      return Rf_duplicate(data2);
    }
    SEXP data1 = R_altrep_data1(x);
    if(data1 == R_NilValue) {
      return Rf_allocVector(STRSXP, 0);
    }
    return R_new_altrep(class_t, data1, R_NilValue);
  }

  static int No_NA(SEXP x) {
    SEXP data2 = R_altrep_data2(x);
    if(data2 != R_NilValue) {
      const R_xlen_t n = Rf_xlength(data2);
      const SEXP * p = STRING_PTR_RO(data2);
      for(R_xlen_t i = 0; i < n; ++i) {
        if(p[i] == NA_STRING) {
          return 0;
        }
      }
      return 1;
    }
    SEXP data1 = R_altrep_data1(x);
    if(data1 == R_NilValue) {
      return 1;
    }
    const charpack_layout::view v = charpack_layout::parse(data1);
    return v.n > 0 && std::memchr(v.tags, CETYPE_EXT_NA.value, v.n) != nullptr ? 0 : 1;
  }

  // The Reader state is data1's bytes, read in place: the end offsets give
  // any record in constant time, so a Reader allocates nothing and needs no
  // release. data1 is immutable and dropped only by materialization, which
  // the Reader contract rules out during a borrow.
  static void * reader_init(SEXP x) {
    if(R_altrep_data2(x) != R_NilValue) {
      return nullptr;
    }
    SEXP data1 = R_altrep_data1(x);
    return data1 == R_NilValue ? nullptr : static_cast<void *>(RAW(data1));
  }

  static charpack_layout::view state_view(void * state) noexcept {
    return charpack_layout::parse(static_cast<const unsigned char *>(state));
  }

  // One record's view. Lengths are always written; pointers and encodings
  // only when asked for. The loops below pass everything they read as
  // locals, because a store through an output pointer may alias anything
  // (cetype_ext_t is a byte), including R_NaInt.
  template<bool Ptrs, bool Encs>
  static void put(unsigned char tag, uint64_t begin, uint64_t end, const char * bytes,
                  const char * empty, int na, const char ** ptr, int * len,
                  cetype_ext_t * enc) noexcept {
    const bool is_na = tag == CETYPE_EXT_NA.value;
    *len = is_na ? na : static_cast<int>(end - begin);
    if(Ptrs) {
      *ptr = is_na ? nullptr : end == begin ? empty : bytes + begin;
    }
    if(Encs) {
      *enc = charport_cetype_ext(tag);
    }
  }

  template<typename Offset, bool Ptrs, bool Encs>
  static void range_loop(const charpack_layout::view & v, R_xlen_t start, R_xlen_t size,
                         const char ** ptrs, int * lens, cetype_ext_t * encs) noexcept {
    const unsigned char * const ends = v.ends;
    const unsigned char * const tags = v.tags;
    const char * const bytes = v.bytes;
    const char * const empty = cpc::empty_data();
    const int na = NA_INTEGER;
    const size_t first = static_cast<size_t>(start);
    R_xlen_t j = 0;
    // Record 0 has no previous end; after it, each begin is the previous
    // end, read from memory rather than carried, so the loop can vectorize.
    if(size > 0 && first == 0) {
      const uint64_t end = charpack_layout::end_of<Offset>(ends, 0);
      put<Ptrs, Encs>(tags[0], 0, end, bytes, empty, na, ptrs, lens, encs);
      j = 1;
    }
    for(; j < size; ++j) {
      const size_t i = first + static_cast<size_t>(j);
      const uint64_t begin = charpack_layout::end_of<Offset>(ends, i - 1);
      const uint64_t end = charpack_layout::end_of<Offset>(ends, i);
      put<Ptrs, Encs>(tags[i], begin, end, bytes, empty, na, Ptrs ? ptrs + j : ptrs, lens + j,
                      Encs ? encs + j : encs);
    }
  }

  template<typename Offset, bool Ptrs, bool Encs>
  static void index_loop(const charpack_layout::view & v, const R_xlen_t * indices,
                         R_xlen_t size, const char ** ptrs, int * lens,
                         cetype_ext_t * encs) noexcept {
    const unsigned char * const ends = v.ends;
    const unsigned char * const tags = v.tags;
    const char * const bytes = v.bytes;
    const char * const empty = cpc::empty_data();
    const int na = NA_INTEGER;
    for(R_xlen_t j = 0; j < size; ++j) {
      const size_t i = static_cast<size_t>(indices[j]);
      const uint64_t begin = i == 0 ? 0 : charpack_layout::end_of<Offset>(ends, i - 1);
      const uint64_t end = charpack_layout::end_of<Offset>(ends, i);
      put<Ptrs, Encs>(tags[i], begin, end, bytes, empty, na, Ptrs ? ptrs + j : ptrs, lens + j,
                      Encs ? encs + j : encs);
    }
  }

  template<bool Ptrs, bool Encs>
  static void range_access(void * state, R_xlen_t start, R_xlen_t size, const char ** ptrs,
                           int * lens, cetype_ext_t * encs) noexcept {
    const charpack_layout::view v = state_view(state);
    if(v.wide) {
      range_loop<uint64_t, Ptrs, Encs>(v, start, size, ptrs, lens, encs);
    } else {
      range_loop<uint32_t, Ptrs, Encs>(v, start, size, ptrs, lens, encs);
    }
  }

  template<bool Ptrs, bool Encs>
  static void index_access(void * state, const R_xlen_t * indices, R_xlen_t size,
                           const char ** ptrs, int * lens, cetype_ext_t * encs) noexcept {
    const charpack_layout::view v = state_view(state);
    if(v.wide) {
      index_loop<uint64_t, Ptrs, Encs>(v, indices, size, ptrs, lens, encs);
    } else {
      index_loop<uint32_t, Ptrs, Encs>(v, indices, size, ptrs, lens, encs);
    }
  }

  static int reader_strviews_range(
      void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
      int * out_lens, cetype_ext_t * out_encs) {
    range_access<true, true>(state, start, size, out_ptrs, out_lens, out_encs);
    return CHARPORT_STATUS_OK;
  }

  static int reader_strviews_index(
      void * state, const R_xlen_t * indices, R_xlen_t size,
      const char ** out_ptrs, int * out_lens, cetype_ext_t * out_encs) {
    index_access<true, true>(state, indices, size, out_ptrs, out_lens, out_encs);
    return CHARPORT_STATUS_OK;
  }

  static int reader_byteviews_range(
      void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
      int * out_lens) {
    range_access<true, false>(state, start, size, out_ptrs, out_lens, nullptr);
    return CHARPORT_STATUS_OK;
  }

  static int reader_byteviews_index(
      void * state, const R_xlen_t * indices, R_xlen_t size,
      const char ** out_ptrs, int * out_lens) {
    index_access<true, false>(state, indices, size, out_ptrs, out_lens, nullptr);
    return CHARPORT_STATUS_OK;
  }

  static int reader_lengths_range(
      void * state, R_xlen_t start, R_xlen_t size, int * out_lens) {
    range_access<false, false>(state, start, size, nullptr, out_lens, nullptr);
    return CHARPORT_STATUS_OK;
  }

  static int reader_lengths_index(
      void * state, const R_xlen_t * indices, R_xlen_t size, int * out_lens) {
    index_access<false, false>(state, indices, size, nullptr, out_lens, nullptr);
    return CHARPORT_STATUS_OK;
  }

  // Encodings need only the tags.
  static int reader_encodings_range(
      void * state, R_xlen_t start, R_xlen_t size, cetype_ext_t * out_encs) {
    const charpack_layout::view v = state_view(state);
    for(R_xlen_t j = 0; j < size; ++j) {
      out_encs[j] = charport_cetype_ext(v.tags[static_cast<size_t>(start + j)]);
    }
    return CHARPORT_STATUS_OK;
  }

  static int reader_encodings_index(
      void * state, const R_xlen_t * indices, R_xlen_t size,
      cetype_ext_t * out_encs) {
    const charpack_layout::view v = state_view(state);
    for(R_xlen_t j = 0; j < size; ++j) {
      out_encs[j] = charport_cetype_ext(v.tags[static_cast<size_t>(indices[j])]);
    }
    return CHARPORT_STATUS_OK;
  }

  // data1 as a serialized state: charvec's packed body, little-endian.
  static SEXP packed_state(SEXP data1) {
    const charpack_layout::view v = charpack_layout::parse(data1);
    const size_t payload = charpack_layout::payload(v);
    // Never larger than data1, whose offsets take at least 4 bytes each.
    const size_t total = charpack_layout::header_bytes +
                         v.n * (sizeof(uint32_t) + 1) + payload;
    SEXP state = Rf_allocVector(RAWSXP, static_cast<R_xlen_t>(total));
    // data1 is not moved by the allocation, so v stays valid.
    unsigned char * p = RAW(state);
    charvec_write_u64_le(p, static_cast<uint64_t>(v.n));
    unsigned char * sizes = p + charpack_layout::header_bytes;
    uint64_t begin = 0;
    for(size_t i = 0; i < v.n; ++i) {
      const uint64_t end = charpack_layout::end_at(v, i);
      charvec_write_u32_le(sizes + sizeof(uint32_t) * i, static_cast<uint32_t>(end - begin));
      begin = end;
    }
    unsigned char * tags = sizes + sizeof(uint32_t) * v.n;
    if(v.n > 0) {
      std::memcpy(tags, v.tags, v.n);
    }
    if(payload > 0) {
      std::memcpy(tags + v.n, v.bytes, payload);
    }
    return state;
  }

  // A data1 from a serialized state, which is validated first. Nothing else
  // holds a state fresh from the stream, so with u32 offsets the sizes become
  // end offsets in place; u64 offsets, or force_wide (for tests), need a new
  // vector.
  static SEXP data1_from_state(SEXP state, bool force_wide) {
    const bool swap = !charvec_host_is_little_endian();
    charvec_serialized_layout layout;
    charport_sexp_guard("charpack Unserialize", [&]() -> SEXP {
      layout = charvec_parse_packed(RAW(state), static_cast<size_t>(Rf_xlength(state)), swap);
      charvec_check_records(layout);
      return R_NilValue;
    });
    const size_t n = layout.n;
    const size_t payload = static_cast<size_t>(layout.data_end - layout.data_offset);
    const bool wide = force_wide || charpack_layout::needs_wide(payload);
    const unsigned char * sizes = layout.size_offset;
    SEXP data1 = state;
    if(wide) {
      const size_t total = charpack_layout::total_bytes(n, payload, true);
      if(total == 0) {
        Rf_error("charport charpack Unserialize: vector exceeds the maximum raw vector size");
      }
      data1 = Rf_allocVector(RAWSXP, static_cast<R_xlen_t>(total));
      // state is not moved by the allocation, so layout stays valid.
      unsigned char * tags = RAW(data1) + charpack_layout::header_bytes + sizeof(uint64_t) * n;
      if(n > 0) {
        std::memcpy(tags, layout.enc_offset, n);
      }
      if(payload > 0) {
        std::memcpy(tags + n, layout.data_offset, payload);
      }
    }
    // In place, each size is read before its slot is overwritten.
    unsigned char * out = RAW(data1);
    unsigned char * ends = out + charpack_layout::header_bytes;
    uint64_t end = 0;
    for(size_t i = 0; i < n; ++i) {
      end += charvec_read_u32_advance(sizes, swap);
      charpack_layout::write_end(ends, wide, i, end);
    }
    charpack_layout::write_head(out, n, wide);
    return data1;
  }

  // Filled: charvec's packed body. Materialized: data2, which unserializes to
  // an ordinary character vector. Unfilled: NULL, so R writes an empty one.
  //
  // A packed state's native-tagged records are UTF-8 by construction: one
  // written outside a UTF-8 locale is sent instead as a temporary character
  // vector, which R serializes with the stream's native encoding. The source
  // stays packed.
  static SEXP Serialized_state(SEXP x) {
    SEXP data2 = R_altrep_data2(x);
    if(data2 != R_NilValue) {
      return data2;
    }
    SEXP data1 = R_altrep_data1(x);
    if(data1 == R_NilValue) {
      return nullptr;
    }
    if(has_native(data1) && !charport_native_is_utf8_impl()) {
      return strings(data1);
    }
    return packed_state(data1);
  }

  static SEXP Unserialize(SEXP /* class */, SEXP state) {
    // From a materialized charpack, or one saved with native records outside
    // a UTF-8 locale: stays a plain character vector.
    if(TYPEOF(state) == STRSXP) {
      return state;
    }
    if(TYPEOF(state) != RAWSXP) {
      Rf_error("charport charpack Unserialize: invalid serialized_state type");
    }
    SEXP data1 = PROTECT(data1_from_state(state, false));
    // Native-tagged records were written in a UTF-8 locale (see
    // Serialized_state); outside one, say so explicitly.
    if(has_native(data1) && !charport_native_is_utf8_impl()) {
      const charpack_layout::view v = charpack_layout::parse(data1);
      unsigned char * tags = const_cast<unsigned char *>(v.tags);
      for(size_t i = 0; i < v.n; ++i) {
        if(tags[i] == CETYPE_EXT_NATIVE.value) {
          tags[i] = CETYPE_EXT_UTF8.value;
        }
      }
    }
    SEXP x = R_new_altrep(class_t, data1, R_NilValue);
    UNPROTECT(1);
    return x;
  }

  static void Init(DllInfo * dll) {
    // class + package names are baked into Inspect output and the registry;
    // keep them stable
    class_t = R_make_altstring_class("charpack", "charport", dll);

    R_set_altrep_Duplicate_method(class_t, Duplicate);
    R_set_altrep_Length_method(class_t, Length);
    R_set_altrep_Inspect_method(class_t, Inspect);
    R_set_altrep_Serialized_state_method(class_t, Serialized_state);
    R_set_altrep_Unserialize_method(class_t, Unserialize);

    R_set_altvec_Dataptr_method(class_t, Dataptr);
    R_set_altvec_Dataptr_or_null_method(class_t, Dataptr_or_null);

    R_set_altstring_Elt_method(class_t, Elt);
    R_set_altstring_Set_elt_method(class_t, Set_elt);
    R_set_altstring_No_NA_method(class_t, No_NA);
  }
};

#endif
