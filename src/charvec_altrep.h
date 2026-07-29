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
  switch(enc) {
  case cetype_ext_t::CE_ASCII:
    return CE_NATIVE;
  case cetype_ext_t::CE_UTF8:
  case cetype_ext_t::CE_ASCII_OR_UTF8:
    return CE_UTF8;
  case cetype_ext_t::CE_LATIN1:
    return CE_LATIN1;
  case cetype_ext_t::CE_BYTES:
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
    return cetype_ext_t::CE_UTF8;
  case CE_LATIN1:
    return cetype_ext_t::CE_LATIN1;
  case CE_BYTES:
    return cetype_ext_t::CE_BYTES;
  default:
    return charsxp_is_ascii(x) ? cetype_ext_t::CE_ASCII
                               : cetype_ext_t::CE_NATIVE;
  }
}

inline charport_strview charsxp_to_view(SEXP x) {
  if(x == NA_STRING) {
    return make_strview(nullptr, NA_INTEGER, cetype_ext_t::CE_NA);
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

// Native-endian serialized_state with a magic/version prefix and endian flag.
struct charvec_serialized_layout {
  size_t n = 0;
  bool swap = false;
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

inline size_t charvec_serialized_prefix_bytes() noexcept {
  return 5;  // "CPV1" + endian flag
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
     serialized_ptr[2] != 'V' || serialized_ptr[3] != '1') {
    throw std::runtime_error("invalid serialized_state format");
  }
  const unsigned char source_endian = serialized_ptr[4];
  if(source_endian != 1 && source_endian != 2) {
    throw std::runtime_error("invalid serialized_state endian flag");
  }
  const bool swap = source_endian != charvec_native_endian_flag();

  const uint64_t n64 = charvec_read_u64(serialized_ptr + prefix_bytes, swap);
  if(n64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
     n64 > static_cast<uint64_t>(std::numeric_limits<R_xlen_t>::max())) {
    throw std::runtime_error("serialized_state length exceeds R_xlen_t");
  }

  const size_t n = static_cast<size_t>(n64);
  const size_t sizes_bytes = cpi::checked_mul_size(n, sizeof(uint32_t), "serialized_state header");
  const size_t enc_bytes = cpi::checked_mul_size(n, sizeof(uint8_t), "serialized_state header");
  const size_t header_bytes = cpi::checked_add_size(
    cpi::checked_add_size(
      cpi::checked_add_size(prefix_bytes, sizeof(uint64_t), "serialized_state header"),
      sizes_bytes,
      "serialized_state header"),
    enc_bytes,
    "serialized_state header"
  );

  if(raw_len < header_bytes) {
    throw std::runtime_error("serialized_state header is truncated");
  }

  charvec_serialized_layout layout;
  layout.n = n;
  layout.swap = swap;
  layout.size_offset = serialized_ptr + prefix_bytes + sizeof(uint64_t);
  layout.enc_offset = layout.size_offset + sizes_bytes;
  layout.data_offset = layout.enc_offset + enc_bytes;
  layout.data_end = serialized_ptr + raw_len;
  return layout;
}

inline const char * charvec_serialized_payload(const unsigned char *& data_offset,
                                               const unsigned char * data_end,
                                               size_t len) {
  const size_t remaining = static_cast<size_t>(data_end - data_offset);
  if(len > remaining) {
    throw std::runtime_error("serialized_state payload is truncated");
  }
  const char * ptr = reinterpret_cast<const char *>(data_offset);
  data_offset += len;
  return ptr;
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

inline char * store_reserve(cpv::Store & store, size_t idx, size_t len,
                            cetype_ext_t enc) {
  const int stored_len = cpc::checked_string_size(len, "stored string length");
  if(idx >= store.records.size()) {
    throw std::runtime_error("charvec store assignment out of bounds");
  }

  const charport_strview current = store.records.view(idx);
  if(enc == cetype_ext_t::CE_NA) {
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

// INVARIANT: mutating a record of a wrapped charvec must keep the Elt cache
// (the STRSXP in the data1 external pointer's protected slot) coherent —
// write the new value through, or re-punch the hole with R_BlankString.
// Every mutation currently funnels through charvec_altrep::string_Set_elt,
// which writes through; a new caller of store_assign must do the same.
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

  // Full materialization, cached in data2. The store is freed afterwards
  // because records would duplicate the cached R strings.
  static SEXP Materialize(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      return data2;
    }
    auto & data1 = Get(vec);
    const R_xlen_t n = static_cast<R_xlen_t>(data1.size());
    // Promote the Elt cache instead of converting from scratch: fill the
    // remaining holes from the store and move the vector to data2, so
    // elements string_Elt already minted are not converted twice. Filling a
    // hole and re-filling a genuinely empty element are the same write, so
    // the R_BlankString marker stays unambiguous.
    data2 = PROTECT(EltCache(vec));
    for(R_xlen_t i = 0; i < n; ++i) {
      if(STRING_ELT(data2, i) == R_BlankString) {
        SET_STRING_ELT(data2, i, cpi::make_charsxp(data1.view(static_cast<size_t>(i))));
      }
    }
    R_set_altrep_data2(vec, data2);
    Finalize(R_altrep_data1(vec));
    // data2 serves every later access; the external pointer's handle on the
    // same vector would only pin a duplicate reference.
    R_SetExternalPtrProtected(R_altrep_data1(vec), R_NilValue);
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
      cpv::Store store = charvec_detail::deep_copy_store(Get(vec));
      return MoveStore(&store);
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

  // Lazy per-element CHARSXP cache backing string_Elt, kept in the protected
  // slot of the data1 external pointer so the GC traces it. This is NOT
  // materialization: data2 stays R_NilValue, so Dataptr_or_null, Duplicate,
  // and Serialized_state still see an unmaterialized charvec. R initializes
  // STRSXP elements to R_BlankString, which doubles as the hole marker:
  // "" and NA_STRING are permanent singletons that never need rooting, so
  // an empty-string element can safely be re-minted on every access.
  // The cache length is fixed at creation, which is sound because a wrapped
  // store's record count is immutable (store_assign is bounds-checked and no
  // post-wrap API grows records; R cannot resize a vector in place). Record
  // mutations must keep the cache coherent — see store_assign.
  static SEXP EltCache(SEXP vec) {
    SEXP xp = R_altrep_data1(vec);
    SEXP cache = R_ExternalPtrProtected(xp);
    if(cache == R_NilValue) {
      cache = Rf_allocVector(STRSXP, static_cast<R_xlen_t>(Get(vec).size()));
      R_SetExternalPtrProtected(xp, cache);
    }
    return cache;
  }

  static SEXP string_Elt(SEXP vec, R_xlen_t i) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      return STRING_ELT(data2, i);
    }
    // Callers may hold Elt results across allocations, relying on the source
    // vector to keep them alive (true of any ordinary STRSXP), so a CHARSXP
    // minted from the store must be rooted before it is handed out. Root it
    // in the per-element cache rather than materializing: staying lazy under
    // Elt is part of the charvec contract.
    return charport_sexp_guard("charvec Elt", [&]() -> SEXP {
      SEXP cache = EltCache(vec);
      SEXP elt = STRING_ELT(cache, i);
      if(elt == R_BlankString) {
        elt = cpi::make_charsxp(Get(vec).view(static_cast<size_t>(i)));
        if(elt != R_BlankString) {
          SET_STRING_ELT(cache, i, elt);
        }
      }
      return elt;
    });
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
      SEXP cache = R_ExternalPtrProtected(R_altrep_data1(vec));
      if(cache != R_NilValue) {
        SET_STRING_ELT(cache, i, new_val);  // keep the Elt cache coherent
      }
      return R_NilValue;
    });
  }

  static int no_NA(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      const R_xlen_t len = Rf_xlength(data2);
      for(R_xlen_t i = 0; i < len; ++i) {
        if(STRING_ELT(data2, i) == NA_STRING) {
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

      charport::charvec::Builder out(len);
      auto copy_element = [&](size_t out_i, R_xlen_t zero_based) {
        if(zero_based < 0 || zero_based >= xlen) {
          return;
        }
        const charport_strview rec = Get(x).view(static_cast<size_t>(zero_based));
        if(!rec.is_na()) {
          out.set(static_cast<R_xlen_t>(out_i), rec);
        }
      };

      if(TYPEOF(indx) == INTSXP) {
        const int * idx = INTEGER(indx);
        for(R_xlen_t i = 0; i < len; ++i) {
          if(idx[i] != NA_INTEGER) {
            copy_element(static_cast<size_t>(i), static_cast<R_xlen_t>(idx[i]) - 1);
          }
        }
      } else {
        const double * idx = REAL(indx);
        for(R_xlen_t i = 0; i < len; ++i) {
          const double one_based = idx[i];
          if(R_FINITE(one_based) && one_based >= 1.0 &&
             one_based <= static_cast<double>(xlen)) {
            copy_element(static_cast<size_t>(i),
                         static_cast<R_xlen_t>(one_based) - 1);
          }
        }
      }

      cpv::Store store = out.release_store();
      return MoveStore(&store);
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
      serialized_ptr[3] = '1';
      serialized_ptr[4] = charvec_native_endian_flag();
      const uint64_t n64 = static_cast<uint64_t>(n);
      std::memcpy(serialized_ptr + charvec_serialized_prefix_bytes(), &n64, sizeof(uint64_t));
      unsigned char * current_offset =
        serialized_ptr + charvec_serialized_prefix_bytes() + sizeof(uint64_t);

      for(size_t i = 0; i < n; ++i) {
        const int len = data1.length(i);
        const uint32_t size = len < 0 ? 0U : static_cast<uint32_t>(len);
        std::memcpy(current_offset, &size, sizeof(uint32_t));
        current_offset += sizeof(uint32_t);
      }

      for(size_t i = 0; i < n; ++i) {
        const uint8_t encoding = static_cast<uint8_t>(data1.encoding(i));
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
        // state from a materialized charvec: stays a plain character vector
        return serialized_state;
      }
      charvec_serialized_layout layout = charvec_parse_serialized(serialized_state);

      const unsigned char * size_offset = layout.size_offset;
      const unsigned char * enc_offset = layout.enc_offset;
      const unsigned char * data_offset = layout.data_offset;

      charport::charvec::Builder out(static_cast<R_xlen_t>(layout.n));
      for(size_t i = 0; i < layout.n; ++i) {
        const uint32_t size = charvec_read_u32_advance(size_offset, layout.swap);
        if(!cpi::check_r_string_len(size)) {
          throw std::runtime_error("serialized string size exceeds R string size");
        }
        const cetype_ext_t encoding = static_cast<cetype_ext_t>(*enc_offset++);
        const size_t stored_len = static_cast<size_t>(size);
        const char * payload = charvec_serialized_payload(data_offset, layout.data_end, stored_len);
        // Untrusted input: accept every encoding a record can legitimately
        // hold (the store keeps encodings verbatim, so any of these can have
        // been serialized) and reject only an out-of-range encoding byte.
        switch(encoding) {
        case cetype_ext_t::CE_ASCII:
        case cetype_ext_t::CE_UTF8:
        case cetype_ext_t::CE_ASCII_OR_UTF8:
        case cetype_ext_t::CE_LATIN1:
        case cetype_ext_t::CE_NATIVE:
        case cetype_ext_t::CE_BYTES:
          out.set(static_cast<R_xlen_t>(i), payload, stored_len, encoding);
          break;
        case cetype_ext_t::CE_NA:
          if(stored_len != 0) {
            throw std::runtime_error("serialized NA string must have zero length");
          }
          out.set_na(static_cast<R_xlen_t>(i));
          break;
        default:
          throw std::runtime_error("invalid string encoding in serialized_state");
        }
      }

      if(data_offset != layout.data_end) {
        throw std::runtime_error("serialized_state has trailing bytes");
      }
      cpv::Store store = out.release_store();
      return MoveStore(&store);
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

#endif
