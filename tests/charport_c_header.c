#include "charport.h"

static int c_reader_lengths(
    void * state, R_xlen_t start, R_xlen_t size, int * out) {
  (void)state;
  (void)start;
  (void)size;
  (void)out;
  return CHARPORT_STATUS_OK;
}

static charport_reader_lengths_range_fn c_reader_lengths_type =
  c_reader_lengths;
static charport_resolve_t c_resolve_type = charport_resolve;
static charport_sexp_info_t c_sexp_info_type = charport_get_sexp_info;
static charport_abi_version_t c_abi_version_type = charport_abi_version;
static charport_charvec_from_views_t c_from_views_type =
  charport_charvec_from_views;

static cetype_ext_t c_charsxp_encoding(SEXP x) {
  if(x == NA_STRING) {
    return charport_cetype_ext(CETYPE_EXT_NA);
  }
  switch(Rf_getCharCE(x)) {
  case CE_UTF8: return charport_cetype_ext(CETYPE_EXT_UTF8);
  case CE_LATIN1: return charport_cetype_ext(CETYPE_EXT_LATIN1);
  case CE_BYTES: return charport_cetype_ext(CETYPE_EXT_BYTES);
  default: return charport_cetype_ext(CETYPE_EXT_NATIVE);
  }
}

SEXP C_charport_c_header_probe(void) {
  charport_sexp_info info = charport_get_sexp_info(R_NilValue);
  return Rf_ScalarLogical(
    c_reader_lengths_type != NULL &&
    c_resolve_type != NULL &&
    c_sexp_info_type != NULL &&
    c_abi_version_type != NULL &&
    c_from_views_type != NULL &&
    !info.is_strsxp &&
    info.length == (R_xlen_t)-1 &&
    CHARPORT_ABI_VERSION == 1
  );
}

SEXP C_charport_c_abi_ok(void) {
  return Rf_ScalarLogical(
    charport_abi_version() == CHARPORT_ABI_VERSION ? TRUE : FALSE
  );
}

SEXP C_charport_c_reader_roundtrip(SEXP x) {
  R_xlen_t i;
  R_xlen_t n;
  R_xlen_t expected_n = XLENGTH(x);
  const char ** ptrs;
  int * lengths;
  cetype_ext_t * encodings;
  int status;
  SEXP out;
  charport_reader reader = charport_resolve(x);

  n = reader.n;
  if(n != expected_n ||
     reader.range.strviews == NULL ||
     reader.range.byteviews == NULL ||
     reader.range.lengths == NULL ||
     reader.range.encodings == NULL ||
     reader.index.strviews == NULL ||
     reader.index.byteviews == NULL ||
     reader.index.lengths == NULL ||
     reader.index.encodings == NULL) {
    charport_reader_release(&reader);
    Rf_error("invalid charport reader");
  }

  ptrs = n == 0 ? NULL : (const char **) R_alloc((size_t)n, sizeof(*ptrs));
  lengths = n == 0 ? NULL : (int *) R_alloc((size_t)n, sizeof(*lengths));
  encodings = n == 0 ? NULL :
    (cetype_ext_t *) R_alloc((size_t)n, sizeof(*encodings));
  status = reader.range.strviews(
    reader.state, 0, n, ptrs, lengths, encodings
  );
  if(status != CHARPORT_STATUS_OK) {
    charport_reader_release(&reader);
    Rf_error("charport reader returned status %d", status);
  }

  for(i = 0; i < n; ++i) {
    if(encodings[i].value == CETYPE_EXT_NA) {
      if(ptrs[i] != NULL || lengths[i] != NA_INTEGER) {
        charport_reader_release(&reader);
        Rf_error("invalid missing charport view");
      }
    } else if(ptrs[i] == NULL || lengths[i] < 0) {
      charport_reader_release(&reader);
      Rf_error("invalid non-missing charport view");
    }
  }

  out = PROTECT(charport_charvec_from_views(n, ptrs, lengths, encodings));
  charport_reader_release(&reader);
  charport_reader_release(&reader);
  if(reader.state != NULL || reader.release != NULL) {
    UNPROTECT(1);
    Rf_error("charport reader release did not clear ownership");
  }
  UNPROTECT(1);
  return out;
}

SEXP C_charport_c_from_views(SEXP x) {
  R_xlen_t i;
  R_xlen_t n;
  const char ** ptrs;
  int * lengths;
  cetype_ext_t * encodings;

  if(TYPEOF(x) != STRSXP) {
    Rf_error("x must be a character vector");
  }
  n = XLENGTH(x);
  if(n == 0) {
    return charport_charvec_from_views(0, NULL, NULL, NULL);
  }

  ptrs = (const char **) R_alloc((size_t)n, sizeof(*ptrs));
  lengths = (int *) R_alloc((size_t)n, sizeof(*lengths));
  encodings = (cetype_ext_t *) R_alloc((size_t)n, sizeof(*encodings));
  for(i = 0; i < n; ++i) {
    SEXP value = STRING_ELT(x, i);
    if(value == NA_STRING) {
      ptrs[i] = NULL;
      lengths[i] = NA_INTEGER;
      encodings[i] = charport_cetype_ext(CETYPE_EXT_NA);
    } else {
      ptrs[i] = CHAR(value);
      lengths[i] = LENGTH(value);
      encodings[i] = c_charsxp_encoding(value);
    }
  }
  return charport_charvec_from_views(n, ptrs, lengths, encodings);
}

SEXP C_charport_c_from_views_bad_length(void) {
  const char * ptrs[] = {"x"};
  const int lengths[] = {-1};
  const cetype_ext_t encodings[] = {CETYPE_EXT_ASCII};
  return charport_charvec_from_views(1, ptrs, lengths, encodings);
}

SEXP C_charport_c_from_views_bad_encoding(void) {
  const char * ptrs[] = {"x"};
  const int lengths[] = {1};
  const cetype_ext_t encodings[] = {{42}};
  return charport_charvec_from_views(1, ptrs, lengths, encodings);
}

SEXP C_charport_c_from_views_null_arrays(void) {
  return charport_charvec_from_views(1, NULL, NULL, NULL);
}
