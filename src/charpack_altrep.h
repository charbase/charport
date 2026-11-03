#ifndef CHARPORT_CHARPACK_ALTREP_H
#define CHARPORT_CHARPACK_ALTREP_H

#include "charvec_altrep.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

// charpack keeps its strings in one RAWSXP in data1:
//
//   [u64 n][u32 size x n][u8 cetype_ext x n][bytes]
//
// in native byte order. This is charvec's packed serialized body. NA is
// CETYPE_EXT_NA with size 0. Sizes come first so they stay 4-byte aligned.
// The serialized state of a filled charpack is data1 in little-endian order,
// which on a little-endian host is data1 itself.
//
// One representation, three states:
//   unfilled      data1 and data2 nil; length 0
//   filled        data1 is the RAWSXP; immutable once installed
//   materialized  data2 is a STRSXP; data1 nil
//
// A fill only moves unfilled to filled (or, for fill_materialized_na, to
// materialized). Reading or duplicating never yields another unfilled vector.

namespace charpack_layout {

constexpr size_t header_bytes = sizeof(uint64_t);
constexpr size_t element_bytes = sizeof(uint32_t) + sizeof(uint8_t);

inline R_xlen_t length(SEXP raw) noexcept {
  uint64_t n;
  std::memcpy(&n, RAW(raw), sizeof(n));
  return static_cast<R_xlen_t>(n);
}

inline const unsigned char * sizes(SEXP raw) noexcept {
  return RAW(raw) + header_bytes;
}

inline const unsigned char * encodings(SEXP raw, R_xlen_t n) noexcept {
  return sizes(raw) + sizeof(uint32_t) * static_cast<size_t>(n);
}

inline const char * bytes(SEXP raw, R_xlen_t n) noexcept {
  return reinterpret_cast<const char *>(encodings(raw, n) + static_cast<size_t>(n));
}

inline uint32_t size_at(const unsigned char * sizes, R_xlen_t i) noexcept {
  uint32_t out;
  std::memcpy(&out, sizes + sizeof(uint32_t) * static_cast<size_t>(i), sizeof(out));
  return out;
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

  // Sizes were checked against R's string limit when the vector was filled,
  // so the only R errors here are allocation failures and embedded NULs.
  // data2 is installed only once complete.
  static SEXP Materialize(SEXP x) {
    SEXP data2 = R_altrep_data2(x);
    if(data2 != R_NilValue) {
      return data2;
    }
    SEXP data1 = R_altrep_data1(x);
    if(data1 == R_NilValue) {
      data2 = PROTECT(Rf_allocVector(STRSXP, 0));
    } else {
      const R_xlen_t n = charpack_layout::length(data1);
      data2 = PROTECT(Rf_allocVector(STRSXP, n));
      const unsigned char * sizes = charpack_layout::sizes(data1);
      const unsigned char * encs = charpack_layout::encodings(data1, n);
      const char * s = charpack_layout::bytes(data1, n);
      for(R_xlen_t i = 0; i < n; ++i) {
        const cetype_ext_t enc = charport_cetype_ext(encs[i]);
        if(enc == CETYPE_EXT_NA) {
          SET_STRING_ELT(data2, i, NA_STRING);
          continue;
        }
        const uint32_t size = charpack_layout::size_at(sizes, i);
        SET_STRING_ELT(data2, i, Rf_mkCharLenCE(s, static_cast<int>(size), cpi::to_base_encoding(enc)));
        s += size;
      }
    }
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
    const R_xlen_t n = charpack_layout::length(data1);
    const unsigned char * encs = charpack_layout::encodings(data1, n);
    for(R_xlen_t i = 0; i < n; ++i) {
      if(encs[i] == CETYPE_EXT_NA.value) {
        return 0;
      }
    }
    return 1;
  }

  // Broker state. A vector of up to scan_limit elements needs no allocation:
  // the state is data1's byte address with the low bit set, and offsets are
  // summed from the sizes on each call. Longer vectors get a malloc'd pointer
  // per element so that range and index access can start anywhere. Views stay
  // valid while the borrow lasts: data1 is immutable and is only dropped by
  // materialization, which the Reader contract rules out during a borrow.
  static constexpr R_xlen_t scan_limit = 64;

  struct reader_state {
    const unsigned char * sizes;
    const unsigned char * encodings;
    const char ** ptrs;
  };

  struct packed {
    const unsigned char * sizes;
    const unsigned char * encodings;
    const char * bytes;
  };

  static bool is_scanned(void * state) noexcept {
    return (reinterpret_cast<uintptr_t>(state) & 1U) != 0;
  }

  static packed unpack_scanned(void * state) noexcept {
    const unsigned char * raw =
      reinterpret_cast<const unsigned char *>(reinterpret_cast<uintptr_t>(state) & ~uintptr_t(1));
    uint64_t n;
    std::memcpy(&n, raw, sizeof(n));
    const unsigned char * sizes = raw + charpack_layout::header_bytes;
    const unsigned char * encs = sizes + sizeof(uint32_t) * static_cast<size_t>(n);
    return packed{sizes, encs, reinterpret_cast<const char *>(encs + static_cast<size_t>(n))};
  }

  static void * reader_init(SEXP x) {
    if(R_altrep_data2(x) != R_NilValue) {
      return nullptr;
    }
    SEXP data1 = R_altrep_data1(x);
    if(data1 == R_NilValue) {
      return nullptr;
    }
    const R_xlen_t n = charpack_layout::length(data1);
    if(n <= scan_limit) {
      return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(RAW(data1)) | 1U);
    }
    const size_t count = static_cast<size_t>(n);
    if(count > (static_cast<size_t>(-1) - sizeof(reader_state)) / sizeof(const char *)) {
      return nullptr;
    }
    void * mem = std::malloc(sizeof(reader_state) + count * sizeof(const char *));
    if(mem == nullptr) {
      return nullptr;
    }
    reader_state * state = static_cast<reader_state *>(mem);
    state->sizes = charpack_layout::sizes(data1);
    state->encodings = charpack_layout::encodings(data1, n);
    state->ptrs = reinterpret_cast<const char **>(state + 1);
    const char * s = charpack_layout::bytes(data1, n);
    for(size_t i = 0; i < count; ++i) {
      if(state->encodings[i] == CETYPE_EXT_NA.value) {
        state->ptrs[i] = nullptr;
        continue;
      }
      const uint32_t size = charpack_layout::size_at(state->sizes, static_cast<R_xlen_t>(i));
      state->ptrs[i] = size == 0 ? cpc::empty_data() : s;
      s += size;
    }
    return state;
  }

  static void reader_release(void * state) {
    if(!is_scanned(state)) {
      std::free(state);
    }
  }

  static void emit(const unsigned char * sizes, cetype_ext_t enc, const char * bytes,
                   R_xlen_t i, const char ** ptr, int * len, cetype_ext_t * out_enc) noexcept {
    if(enc == CETYPE_EXT_NA) {
      *ptr = nullptr;
      *len = NA_INTEGER;
    } else {
      const uint32_t size = charpack_layout::size_at(sizes, i);
      *ptr = size == 0 ? cpc::empty_data() : bytes;
      *len = static_cast<int>(size);
    }
    *out_enc = enc;
  }

  static const char * scanned_offset(const packed & p, R_xlen_t i) noexcept {
    const char * s = p.bytes;
    for(R_xlen_t k = 0; k < i; ++k) {
      s += charpack_layout::size_at(p.sizes, k);
    }
    return s;
  }

  // Views of [start, start + size), written at out + j. Any of the outputs may
  // be scratch when the caller does not need it.
  static void range_views(void * state, R_xlen_t start, R_xlen_t size, const char ** ptrs,
                          int * lens, cetype_ext_t * encs, bool step_ptrs, bool step_lens,
                          bool step_encs) noexcept {
    if(is_scanned(state)) {
      const packed p = unpack_scanned(state);
      const char * s = scanned_offset(p, start);
      for(R_xlen_t j = 0; j < size; ++j) {
        const R_xlen_t i = start + j;
        emit(p.sizes, charport_cetype_ext(p.encodings[i]), s, i,
             ptrs + (step_ptrs ? j : 0), lens + (step_lens ? j : 0), encs + (step_encs ? j : 0));
        s += charpack_layout::size_at(p.sizes, i);
      }
      return;
    }
    const reader_state * st = static_cast<const reader_state *>(state);
    for(R_xlen_t j = 0; j < size; ++j) {
      const R_xlen_t i = start + j;
      emit(st->sizes, charport_cetype_ext(st->encodings[i]), st->ptrs[i], i,
           ptrs + (step_ptrs ? j : 0), lens + (step_lens ? j : 0), encs + (step_encs ? j : 0));
    }
  }

  static void index_views(void * state, const R_xlen_t * indices, R_xlen_t size,
                          const char ** ptrs, int * lens, cetype_ext_t * encs,
                          bool step_ptrs, bool step_lens, bool step_encs) noexcept {
    const bool scanned = is_scanned(state);
    const packed p = scanned ? unpack_scanned(state) : packed{nullptr, nullptr, nullptr};
    const reader_state * st = static_cast<const reader_state *>(state);
    for(R_xlen_t j = 0; j < size; ++j) {
      const R_xlen_t i = indices[j];
      if(scanned) {
        emit(p.sizes, charport_cetype_ext(p.encodings[i]), scanned_offset(p, i), i,
             ptrs + (step_ptrs ? j : 0), lens + (step_lens ? j : 0), encs + (step_encs ? j : 0));
      } else {
        emit(st->sizes, charport_cetype_ext(st->encodings[i]), st->ptrs[i], i,
             ptrs + (step_ptrs ? j : 0), lens + (step_lens ? j : 0), encs + (step_encs ? j : 0));
      }
    }
  }

  static int reader_strviews_range(
      void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
      int * out_lens, cetype_ext_t * out_encs) {
    range_views(state, start, size, out_ptrs, out_lens, out_encs, true, true, true);
    return CHARPORT_STATUS_OK;
  }

  static int reader_strviews_index(
      void * state, const R_xlen_t * indices, R_xlen_t size,
      const char ** out_ptrs, int * out_lens, cetype_ext_t * out_encs) {
    index_views(state, indices, size, out_ptrs, out_lens, out_encs, true, true, true);
    return CHARPORT_STATUS_OK;
  }

  static int reader_byteviews_range(
      void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
      int * out_lens) {
    cetype_ext_t enc;
    range_views(state, start, size, out_ptrs, out_lens, &enc, true, true, false);
    return CHARPORT_STATUS_OK;
  }

  static int reader_byteviews_index(
      void * state, const R_xlen_t * indices, R_xlen_t size,
      const char ** out_ptrs, int * out_lens) {
    cetype_ext_t enc;
    index_views(state, indices, size, out_ptrs, out_lens, &enc, true, true, false);
    return CHARPORT_STATUS_OK;
  }

  static int reader_lengths_range(
      void * state, R_xlen_t start, R_xlen_t size, int * out_lens) {
    const char * ptr;
    cetype_ext_t enc;
    range_views(state, start, size, &ptr, out_lens, &enc, false, true, false);
    return CHARPORT_STATUS_OK;
  }

  static int reader_lengths_index(
      void * state, const R_xlen_t * indices, R_xlen_t size, int * out_lens) {
    const char * ptr;
    cetype_ext_t enc;
    index_views(state, indices, size, &ptr, out_lens, &enc, false, true, false);
    return CHARPORT_STATUS_OK;
  }

  static int reader_encodings_range(
      void * state, R_xlen_t start, R_xlen_t size, cetype_ext_t * out_encs) {
    const char * ptr;
    int len;
    range_views(state, start, size, &ptr, &len, out_encs, false, false, true);
    return CHARPORT_STATUS_OK;
  }

  static int reader_encodings_index(
      void * state, const R_xlen_t * indices, R_xlen_t size,
      cetype_ext_t * out_encs) {
    const char * ptr;
    int len;
    index_views(state, indices, size, &ptr, &len, out_encs, false, false, true);
    return CHARPORT_STATUS_OK;
  }

  // Filled: data1, little-endian. Materialized: data2, which unserializes to
  // an ordinary character vector. Unfilled: NULL, so R writes an empty one.
  static SEXP Serialized_state(SEXP x) {
    SEXP data2 = R_altrep_data2(x);
    if(data2 != R_NilValue) {
      return data2;
    }
    SEXP data1 = R_altrep_data1(x);
    if(data1 == R_NilValue) {
      return nullptr;
    }
    if(charvec_host_is_little_endian()) {
      return data1;
    }
    SEXP state = PROTECT(Rf_duplicate(data1));
    charvec_bswap_packed(RAW(state), static_cast<size_t>(charpack_layout::length(data1)));
    UNPROTECT(1);
    return state;
  }

  // The state comes from the serialization stream, so nothing else holds it
  // and on a little-endian host it becomes data1 without a copy.
  static SEXP Unserialize(SEXP /* class */, SEXP state) {
    if(TYPEOF(state) == STRSXP) {
      return state;
    }
    if(TYPEOF(state) != RAWSXP) {
      Rf_error("charport charpack Unserialize: invalid serialized_state type");
    }
    const bool swap = !charvec_host_is_little_endian();
    size_t n = 0;
    charport_sexp_guard("charpack Unserialize", [&]() -> SEXP {
      const charvec_serialized_layout layout = charvec_parse_packed(
        RAW(state), static_cast<size_t>(Rf_xlength(state)), swap);
      charvec_check_records(layout);
      n = layout.n;
      return R_NilValue;
    });
    SEXP data1 = PROTECT(swap ? Rf_duplicate(state) : state);
    if(swap) {
      charvec_bswap_packed(RAW(data1), n);
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
