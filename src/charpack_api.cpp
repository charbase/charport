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

template<typename Offset>
void write_records(size_t count, const char * const * ptrs, const int * lengths,
                   const cetype_ext_t * encodings, unsigned char * ends,
                   unsigned char * encs) noexcept {
  Offset end = 0;
  for(size_t i = 0; i < count; ++i) {
    const bool na = ptrs == nullptr ? record_is_na(lengths[i], encodings[i])
                                    : cpi::view_is_na(ptrs[i], lengths[i], encodings[i]);
    end += na ? Offset(0) : static_cast<Offset>(lengths[i]);
    std::memcpy(ends + sizeof(Offset) * i, &end, sizeof(end));
    encs[i] = na ? CETYPE_EXT_NA.value : encodings[i].value;
  }
}

// Allocates data1 and writes its head, end offsets and encodings. The offsets
// are u32 unless the payload needs u64, or force_wide asks for it (tests).
// Nothing allocates after data1, so it is returned unprotected for the caller
// to protect (rchk does not follow a PROTECT across a return).
SEXP alloc_data1(R_xlen_t n, const char * const * ptrs, const int * lengths,
                 const cetype_ext_t * encodings, size_t payload, bool force_wide,
                 const char * op) {
  const size_t count = static_cast<size_t>(n);
  const bool wide = force_wide || charpack_layout::needs_wide(payload);
  const size_t total = charpack_layout::total_bytes(count, payload, wide);
  if(total == 0) {
    Rf_error("charport charpack %s: vector exceeds the maximum raw vector size", op);
  }
  SEXP data1 = Rf_allocVector(RAWSXP, static_cast<R_xlen_t>(total));
  unsigned char * p = RAW(data1);
  charpack_layout::write_head(p, count, wide);
  unsigned char * ends = p + charpack_layout::header_bytes;
  unsigned char * encs = ends + charpack_layout::offset_bytes(wide) * count;
  if(wide) {
    write_records<uint64_t>(count, ptrs, lengths, encodings, ends, encs);
  } else {
    write_records<uint32_t>(count, ptrs, lengths, encodings, ends, encs);
  }
  return data1;
}

char * payload_start(SEXP data1) {
  return const_cast<char *>(charpack_layout::parse(data1).bytes);
}

void fill_views(SEXP x, R_xlen_t n, const char * const * ptrs, const int * lengths,
                const cetype_ext_t * encodings, bool force_wide) {
  const char * op = "fill_views";
  check_unfilled(x, op);
  if(n < 0) {
    Rf_error("charport charpack %s: negative length", op);
  }
  if(n > 0 && (ptrs == nullptr || lengths == nullptr || encodings == nullptr)) {
    Rf_error("charport charpack %s: input arrays must not be NULL", op);
  }
  const size_t payload = checked_payload(n, ptrs, lengths, encodings, op);
  SEXP data1 = PROTECT(alloc_data1(n, ptrs, lengths, encodings, payload, force_wide, op));
  char * dest = payload_start(data1);
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

void fill_slices(SEXP x, R_xlen_t n, const int * lengths, const cetype_ext_t * encodings,
                 R_xlen_t n_slices, const char * const * slices, const size_t * slice_bytes,
                 bool force_wide) {
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
  SEXP data1 = PROTECT(alloc_data1(n, nullptr, lengths, encodings, payload, force_wide, op));
  char * dest = payload_start(data1);
  for(R_xlen_t k = 0; k < n_slices; ++k) {
    if(slice_bytes[k] > 0) {
      std::memcpy(dest, slices[k], slice_bytes[k]);
      dest += slice_bytes[k];
    }
  }
  R_set_altrep_data1(x, data1);
  UNPROTECT(1);
}

} // namespace

extern "C" SEXP charport_charpack_placeholder_impl(void) {
  return R_new_altrep(charpack_altrep::class_t, R_NilValue, R_NilValue);
}

extern "C" void charport_charpack_fill_views_impl(
    SEXP x, R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings) {
  fill_views(x, n, ptrs, lengths, encodings, false);
}

extern "C" void charport_charpack_fill_slices_impl(
    SEXP x, R_xlen_t n, const int * lengths, const cetype_ext_t * encodings,
    R_xlen_t n_slices, const char * const * slices, const size_t * slice_bytes) {
  fill_slices(x, n, lengths, encodings, n_slices, slices, slice_bytes, false);
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
// offsets (n_slices <= 0 selects fill_views). wide forces u64 end offsets.
extern "C" SEXP C_charpack_fill_from(SEXP x, SEXP strings, SEXP n_slices_, SEXP wide_) {
  if(TYPEOF(strings) != STRSXP) {
    Rf_error("charport charpack fill_from: strings must be a character vector");
  }
  const R_xlen_t n = Rf_xlength(strings);
  const int n_slices = Rf_asInteger(n_slices_);
  const bool wide = Rf_asLogical(wide_) == TRUE;
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
    fill_views(x, n, ptrs, lengths, encodings, wide);
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
  fill_slices(x, n, lengths, encodings, n_slices, slices, slice_bytes, wide);
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

// The serialized state of a filled charpack: data1 itself on a little-endian
// host.
extern "C" SEXP C_charpack_serialized_state(SEXP x) {
  if(!charpack_altrep::is(x) || R_altrep_data1(x) == R_NilValue) {
    Rf_error("charport charpack serialized_state: x must be a filled charpack");
  }
  return charpack_altrep::Serialized_state(x);
}

// A charpack unserialized from a copy of a state, leaving the caller's intact.
extern "C" SEXP C_charpack_from_state(SEXP state) {
  if(TYPEOF(state) != RAWSXP) {
    Rf_error("charport charpack from_state: state must be a raw vector");
  }
  SEXP copy = PROTECT(Rf_duplicate(state));
  SEXP x = charpack_altrep::Unserialize(R_NilValue, copy);
  UNPROTECT(1);
  return x;
}

extern "C" SEXP C_unregister_charpack(void) {
  charport_unregister_altrep(charpack_altrep::class_t);
  return R_NilValue;
}

extern "C" SEXP C_register_charpack(void) {
  charport_register_altrep(
    charpack_altrep::class_t,
    charport_reader_state_fns{charpack_altrep::reader_init, nullptr},
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
