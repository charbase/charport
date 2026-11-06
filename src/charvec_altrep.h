#ifndef CHARPORT_CHARVEC_ALTREP_H
#define CHARPORT_CHARVEC_ALTREP_H

#define R_NO_REMAP
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <R_ext/Rdynload.h>
#include <Rversion.h>

#include "../inst/include/charport.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <new>

namespace cpi = charport::internal;
namespace cpv = charport::charvec;
namespace cpc = charport::charvec::components;

// Defined in charport_registry.cpp; exported as charport_native_is_utf8_v1.
extern "C" int charport_native_is_utf8_impl(void);

namespace charvec_detail {
inline SEXP wrap_store_with_cleanup(cpv::Store * source) noexcept;
}

namespace charport {
namespace internal {

inline bool charsxp_is_ascii(SEXP x) {
#if (R_VERSION >= R_Version(4, 5, 0))
  return Rf_charIsASCII(x) == TRUE;
#else
  // Before 4.5 there is no CRAN-safe accessor for the CHARSXP ASCII flag, so
  // this scans the bytes (O(len) per native-encoded string). Reading the
  // levels bit directly would be free but is not API-safe; revisit later.
  return check_ascii(CHAR(x), static_cast<size_t>(Rf_xlength(x)));
#endif
}

inline cetype_t to_base_encoding(cetype_ext_t enc) noexcept {
  switch(enc.value) {
  case CETYPE_EXT_ASCII.value:
    return CE_NATIVE;
  case CETYPE_EXT_UTF8.value:
  case CETYPE_EXT_ASCII_OR_UTF8.value:
    return CE_UTF8;
  case CETYPE_EXT_LATIN1.value:
    return CE_LATIN1;
  case CETYPE_EXT_BYTES.value:
    return CE_BYTES;
  default:
    return CE_NATIVE;
  }
}

inline SEXP make_charsxp(const charport_strview & view) {
  if(view.is_na()) {
    return NA_STRING;
  }
  if(!check_r_string_len(view.len)) {
    throw std::runtime_error("string size exceeds R string size");
  }
  return Rf_mkCharLenCE(view.ptr, static_cast<int>(view.len), to_base_encoding(view.enc));
}

inline cetype_ext_t classify_charsxp(SEXP x) {
  switch(Rf_getCharCE(x)) {
  case CE_UTF8:
    return CETYPE_EXT_UTF8;
  case CE_LATIN1:
    return CETYPE_EXT_LATIN1;
  case CE_BYTES:
    return CETYPE_EXT_BYTES;
  default:
    return charsxp_is_ascii(x) ? CETYPE_EXT_ASCII
                               : CETYPE_EXT_NATIVE;
  }
}

// Bulk-input validation shared by the from_views/fill entry points.
inline bool valid_view_encoding(cetype_ext_t enc) noexcept {
  switch(enc.value) {
  case CETYPE_EXT_NATIVE.value:
  case CETYPE_EXT_UTF8.value:
  case CETYPE_EXT_LATIN1.value:
  case CETYPE_EXT_BYTES.value:
  case CETYPE_EXT_ASCII_OR_UTF8.value:
  case CETYPE_EXT_ASCII.value:
  case CETYPE_EXT_NA.value:
    return true;
  }
  return false;
}

inline bool view_is_na(const char * ptr, int len, cetype_ext_t enc) noexcept {
  return ptr == nullptr || len == NA_INTEGER || enc == CETYPE_EXT_NA;
}

inline charport_strview charsxp_to_view(SEXP x) {
  if(x == NA_STRING) {
    return make_strview(nullptr, NA_INTEGER, CETYPE_EXT_NA);
  }
  return make_strview(CHAR(x), LENGTH(x), classify_charsxp(x));
}

} // namespace internal
} // namespace charport

template <typename Fun>
inline SEXP charport_sexp_guard(const char * op, Fun fun) {
  char msg[512];
  try {
    return fun();
  } catch(const std::exception & e) {
    // snprintf copies the message; Rf_error must run after C++ catch exits.
    std::snprintf(msg, sizeof(msg), "%s", e.what());
  } catch(...) {
    std::snprintf(msg, sizeof(msg), "unknown C++ exception");
  }
  Rf_error("charport %s: %s", op, msg);
  return R_NilValue;
}

// serialized_state is "CPV2", an endian flag, then a packed body:
//
//   [u64 n][u32 size x n][u8 cetype_ext x n][bytes]
//
// It is written little-endian (flag 1) on every host. A body with flag 2 is
// big-endian and is still read. charpack uses its own layout, with end offsets
// rather than sizes (see charpack_altrep.h).
//
// CPV2 is only written in a UTF-8 locale when it holds native records, so
// they are UTF-8 text. CPV1 (charport 0.1) could come from any locale; its
// native records are read as the current locale's, as they always were.
struct charvec_serialized_layout {
  size_t n = 0;
  bool swap = false;
  bool native_is_utf8_text = false;  // CPV2
  const unsigned char * size_offset = nullptr;
  const unsigned char * enc_offset = nullptr;
  const unsigned char * data_offset = nullptr;
  const unsigned char * data_end = nullptr;
};

inline bool charvec_host_is_little_endian() noexcept {
  const uint16_t one = 1;
  return *reinterpret_cast<const unsigned char *>(&one) == 1;
}

inline unsigned char charvec_native_endian_flag() noexcept {
  return charvec_host_is_little_endian() ? static_cast<unsigned char>(1)
                                         : static_cast<unsigned char>(2);
}

inline uint32_t charvec_bswap32(uint32_t x) noexcept {
  return ((x & 0x000000ffU) << 24) |
         ((x & 0x0000ff00U) << 8) |
         ((x & 0x00ff0000U) >> 8) |
         ((x & 0xff000000U) >> 24);
}

inline uint64_t charvec_bswap64(uint64_t x) noexcept {
  return ((x & 0x00000000000000ffULL) << 56) |
         ((x & 0x000000000000ff00ULL) << 40) |
         ((x & 0x0000000000ff0000ULL) << 24) |
         ((x & 0x00000000ff000000ULL) << 8) |
         ((x & 0x000000ff00000000ULL) >> 8) |
         ((x & 0x0000ff0000000000ULL) >> 24) |
         ((x & 0x00ff000000000000ULL) >> 40) |
         ((x & 0xff00000000000000ULL) >> 56);
}

inline uint64_t charvec_read_u64(const unsigned char * ptr, bool swap) noexcept {
  uint64_t out = 0;
  std::memcpy(&out, ptr, sizeof(uint64_t));
  return swap ? charvec_bswap64(out) : out;
}

inline uint32_t charvec_read_u32_advance(const unsigned char *& ptr, bool swap) noexcept {
  uint32_t out = 0;
  std::memcpy(&out, ptr, sizeof(uint32_t));
  ptr += sizeof(uint32_t);
  return swap ? charvec_bswap32(out) : out;
}

inline void charvec_write_u64_le(unsigned char * ptr, uint64_t x) noexcept {
  if(!charvec_host_is_little_endian()) {
    x = charvec_bswap64(x);
  }
  std::memcpy(ptr, &x, sizeof(uint64_t));
}

inline void charvec_write_u32_le(unsigned char * ptr, uint32_t x) noexcept {
  if(!charvec_host_is_little_endian()) {
    x = charvec_bswap32(x);
  }
  std::memcpy(ptr, &x, sizeof(uint32_t));
}

// Swaps n and the sizes of a packed body of n records in place.
inline void charvec_bswap_packed(unsigned char * body, size_t n) noexcept {
  uint64_t n64;
  std::memcpy(&n64, body, sizeof(uint64_t));
  n64 = charvec_bswap64(n64);
  std::memcpy(body, &n64, sizeof(uint64_t));
  unsigned char * sizes = body + sizeof(uint64_t);
  for(size_t i = 0; i < n; ++i) {
    uint32_t size;
    std::memcpy(&size, sizes + sizeof(uint32_t) * i, sizeof(uint32_t));
    size = charvec_bswap32(size);
    std::memcpy(sizes + sizeof(uint32_t) * i, &size, sizeof(uint32_t));
  }
}

inline size_t charvec_serialized_prefix_bytes() noexcept {
  return 5;  // "CPV2" + endian flag
}

inline size_t charvec_serialized_length(size_t n, size_t payload_bytes) {
  const size_t sizes_bytes = cpi::checked_mul_size(n, sizeof(uint32_t), "serialized_state");
  const size_t enc_bytes = cpi::checked_mul_size(n, sizeof(uint8_t), "serialized_state");
  const size_t header_bytes = cpi::checked_add_size(
    cpi::checked_add_size(
      cpi::checked_add_size(charvec_serialized_prefix_bytes(), sizeof(uint64_t), "serialized_state"),
      sizes_bytes,
      "serialized_state"),
    enc_bytes,
    "serialized_state"
  );
  const size_t total_bytes = cpi::checked_add_size(header_bytes, payload_bytes, "serialized_state");
  if(total_bytes > static_cast<size_t>(std::numeric_limits<R_xlen_t>::max())) {
    throw std::runtime_error("serialized_state length exceeds R_xlen_t");
  }
  return total_bytes;
}

// Checks that a packed body's header fits in len bytes. Records are checked by
// charvec_check_records.
inline charvec_serialized_layout charvec_parse_packed(const unsigned char * body, size_t len,
                                                      bool swap) {
  if(len < sizeof(uint64_t)) {
    throw std::runtime_error("serialized_state is truncated");
  }
  const uint64_t n64 = charvec_read_u64(body, swap);
  if(n64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
     n64 > static_cast<uint64_t>(std::numeric_limits<R_xlen_t>::max())) {
    throw std::runtime_error("serialized_state length exceeds R_xlen_t");
  }

  const size_t n = static_cast<size_t>(n64);
  const size_t sizes_bytes = cpi::checked_mul_size(n, sizeof(uint32_t), "serialized_state header");
  const size_t enc_bytes = cpi::checked_mul_size(n, sizeof(uint8_t), "serialized_state header");
  const size_t header_bytes = cpi::checked_add_size(
    cpi::checked_add_size(sizeof(uint64_t), sizes_bytes, "serialized_state header"),
    enc_bytes,
    "serialized_state header"
  );

  if(len < header_bytes) {
    throw std::runtime_error("serialized_state header is truncated");
  }

  charvec_serialized_layout layout;
  layout.n = n;
  layout.swap = swap;
  layout.size_offset = body + sizeof(uint64_t);
  layout.enc_offset = layout.size_offset + sizes_bytes;
  layout.data_offset = layout.enc_offset + enc_bytes;
  layout.data_end = body + len;
  return layout;
}

inline charvec_serialized_layout charvec_parse_serialized(SEXP serialized_state) {
  if(TYPEOF(serialized_state) != RAWSXP) {
    throw std::runtime_error("invalid serialized_state type");
  }

  const size_t raw_len = static_cast<size_t>(Rf_xlength(serialized_state));
  const size_t prefix_bytes = charvec_serialized_prefix_bytes();
  if(raw_len < prefix_bytes + sizeof(uint64_t)) {
    throw std::runtime_error("serialized_state is truncated");
  }

  const unsigned char * serialized_ptr = RAW(serialized_state);
  if(serialized_ptr[0] != 'C' || serialized_ptr[1] != 'P' ||
     serialized_ptr[2] != 'V' || (serialized_ptr[3] != '1' && serialized_ptr[3] != '2')) {
    throw std::runtime_error("invalid serialized_state format");
  }
  const unsigned char source_endian = serialized_ptr[4];
  if(source_endian != 1 && source_endian != 2) {
    throw std::runtime_error("invalid serialized_state endian flag");
  }
  const bool swap = source_endian != charvec_native_endian_flag();
  charvec_serialized_layout layout =
    charvec_parse_packed(serialized_ptr + prefix_bytes, raw_len - prefix_bytes, swap);
  layout.native_is_utf8_text = serialized_ptr[3] == '2';
  return layout;
}

// Untrusted input: accepts every encoding a record can legitimately hold,
// rejects an out-of-range encoding byte, and requires the sizes to cover the
// payload exactly.
inline void charvec_check_records(const charvec_serialized_layout & layout) {
  const unsigned char * size_offset = layout.size_offset;
  const size_t payload = static_cast<size_t>(layout.data_end - layout.data_offset);
  size_t used = 0;
  for(size_t i = 0; i < layout.n; ++i) {
    const uint32_t size = charvec_read_u32_advance(size_offset, layout.swap);
    if(!cpi::check_r_string_len(size)) {
      throw std::runtime_error("serialized string size exceeds R string size");
    }
    const cetype_ext_t encoding = charport_cetype_ext(layout.enc_offset[i]);
    if(!cpi::valid_view_encoding(encoding)) {
      throw std::runtime_error("invalid string encoding in serialized_state");
    }
    if(encoding == CETYPE_EXT_NA && size != 0) {
      throw std::runtime_error("serialized NA string must have zero length");
    }
    if(size > payload - used) {
      throw std::runtime_error("serialized_state payload is truncated");
    }
    used += size;
  }
  if(used != payload) {
    throw std::runtime_error("serialized_state has trailing bytes");
  }
}

namespace charvec_detail {

inline cpv::Store rebuild_store(const cpc::RecordTable & source) {
  cpv::Store out(source.size(), 0);
  const size_t max_block = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
  size_t i = 0;

  while(i < source.size()) {
    size_t block_bytes = 0;
    size_t j = i;
    while(j < source.size()) {
      const charport_strview record = source.view(j);
      const size_t len = record.is_na() ? 0 : static_cast<size_t>(record.len);
      if(len > max_block - block_bytes) {
        break;
      }
      block_bytes += len;
      ++j;
    }

    if(block_bytes > 0) {
      char * write_ptr = out.slices.push_front(block_bytes);
      for(size_t k = i; k < j; ++k) {
        const charport_strview record = source.view(k);
        if(record.is_na()) {
          continue;
        }
        if(record.len == 0) {
          out.records.set(k, cpc::empty_data(), 0, record.enc);
          continue;
        }
        std::memcpy(write_ptr, record.ptr, static_cast<size_t>(record.len));
        out.records.set(k, write_ptr, record.len, record.enc);
        write_ptr += record.len;
      }
    } else {
      for(size_t k = i; k < j; ++k) {
        const charport_strview record = source.view(k);
        if(!record.is_na() && record.len == 0) {
          out.records.set(k, cpc::empty_data(), 0, record.enc);
        }
      }
    }
    i = (j > i) ? j : i + 1;
  }

  return out;
}

inline cpv::Store deep_copy_store(const cpv::Store & source) {
  return rebuild_store(source.records);
}

// The private ALTREP wrapper consumes a heap Store.  Returning the pointer
// from these native-only helpers lets every Builder/temporary Store finish
// before R_ExecWithCleanup starts.
inline cpv::Store * heap_store(cpv::Store && source) {
  return new cpv::Store(std::move(source));
}

inline cpv::Store * store_from_chars(R_xlen_t n, const SEXP * ptr) {
  cpv::Builder builder(n);
  for(R_xlen_t i = 0; i < n; ++i) {
    builder.set(i, cpi::charsxp_to_view(ptr[i]));
  }
  cpv::Store store = builder.release_store();
  return heap_store(std::move(store));
}

inline cpv::Store * duplicate_store(const cpv::Store & source) {
  cpv::Store store = deep_copy_store(source);
  return heap_store(std::move(store));
}

inline cpv::Store * subset_store(
    const cpv::Store & source, R_xlen_t source_length, R_xlen_t output_length,
    const int * integer_indices, const double * real_indices) {
  cpv::Builder out(output_length);
  auto copy_element = [&](size_t out_i, R_xlen_t zero_based) {
    if(zero_based < 0 || zero_based >= source_length) {
      return;
    }
    const charport_strview record = source.view(static_cast<size_t>(zero_based));
    if(!record.is_na()) {
      out.set(static_cast<R_xlen_t>(out_i), record);
    }
  };

  if(integer_indices != nullptr) {
    for(R_xlen_t i = 0; i < output_length; ++i) {
      if(integer_indices[i] != NA_INTEGER) {
        copy_element(static_cast<size_t>(i),
                     static_cast<R_xlen_t>(integer_indices[i]) - 1);
      }
    }
  } else {
    for(R_xlen_t i = 0; i < output_length; ++i) {
      const double one_based = real_indices[i];
      // A real subscript truncates, as in R, so 3.5 selects element 3.
      if(R_FINITE(one_based) && one_based >= 1.0 &&
         one_based < static_cast<double>(source_length) + 1.0) {
        copy_element(static_cast<size_t>(i),
                     static_cast<R_xlen_t>(one_based) - 1);
      }
    }
  }

  cpv::Store store = out.release_store();
  return heap_store(std::move(store));
}

inline cpv::Store * unserialize_store(const charvec_serialized_layout & layout) {
  charvec_check_records(layout);
  const unsigned char * size_offset = layout.size_offset;
  const unsigned char * data_offset = layout.data_offset;

  // A CPV2 state's native-tagged records were written in a UTF-8 locale (see
  // Serialized_state); outside one, say so explicitly. Checked only once a
  // native record turns up. CPV1 keeps native as the current locale's.
  int native_as_utf8 = layout.native_is_utf8_text ? -1 : 0;
  cpv::Builder out(static_cast<R_xlen_t>(layout.n));
  for(size_t i = 0; i < layout.n; ++i) {
    const size_t size = charvec_read_u32_advance(size_offset, layout.swap);
    cetype_ext_t encoding = charport_cetype_ext(layout.enc_offset[i]);
    if(encoding == CETYPE_EXT_NATIVE && native_as_utf8 != 0) {
      if(native_as_utf8 < 0) {
        native_as_utf8 = !charport_native_is_utf8_impl();
      }
      if(native_as_utf8) {
        encoding = CETYPE_EXT_UTF8;
      }
    }
    if(encoding == CETYPE_EXT_NA) {
      out.set_na(static_cast<R_xlen_t>(i));
    } else {
      out.set(static_cast<R_xlen_t>(i), reinterpret_cast<const char *>(data_offset), size, encoding);
    }
    data_offset += size;
  }
  cpv::Store store = out.release_store();
  return heap_store(std::move(store));
}

inline char * store_reserve(cpv::Store & store, size_t idx, size_t len,
                            cetype_ext_t enc) {
  const int stored_len = cpc::checked_string_size(len, "stored string length");
  if(idx >= store.records.size()) {
    throw std::runtime_error("charvec store assignment out of bounds");
  }

  const charport_strview current = store.records.view(idx);
  if(enc == CETYPE_EXT_NA) {
    store.records.set_na(idx);
    return nullptr;
  }
  if(stored_len == 0) {
    store.records.set(idx, cpc::empty_data(), 0, enc);
    return const_cast<char*>(cpc::empty_data());
  }
  if(!current.is_na() && current.len >= stored_len) {
    store.records.set(idx, current.ptr, stored_len, enc);
    return const_cast<char*>(current.ptr);
  }

  char * dest = store.slices.push_front(static_cast<size_t>(stored_len));
  store.records.set(idx, dest, stored_len, enc);
  return dest;
}

// Keep the Elt cache in sync with the Store. Write through an allocated
// chunk, or leave an untouched chunk as R_NilValue. Mutations go through
// string_Set_elt.
inline void store_assign(cpv::Store & store, size_t idx,
                         const charport_strview & value) {
  const size_t len = value.is_na() ? 0 : static_cast<size_t>(value.len);
  char * dest = store_reserve(store, idx, len, value.enc);
  if(dest != nullptr && len > 0) {
    std::memmove(dest, value.ptr, len);
  }
}

inline void compact_store(cpv::Store & store) {
  store = rebuild_store(store.records);
}

} // namespace charvec_detail

struct charvec_altrep {
  static R_altrep_class_t class_t;

  // The Store is the source of truth. The Elt cache only roots CHARSXPs so
  // STRING_ELT results stay alive across later allocations; it must stay in
  // sync with the Store. It is a VECSXP of 1024-element STRSXP chunks in
  // R_ExternalPtrProtected(data1). R_BlankString is a miss (empty strings
  // are reminted). Chunks are allocated on first touch. data2 stays
  // R_NilValue until full materialization.
  static constexpr R_xlen_t chunk_shift = 10;
  static constexpr R_xlen_t chunk_size = R_xlen_t(1) << chunk_shift;
  static constexpr R_xlen_t chunk_mask = chunk_size - 1;

  static R_xlen_t chunk_count(R_xlen_t n) noexcept {
    return (n >> chunk_shift) + ((n & chunk_mask) != 0);
  }

  static R_xlen_t chunk_len(R_xlen_t n, R_xlen_t c) noexcept {
    const R_xlen_t remaining = n - (c << chunk_shift);
    return remaining > chunk_size ? chunk_size : remaining;
  }

  static SEXP MoveStore(cpv::Store * source) noexcept {
    if(source == nullptr) {
      Rf_error("charvec MoveStore: source is NULL");
    }
    SEXP xp = PROTECT(R_MakeExternalPtr(nullptr, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(xp, charvec_altrep::Finalize, TRUE);
    SEXP out = PROTECT(R_new_altrep(class_t, xp, R_NilValue));

    // Build the R shell before moving source. After the move, installing the
    // address cannot allocate, so the external pointer owns the Store.
    cpv::Store * data = new(std::nothrow) cpv::Store(std::move(*source));
    if(data == nullptr) {
      Rf_error("charvec MoveStore: could not allocate Store owner");
    }
    R_SetExternalPtrAddr(xp, data);
    UNPROTECT(2);
    return out;
  }

  static void Finalize(SEXP xp) {
    auto * ptr = static_cast<cpv::Store*>(R_ExternalPtrAddr(xp));
    if(ptr == nullptr) {
      return;
    }
    delete ptr;
    R_ClearExternalPtr(xp);
  }

  static cpv::Store * Ptr(SEXP vec) {
    return static_cast<cpv::Store*>(R_ExternalPtrAddr(R_altrep_data1(vec)));
  }

  static cpv::Store & Get(SEXP vec) {
    return *Ptr(vec);
  }

  static R_xlen_t Length(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      return Rf_xlength(data2);
    }
    return static_cast<R_xlen_t>(Get(vec).size());
  }

  static Rboolean Inspect(SEXP x, int /* pre */, int /* deep */, int /* pvec */,
                          void (*/* inspect_subtree */)(SEXP, int, int, int)) {
    bool materialized = R_altrep_data2(x) != R_NilValue;
    Rprintf("charport charvec (len=%llu, ptr=%p, %s)\n",
            static_cast<unsigned long long int>(Length(x)),
            static_cast<void*>(Ptr(x)),
            materialized ? "materialized" : "not materialized");
    return TRUE;
  }

  // The store's strings as a new character vector, leaving the store alone.
  static SEXP strings(const cpv::Store & store) {
    const R_xlen_t n = static_cast<R_xlen_t>(store.size());
    SEXP out = PROTECT(Rf_allocVector(STRSXP, n));
    for(R_xlen_t i = 0; i < n; ++i) {
      SET_STRING_ELT(out, i, cpi::make_charsxp(store.view(static_cast<size_t>(i))));
    }
    UNPROTECT(1);
    return out;
  }

  static void install_materialized(SEXP vec, SEXP xp, SEXP data2) {
    R_set_altrep_data2(vec, data2);
    Finalize(xp);
    R_SetExternalPtrProtected(xp, R_NilValue);
  }

  // Materialize into data2, reusing cached CHARSXPs, then free the Store.
  static SEXP Materialize(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      return data2;
    }
    auto & store = Get(vec);
    const R_xlen_t n = static_cast<R_xlen_t>(store.size());
    SEXP xp = R_altrep_data1(vec);
    SEXP chunks = R_ExternalPtrProtected(xp);

    // A single allocated chunk is already length n; promote it.
    if(chunks != R_NilValue && Rf_xlength(chunks) == 1) {
      SEXP chunk = VECTOR_ELT(chunks, 0);
      if(chunk != R_NilValue) {
        data2 = chunk;
        const SEXP * p = STRING_PTR_RO(data2);
        for(R_xlen_t i = 0; i < n; ++i) {
          if(p[i] == R_BlankString) {
            SET_STRING_ELT(data2, i, cpi::make_charsxp(store.view(static_cast<size_t>(i))));
          }
        }
        install_materialized(vec, xp, data2);
        return data2;
      }
    }

    data2 = PROTECT(Rf_allocVector(STRSXP, n));
    if(chunks == R_NilValue) {
      for(R_xlen_t i = 0; i < n; ++i) {
        SET_STRING_ELT(data2, i, cpi::make_charsxp(store.view(static_cast<size_t>(i))));
      }
    } else {
      const R_xlen_t n_chunks = Rf_xlength(chunks);
      for(R_xlen_t c = 0; c < n_chunks; ++c) {
        SEXP chunk = VECTOR_ELT(chunks, c);
        const R_xlen_t start = c << chunk_shift;
        const R_xlen_t end = start + chunk_len(n, c);
        if(chunk == R_NilValue) {
          for(R_xlen_t i = start; i < end; ++i) {
            SET_STRING_ELT(data2, i, cpi::make_charsxp(store.view(static_cast<size_t>(i))));
          }
          continue;
        }
        const SEXP * src = STRING_PTR_RO(chunk);
        for(R_xlen_t i = start, j = 0; i < end; ++i, ++j) {
          SEXP elt = src[j];
          if(elt == R_BlankString) {
            elt = cpi::make_charsxp(store.view(static_cast<size_t>(i)));
          }
          SET_STRING_ELT(data2, i, elt);
        }
      }
    }
    install_materialized(vec, xp, data2);
    UNPROTECT(1);
    return data2;
  }

  static SEXP Duplicate(SEXP vec, Rboolean /* deep */) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      // materialized: duplicate the cached STRSXP, sharing CHARSXPs. The copy
      // is an ordinary character vector; R's DuplicateEX copies attributes.
      return Rf_duplicate(data2);
    }
    return charport_sexp_guard("charvec Duplicate", [&]() -> SEXP {
      cpv::Store * store = charvec_detail::duplicate_store(Get(vec));
      return charvec_detail::wrap_store_with_cleanup(store);
    });
  }

  static const void * Dataptr_or_null(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 == R_NilValue) {
      return nullptr;
    }
    return STRING_PTR_RO(data2);
  }

  static SEXP MaterializeGuarded(SEXP vec) {
    return charport_sexp_guard("charvec materialize", [&]() -> SEXP {
      return Materialize(vec);
    });
  }

  static void * Dataptr(SEXP vec, Rboolean /* writeable */) {
    return const_cast<void*>(static_cast<const void*>(STRING_PTR_RO(MaterializeGuarded(vec))));
  }

  static SEXP EltChunk(SEXP vec, R_xlen_t i) {
    SEXP xp = R_altrep_data1(vec);
    SEXP chunks = R_ExternalPtrProtected(xp);
    const R_xlen_t c = i >> chunk_shift;
    if(chunks != R_NilValue) {
      SEXP chunk = VECTOR_ELT(chunks, c);
      if(chunk != R_NilValue) {
        return chunk;
      }
      const R_xlen_t n = static_cast<R_xlen_t>(Get(vec).size());
      chunk = PROTECT(Rf_allocVector(STRSXP, chunk_len(n, c)));
      SET_VECTOR_ELT(chunks, c, chunk);
      UNPROTECT(1);
      return chunk;
    }

    const R_xlen_t n = static_cast<R_xlen_t>(Get(vec).size());
    chunks = PROTECT(Rf_allocVector(VECSXP, chunk_count(n)));
    R_SetExternalPtrProtected(xp, chunks);
    SEXP chunk = PROTECT(Rf_allocVector(STRSXP, chunk_len(n, c)));
    SET_VECTOR_ELT(chunks, c, chunk);
    UNPROTECT(2);
    return chunk;
  }

  static SEXP string_Elt(SEXP vec, R_xlen_t i) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      return STRING_ELT(data2, i);
    }
    SEXP chunk = EltChunk(vec, i);
    const R_xlen_t j = i & chunk_mask;
    SEXP elt = STRING_ELT(chunk, j);
    if(elt != R_BlankString) {
      return elt;
    }
    // EltChunk roots chunk through vec's external pointer; protect explicitly
    // because rchk cannot see that relationship across the helper call.
    PROTECT(chunk);
    elt = cpi::make_charsxp(Get(vec).view(static_cast<size_t>(i)));
    if(elt != R_BlankString) {
      SET_STRING_ELT(chunk, j, elt);
    }
    UNPROTECT(1);
    return elt;
  }

  static void string_Set_elt(SEXP vec, R_xlen_t i, SEXP new_val) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      SET_STRING_ELT(data2, i, new_val);
      return;
    }
    charport_sexp_guard("charvec Set_elt", [&]() -> SEXP {
      charvec_detail::store_assign(
        Get(vec), static_cast<size_t>(i), cpi::charsxp_to_view(new_val));
      SEXP chunks = R_ExternalPtrProtected(R_altrep_data1(vec));
      if(chunks != R_NilValue) {
        SEXP chunk = VECTOR_ELT(chunks, i >> chunk_shift);
        if(chunk != R_NilValue) {
          SET_STRING_ELT(chunk, i & chunk_mask, new_val);
        }
      }
      return R_NilValue;
    });
  }

  static int no_NA(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      const R_xlen_t len = Rf_xlength(data2);
      const SEXP * p = STRING_PTR_RO(data2);
      for(R_xlen_t i = 0; i < len; ++i) {
        if(p[i] == NA_STRING) {
          return 0;
        }
      }
      return 1;
    }
    auto & data1 = Get(vec);
    for(size_t i = 0; i < data1.size(); ++i) {
      if(data1.view(i).is_na()) {
        return 0;
      }
    }
    return 1;
  }

  // Subset copies records store-to-store: one memcpy per element, no
  // intermediate owned-string temporary. A materialized charvec falls back to
  // the default subset over the cached STRSXP, which shares CHARSXPs. R
  // normalizes subscripts to INTSXP/REALSXP before this method; anything else
  // also falls back to the default implementation.
  static SEXP Extract_subset(SEXP x, SEXP indx, SEXP call) {
    (void)call;
    if(R_altrep_data2(x) != R_NilValue) {
      return nullptr;
    }
    if(TYPEOF(indx) != INTSXP && TYPEOF(indx) != REALSXP) {
      return nullptr;
    }
    return charport_sexp_guard("charvec Extract_subset", [&]() -> SEXP {
      const R_xlen_t xlen = static_cast<R_xlen_t>(Get(x).size());
      const R_xlen_t len = Rf_xlength(indx);
      const int * integer_indices = nullptr;
      const double * real_indices = nullptr;
      if(TYPEOF(indx) == INTSXP) {
        // Acquire the possibly materializing index before native output owns
        // anything, so an R error cannot bypass a Store cleanup boundary.
        integer_indices = INTEGER(indx);
      } else {
        real_indices = REAL(indx);
      }
      cpv::Store * store = charvec_detail::subset_store(
        Get(x), xlen, len, integer_indices, real_indices);
      return charvec_detail::wrap_store_with_cleanup(store);
    });
  }

  static SEXP Serialized_state(SEXP vec) {
    return charport_sexp_guard("charvec Serialized_state", [&]() -> SEXP {
      SEXP data2 = R_altrep_data2(vec);
      if(data2 != R_NilValue) {
        // materialized: serialize the plain STRSXP; unserializes to one too
        return data2;
      }
      auto & data1 = Get(vec);
      const size_t n = data1.size();
      // A packed state's native-tagged records are UTF-8 by construction:
      // outside a UTF-8 locale, send a temporary character vector instead,
      // which R serializes with the stream's native encoding.
      for(size_t i = 0; i < n; ++i) {
        if(data1.encoding(i) == CETYPE_EXT_NATIVE) {
          if(!charport_native_is_utf8_impl()) {
            return strings(data1);
          }
          break;
        }
      }
      size_t total_size = 0;
      for(size_t i = 0; i < n; ++i) {
        const int len = data1.length(i);
        total_size = cpi::checked_add_size(
          total_size, len < 0 ? 0 : static_cast<size_t>(len), "serialized_state payload");
      }
      const size_t total_bytes = charvec_serialized_length(n, total_size);
      SEXP serialized_state = Rf_allocVector(RAWSXP, static_cast<R_xlen_t>(total_bytes));
      unsigned char * serialized_ptr = RAW(serialized_state);
      serialized_ptr[0] = 'C';
      serialized_ptr[1] = 'P';
      serialized_ptr[2] = 'V';
      serialized_ptr[3] = '2';
      serialized_ptr[4] = 1;  // little-endian
      charvec_write_u64_le(serialized_ptr + charvec_serialized_prefix_bytes(),
                           static_cast<uint64_t>(n));
      unsigned char * current_offset =
        serialized_ptr + charvec_serialized_prefix_bytes() + sizeof(uint64_t);

      for(size_t i = 0; i < n; ++i) {
        const int len = data1.length(i);
        charvec_write_u32_le(current_offset, len < 0 ? 0U : static_cast<uint32_t>(len));
        current_offset += sizeof(uint32_t);
      }

      for(size_t i = 0; i < n; ++i) {
        const uint8_t encoding = data1.encoding(i).value;
        std::memcpy(current_offset, &encoding, sizeof(uint8_t));
        current_offset += sizeof(uint8_t);
      }

      for(size_t i = 0; i < n; ++i) {
        const charport_strview rec = data1.view(i);
        if(rec.len > 0) {
          std::memcpy(current_offset, rec.ptr, static_cast<size_t>(rec.len));
          current_offset += static_cast<size_t>(rec.len);
        }
      }
      return serialized_state;
    });
  }

  static SEXP Unserialize(SEXP /* class */, SEXP serialized_state) {
    return charport_sexp_guard("charvec Unserialize", [&]() -> SEXP {
      if(TYPEOF(serialized_state) == STRSXP) {
        // state from a materialized charvec, or one saved with native records
        // outside a UTF-8 locale: stays a plain character vector
        return serialized_state;
      }
      charvec_serialized_layout layout = charvec_parse_serialized(serialized_state);
      cpv::Store * store = charvec_detail::unserialize_store(layout);
      return charvec_detail::wrap_store_with_cleanup(store);
    });
  }

  // Broker hooks used when the reference class is registered.
  // init borrows the store pointer that already hangs off data1; a materialized
  // charvec has released its store, so init returns NULL and the broker serves
  // it via the direct path over the cached data2.
  static void * reader_init(SEXP x) {
    if(R_altrep_data2(x) != R_NilValue) {
      return nullptr;
    }
    return Ptr(x);
  }

  static int reader_strviews_range(
      void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
      int * out_lens, cetype_ext_t * out_encs) {
    if(size == 0) {
      return CHARPORT_STATUS_OK;
    }
    const cpv::Store * data = static_cast<cpv::Store *>(state);
    const size_t offset = static_cast<size_t>(start);
    const size_t count = static_cast<size_t>(size);
    std::memcpy(out_ptrs, data->records.ptrs() + offset,
                count * sizeof(const char *));
    std::memcpy(out_lens, data->records.lengths() + offset,
                count * sizeof(int));
    std::memcpy(out_encs, data->records.encodings() + offset,
                count * sizeof(cetype_ext_t));
    return CHARPORT_STATUS_OK;
  }

  static int reader_strviews_index(
      void * state, const R_xlen_t * indices, R_xlen_t size,
      const char ** out_ptrs, int * out_lens, cetype_ext_t * out_encs) {
    const cpv::Store * data = static_cast<cpv::Store *>(state);
    for(R_xlen_t j = 0; j < size; ++j) {
      const size_t i = static_cast<size_t>(indices[j]);
      out_ptrs[j] = data->records.ptrs()[i];
      out_lens[j] = data->records.lengths()[i];
      out_encs[j] = data->records.encodings()[i];
    }
    return CHARPORT_STATUS_OK;
  }

  static int reader_byteviews_range(
      void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
      int * out_lens) {
    if(size == 0) {
      return CHARPORT_STATUS_OK;
    }
    const cpv::Store * data = static_cast<cpv::Store *>(state);
    const size_t offset = static_cast<size_t>(start);
    const size_t count = static_cast<size_t>(size);
    std::memcpy(out_ptrs, data->records.ptrs() + offset,
                count * sizeof(const char *));
    std::memcpy(out_lens, data->records.lengths() + offset,
                count * sizeof(int));
    return CHARPORT_STATUS_OK;
  }

  static int reader_byteviews_index(
      void * state, const R_xlen_t * indices, R_xlen_t size,
      const char ** out_ptrs, int * out_lens) {
    const cpv::Store * data = static_cast<cpv::Store *>(state);
    for(R_xlen_t j = 0; j < size; ++j) {
      const size_t i = static_cast<size_t>(indices[j]);
      out_ptrs[j] = data->records.ptrs()[i];
      out_lens[j] = data->records.lengths()[i];
    }
    return CHARPORT_STATUS_OK;
  }

  static int reader_lengths_range(
      void * state, R_xlen_t start, R_xlen_t size, int * out_lens) {
    if(size == 0) {
      return CHARPORT_STATUS_OK;
    }
    const cpv::Store * data = static_cast<cpv::Store *>(state);
    const size_t offset = static_cast<size_t>(start);
    const size_t count = static_cast<size_t>(size);
    std::memcpy(out_lens, data->records.lengths() + offset,
                count * sizeof(int));
    return CHARPORT_STATUS_OK;
  }

  static int reader_lengths_index(
      void * state, const R_xlen_t * indices, R_xlen_t size, int * out_lens) {
    const cpv::Store * data = static_cast<cpv::Store *>(state);
    for(R_xlen_t j = 0; j < size; ++j) {
      const size_t i = static_cast<size_t>(indices[j]);
      out_lens[j] = data->records.lengths()[i];
    }
    return CHARPORT_STATUS_OK;
  }

  static int reader_encodings_range(
      void * state, R_xlen_t start, R_xlen_t size, cetype_ext_t * out_encs) {
    if(size == 0) {
      return CHARPORT_STATUS_OK;
    }
    const cpv::Store * data = static_cast<cpv::Store *>(state);
    const size_t offset = static_cast<size_t>(start);
    const size_t count = static_cast<size_t>(size);
    std::memcpy(out_encs, data->records.encodings() + offset,
                count * sizeof(cetype_ext_t));
    return CHARPORT_STATUS_OK;
  }

  static int reader_encodings_index(
      void * state, const R_xlen_t * indices, R_xlen_t size,
      cetype_ext_t * out_encs) {
    const cpv::Store * data = static_cast<cpv::Store *>(state);
    for(R_xlen_t j = 0; j < size; ++j) {
      const size_t i = static_cast<size_t>(indices[j]);
      out_encs[j] = data->records.encodings()[i];
    }
    return CHARPORT_STATUS_OK;
  }

  static void Init(DllInfo * dll) {
    // class + package names are baked into serialized objects, so they are
    // final from the first CRAN release on
    class_t = R_make_altstring_class("charvec", "charport", dll);

    R_set_altrep_Serialized_state_method(class_t, Serialized_state);
    R_set_altrep_Unserialize_method(class_t, Unserialize);
    R_set_altrep_Duplicate_method(class_t, Duplicate);

    R_set_altrep_Length_method(class_t, Length);
    R_set_altrep_Inspect_method(class_t, Inspect);

    R_set_altvec_Dataptr_method(class_t, Dataptr);
    R_set_altvec_Dataptr_or_null_method(class_t, Dataptr_or_null);

    R_set_altstring_Elt_method(class_t, string_Elt);
    R_set_altstring_Set_elt_method(class_t, string_Set_elt);
    R_set_altstring_No_NA_method(class_t, no_NA);

    R_set_altvec_Extract_subset_method(class_t, Extract_subset);
  }
};

namespace charvec_detail {

// This context carries the consumed heap owner as a raw pointer; it is
// trivially destructible because R_ExecWithCleanup may bypass ordinary C++
// stack unwinding.
struct store_wrap_state {
  cpv::Store * pending;
};

// R APIs below either return normally or unwind through R's cleanup context;
// none can propagate a C++ exception through that context.
inline SEXP store_wrap_exec(void * data) noexcept {
  store_wrap_state * state = static_cast<store_wrap_state *>(data);
  SEXP xp = PROTECT(R_MakeExternalPtr(nullptr, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(xp, charvec_altrep::Finalize, TRUE);
  SEXP out = PROTECT(R_new_altrep(charvec_altrep::class_t, xp, R_NilValue));

  // Transfer ownership only after every allocation above has succeeded.
  R_SetExternalPtrAddr(xp, state->pending);
  state->pending = nullptr;
  UNPROTECT(2);
  return out;
}

inline void store_wrap_cleanup(void * data) noexcept {
  store_wrap_state * state = static_cast<store_wrap_state *>(data);
  // The finalizer owns the Store after a successful transfer; delete nullptr
  // on that path and consume the pending heap owner on an R error.
  delete state->pending;
  state->pending = nullptr;
}

inline SEXP wrap_store_with_cleanup(cpv::Store * source) noexcept {
  if(source == nullptr) {
    Rf_error("charvec wrap: source is NULL");
  }
  store_wrap_state state{source};
  return R_ExecWithCleanup(
    &store_wrap_exec, &state, &store_wrap_cleanup, &state);
}

} // namespace charvec_detail

#endif
