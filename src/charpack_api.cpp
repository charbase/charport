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
// are u32 unless the payload needs u64.
// Nothing allocates after data1, so it is returned unprotected for the caller
// to protect (rchk does not follow a PROTECT across a return).
SEXP alloc_data1(R_xlen_t n, const char * const * ptrs, const int * lengths,
                 const cetype_ext_t * encodings, size_t payload,
                 const char * op) {
  const size_t count = static_cast<size_t>(n);
  const bool wide = charpack_layout::needs_wide(payload);
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
  SEXP data1 = PROTECT(alloc_data1(n, ptrs, lengths, encodings, payload, op));
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
  SEXP data1 = PROTECT(alloc_data1(n, nullptr, lengths, encodings, payload, op));
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
  fill_views(x, n, ptrs, lengths, encodings);
}

extern "C" void charport_charpack_fill_slices_impl(
    SEXP x, R_xlen_t n, const int * lengths, const cetype_ext_t * encodings,
    R_xlen_t n_slices, const char * const * slices, const size_t * slice_bytes) {
  fill_slices(x, n, lengths, encodings, n_slices, slices, slice_bytes);
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

namespace {

struct as_charpack_state {
  charport_reader reader;
  R_xlen_t n;
  const char ** ptrs;
  int * lengths;
  cetype_ext_t * encodings;
};

SEXP as_charpack_build(void * data) {
  const as_charpack_state * state = static_cast<as_charpack_state *>(data);
  SEXP out = PROTECT(charport_charpack_placeholder_impl());
  fill_views(out, state->n, state->ptrs, state->lengths, state->encodings);
  UNPROTECT(1);
  return out;
}

void as_charpack_release(void * data) {
  const as_charpack_state * state = static_cast<as_charpack_state *>(data);
  if(state->reader.release != nullptr) {
    state->reader.release(state->reader.state);
  }
}

} // namespace

// One read of the whole vector through the broker, then one copy into the
// charpack, which needs every length before it can allocate. Copying through
// an intermediate Store was measured twice as slow. The view arrays come from
// R_alloc and are allocated before the borrow starts, so no R error can leak
// it: from resolve() to R_ExecWithCleanup nothing calls R.
extern "C" SEXP C_as_charpack(SEXP x) {
  if(TYPEOF(x) != STRSXP) {
    Rf_error("charport as_charpack: x must be a character vector");
  }
  const R_xlen_t n = Rf_xlength(x);
  as_charpack_state state;
  state.n = n;
  state.ptrs = reinterpret_cast<const char **>(R_alloc(static_cast<size_t>(n) + 1, sizeof(const char *)));
  state.lengths = reinterpret_cast<int *>(R_alloc(static_cast<size_t>(n) + 1, sizeof(int)));
  state.encodings = reinterpret_cast<cetype_ext_t *>(R_alloc(static_cast<size_t>(n) + 1, sizeof(cetype_ext_t)));
  state.reader = charport::resolve(x);

  int status = CHARPORT_STATUS_OK;
  if(state.reader.n != n) {
    status = CHARPORT_STATUS_ERROR;
  } else if(n > 0) {
    status = state.reader.range.strviews(
      state.reader.state, 0, n, state.ptrs, state.lengths, state.encodings);
  }
  if(status != CHARPORT_STATUS_OK) {
    as_charpack_release(&state);
    return charport_sexp_guard("as_charpack", [&]() -> SEXP {
      charport::detail::check_access(status);
      return R_NilValue;
    });
  }
  return R_ExecWithCleanup(
    &as_charpack_build, &state, &as_charpack_release, &state);
}

extern "C" SEXP C_is_charpack(SEXP x) {
  return Rf_ScalarLogical(charpack_altrep::is(x) ? TRUE : FALSE);
}

extern "C" void charport_register_charpack_impl(void) {
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
}
