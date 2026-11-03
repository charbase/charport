#include "charpack_altrep.h"
#include "charport_registry.h"

#include <cstring>

// Fills validate every input before allocating, then allocate the RAWSXP and
// install it last, so an error leaves x unfilled. They hold no C++ objects
// with destructors while R can raise an error.

namespace {

void check_unfilled(SEXP x, const char * op) {
  if(!charpack_altrep::is(x)) {
    Rf_error("charport charpack %s: x must be a charpack", op);
  }
  if(!charpack_altrep::is_unfilled(x)) {
    Rf_error("charport charpack %s: x is not an unfilled placeholder", op);
  }
}

bool record_is_na(int len, cetype_ext_t enc) noexcept {
  return len == NA_INTEGER || enc == CETYPE_EXT_NA;
}

// Checks lengths and encodings and returns the payload size.
size_t checked_payload(R_xlen_t n, const char * const * ptrs, const int * lengths,
                       const cetype_ext_t * encodings, const char * op) {
  size_t total = 0;
  for(R_xlen_t i = 0; i < n; ++i) {
    if(!cpi::valid_view_encoding(encodings[i])) {
      Rf_error("charport charpack %s: invalid encoding at index %lld",
               op, static_cast<long long>(i));
    }
    const bool na = ptrs == nullptr ? record_is_na(lengths[i], encodings[i])
                                    : cpi::view_is_na(ptrs[i], lengths[i], encodings[i]);
    if(na) {
      continue;
    }
    if(lengths[i] < 0) {
      Rf_error("charport charpack %s: negative length at index %lld",
               op, static_cast<long long>(i));
    }
    const size_t len = static_cast<size_t>(lengths[i]);
    if(total > static_cast<size_t>(-1) - len) {
      Rf_error("charport charpack %s: total size overflows", op);
    }
    total += len;
  }
  return total;
}

// Allocates data1 and writes n, sizes and encodings. Returns it protected.
SEXP alloc_data1(R_xlen_t n, const char * const * ptrs, const int * lengths,
                 const cetype_ext_t * encodings, size_t payload, const char * op) {
  const size_t count = static_cast<size_t>(n);
  const size_t meta_limit = (static_cast<size_t>(R_XLEN_T_MAX) - charpack_layout::header_bytes) /
                            charpack_layout::element_bytes;
  if(count > meta_limit ||
     payload > static_cast<size_t>(R_XLEN_T_MAX) - charpack_layout::header_bytes -
               count * charpack_layout::element_bytes) {
    Rf_error("charport charpack %s: vector exceeds the maximum raw vector size", op);
  }
  const size_t total = charpack_layout::header_bytes + count * charpack_layout::element_bytes + payload;
  SEXP data1 = PROTECT(Rf_allocVector(RAWSXP, static_cast<R_xlen_t>(total)));
  unsigned char * p = RAW(data1);
  const uint64_t n64 = static_cast<uint64_t>(n);
  std::memcpy(p, &n64, sizeof(n64));
  unsigned char * sizes = p + charpack_layout::header_bytes;
  unsigned char * encs = sizes + sizeof(uint32_t) * count;
  for(size_t i = 0; i < count; ++i) {
    const bool na = ptrs == nullptr ? record_is_na(lengths[i], encodings[i])
                                    : cpi::view_is_na(ptrs[i], lengths[i], encodings[i]);
    const uint32_t size = na ? 0U : static_cast<uint32_t>(lengths[i]);
    std::memcpy(sizes + sizeof(uint32_t) * i, &size, sizeof(size));
    encs[i] = na ? CETYPE_EXT_NA.value : encodings[i].value;
  }
  return data1;
}

char * payload_start(SEXP data1, R_xlen_t n) {
  return reinterpret_cast<char *>(RAW(data1)) + charpack_layout::header_bytes +
         static_cast<size_t>(n) * charpack_layout::element_bytes;
}

} // namespace

extern "C" SEXP charport_charpack_placeholder_impl(void) {
  return R_new_altrep(charpack_altrep::class_t, R_NilValue, R_NilValue);
}

extern "C" void charport_charpack_fill_views_impl(
    SEXP x, R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings) {
  const char * op = "fill_views";
  check_unfilled(x, op);
  if(n < 0) {
    Rf_error("charport charpack %s: negative length", op);
  }
  if(n > 0 && (ptrs == nullptr || lengths == nullptr || encodings == nullptr)) {
    Rf_error("charport charpack %s: input arrays must not be NULL", op);
  }
  const size_t payload = checked_payload(n, ptrs, lengths, encodings, op);
  SEXP data1 = alloc_data1(n, ptrs, lengths, encodings, payload, op);
  char * dest = payload_start(data1, n);
  for(R_xlen_t i = 0; i < n; ++i) {
    if(cpi::view_is_na(ptrs[i], lengths[i], encodings[i]) || lengths[i] == 0) {
      continue;
    }
    std::memcpy(dest, ptrs[i], static_cast<size_t>(lengths[i]));
    dest += lengths[i];
  }
  R_set_altrep_data1(x, data1);
  UNPROTECT(1);
}

extern "C" void charport_charpack_fill_slices_impl(
    SEXP x, R_xlen_t n, const int * lengths, const cetype_ext_t * encodings,
    R_xlen_t n_slices, const char * const * slices, const size_t * slice_bytes) {
  const char * op = "fill_slices";
  check_unfilled(x, op);
  if(n < 0 || n_slices < 0) {
    Rf_error("charport charpack %s: negative length", op);
  }
  if(n > 0 && (lengths == nullptr || encodings == nullptr)) {
    Rf_error("charport charpack %s: input arrays must not be NULL", op);
  }
  if(n_slices > 0 && (slices == nullptr || slice_bytes == nullptr)) {
    Rf_error("charport charpack %s: slice arrays must not be NULL", op);
  }
  const size_t payload = checked_payload(n, nullptr, lengths, encodings, op);
  size_t sliced = 0;
  for(R_xlen_t k = 0; k < n_slices; ++k) {
    if(slice_bytes[k] > 0 && slices[k] == nullptr) {
      Rf_error("charport charpack %s: slice %lld is NULL", op, static_cast<long long>(k));
    }
    if(sliced > static_cast<size_t>(-1) - slice_bytes[k]) {
      Rf_error("charport charpack %s: total slice size overflows", op);
    }
    sliced += slice_bytes[k];
  }
  if(sliced != payload) {
    Rf_error("charport charpack %s: slices hold %llu bytes but the lengths need %llu",
             op, static_cast<unsigned long long>(sliced),
             static_cast<unsigned long long>(payload));
  }
  SEXP data1 = alloc_data1(n, nullptr, lengths, encodings, payload, op);
  char * dest = payload_start(data1, n);
  for(R_xlen_t k = 0; k < n_slices; ++k) {
    if(slice_bytes[k] > 0) {
      std::memcpy(dest, slices[k], slice_bytes[k]);
      dest += slice_bytes[k];
    }
  }
  R_set_altrep_data1(x, data1);
  UNPROTECT(1);
}

extern "C" SEXP charport_charpack_fill_materialized_na_impl(SEXP x, R_xlen_t n) {
  const char * op = "fill_materialized_na";
  check_unfilled(x, op);
  if(n < 0) {
    Rf_error("charport charpack %s: negative length", op);
  }
  SEXP strings = PROTECT(Rf_allocVector(STRSXP, n));
  for(R_xlen_t i = 0; i < n; ++i) {
    SET_STRING_ELT(strings, i, NA_STRING);
  }
  R_set_altrep_data2(x, strings);
  UNPROTECT(1);
  return strings;
}

extern "C" int charport_is_charpack_impl(SEXP x) {
  return charpack_altrep::is(x) ? 1 : 0;
}

// R entry points for tests and diagnostics.

extern "C" SEXP C_charpack_placeholder(void) {
  return charport_charpack_placeholder_impl();
}

// Fills x from an ordinary character vector through fill_views, or through
// fill_slices with the payload cut into n_slices pieces at arbitrary byte
// offsets (n_slices <= 0 selects fill_views).
extern "C" SEXP C_charpack_fill_from(SEXP x, SEXP strings, SEXP n_slices_) {
  if(TYPEOF(strings) != STRSXP) {
    Rf_error("charport charpack fill_from: strings must be a character vector");
  }
  const R_xlen_t n = Rf_xlength(strings);
  const int n_slices = Rf_asInteger(n_slices_);
  const SEXP * src = STRING_PTR_RO(strings);
  const char ** ptrs = reinterpret_cast<const char **>(R_alloc(static_cast<size_t>(n) + 1, sizeof(const char *)));
  int * lengths = reinterpret_cast<int *>(R_alloc(static_cast<size_t>(n) + 1, sizeof(int)));
  cetype_ext_t * encodings =
    reinterpret_cast<cetype_ext_t *>(R_alloc(static_cast<size_t>(n) + 1, sizeof(cetype_ext_t)));
  size_t payload = 0;
  for(R_xlen_t i = 0; i < n; ++i) {
    const charport_strview view = cpi::charsxp_to_view(src[i]);
    ptrs[i] = view.ptr;
    lengths[i] = view.len;
    encodings[i] = view.enc;
    if(!view.is_na()) {
      payload += static_cast<size_t>(view.len);
    }
  }
  if(n_slices <= 0 || n_slices == NA_INTEGER) {
    charport_charpack_fill_views_impl(x, n, ptrs, lengths, encodings);
    return x;
  }
  char * joined = R_alloc(payload + 1, 1);
  char * p = joined;
  for(R_xlen_t i = 0; i < n; ++i) {
    if(!cpi::view_is_na(ptrs[i], lengths[i], encodings[i]) && lengths[i] > 0) {
      std::memcpy(p, ptrs[i], static_cast<size_t>(lengths[i]));
      p += lengths[i];
    }
  }
  const char ** slices = reinterpret_cast<const char **>(R_alloc(static_cast<size_t>(n_slices), sizeof(const char *)));
  size_t * slice_bytes = reinterpret_cast<size_t *>(R_alloc(static_cast<size_t>(n_slices), sizeof(size_t)));
  size_t offset = 0;
  for(int k = 0; k < n_slices; ++k) {
    const size_t end = payload * static_cast<size_t>(k + 1) / static_cast<size_t>(n_slices);
    slices[k] = joined + offset;
    slice_bytes[k] = end - offset;
    offset = end;
  }
  charport_charpack_fill_slices_impl(x, n, lengths, encodings, n_slices, slices, slice_bytes);
  return x;
}

extern "C" SEXP C_charpack_fill_materialized_na(SEXP x, SEXP n) {
  return charport_charpack_fill_materialized_na_impl(x, static_cast<R_xlen_t>(Rf_asReal(n)));
}

extern "C" SEXP C_is_charpack(SEXP x) {
  return Rf_ScalarLogical(charpack_altrep::is(x) ? TRUE : FALSE);
}

extern "C" SEXP C_charpack_state(SEXP x) {
  if(!charpack_altrep::is(x)) {
    return Rf_ScalarString(NA_STRING);
  }
  return Rf_mkString(charpack_altrep::state_name(x));
}

// The filled RAWSXP itself (shared, read only), or NULL.
extern "C" SEXP C_charpack_data1(SEXP x) {
  if(!charpack_altrep::is(x)) {
    return R_NilValue;
  }
  return R_altrep_data1(x);
}

// A copy of a packed body with n and the sizes byte-swapped, so tests on a
// little-endian host can exercise the big-endian serialization path.
extern "C" SEXP C_charpack_bswap_packed(SEXP raw) {
  if(TYPEOF(raw) != RAWSXP || Rf_xlength(raw) < static_cast<R_xlen_t>(sizeof(uint64_t))) {
    Rf_error("charport charpack bswap_packed: raw must be a packed body");
  }
  uint64_t n;
  std::memcpy(&n, RAW(raw), sizeof(n));
  if(n > (static_cast<uint64_t>(Rf_xlength(raw)) - sizeof(uint64_t)) / sizeof(uint32_t)) {
    Rf_error("charport charpack bswap_packed: raw is truncated");
  }
  SEXP out = PROTECT(Rf_duplicate(raw));
  charvec_bswap_packed(RAW(out), static_cast<size_t>(n));
  UNPROTECT(1);
  return out;
}

extern "C" SEXP C_unregister_charpack(void) {
  charport_unregister_altrep(charpack_altrep::class_t);
  return R_NilValue;
}

extern "C" SEXP C_register_charpack(void) {
  charport_register_altrep(
    charpack_altrep::class_t,
    charport_reader_state_fns{charpack_altrep::reader_init, charpack_altrep::reader_release},
    charport_reader_range_fns{
      charpack_altrep::reader_strviews_range,
      charpack_altrep::reader_byteviews_range,
      charpack_altrep::reader_lengths_range,
      charpack_altrep::reader_encodings_range
    },
    charport_reader_index_fns{
      charpack_altrep::reader_strviews_index,
      charpack_altrep::reader_byteviews_index,
      charpack_altrep::reader_lengths_index,
      charpack_altrep::reader_encodings_index
    },
    charport_reader_capabilities{true, true}
  );
  return R_NilValue;
}
