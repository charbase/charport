#ifndef CHARPORT_INTERNAL_R_INTEROP_H
#define CHARPORT_INTERNAL_R_INTEROP_H

// The R-facing edge of the charvec store: CHARSXP -> record classification
// and record -> CHARSXP materialization. Everything here runs on the main R
// thread only (R API calls throughout).
//
// API-status note: uses Rf_getCharCE, Rf_translateCharUTF8, Rf_mkCharLenCE,
// Rf_charIsASCII (>= 4.5.0), and Rf_xlength on CHARSXPs. All believed to be
// on the supported-API list after the R 4.4-4.6 non-API cleanup; re-audit
// against the current classification before the first CRAN release.

#define R_NO_REMAP
#include <Rinternals.h>
#include <Rversion.h>

#include "base.h"
#include "charvec_store.h"

namespace charport {
namespace internal {

inline bool charsxp_is_ascii(SEXP x) {
#if (R_VERSION >= R_Version(4, 5, 0))
  return Rf_charIsASCII(x) == TRUE;
#else
  return check_ascii(CHAR(x), static_cast<size_t>(Rf_xlength(x)));
#endif
}

inline cetype_t to_base_encoding(charport_enc enc) noexcept {
  switch(enc) {
  case charport_enc::CE_ASCII:
    return CE_NATIVE;
  case charport_enc::CE_UTF8:
  case charport_enc::CE_ASCII_OR_UTF8:  // interim normative reading: treat as UTF-8
    return CE_UTF8;
  case charport_enc::CE_LATIN1:
    return CE_LATIN1;
  case charport_enc::CE_BYTES:
    return CE_BYTES;
  default:
    return CE_NATIVE;
  }
}

inline SEXP make_charsxp(const charport_strview & v) {
  if(v.is_na()) {
    return NA_STRING;
  }
  if(!check_r_string_len(v.len)) {
    throw std::runtime_error("string size exceeds R string size");
  }
  return Rf_mkCharLenCE(v.ptr, static_cast<int>(v.len), to_base_encoding(v.enc));
}

// Classify an incoming CHARSXP (not NA_STRING) against the storage policy:
// returns CE_ASCII / CE_UTF8 / CE_BYTES for borrowable bytes, or
// CE_LATIN1 / CE_NATIVE meaning the caller must translate to UTF-8 first.
inline charport_enc classify_charsxp(SEXP x) {
  switch(Rf_getCharCE(x)) {
  // pure-ASCII strings are never encoding-marked in R, so a marked CHARSXP
  // cannot be ASCII -- the ASCII probe only runs for unmarked (native) ones
  case CE_UTF8:
    return charport_enc::CE_UTF8;
  case CE_LATIN1:
    return charport_enc::CE_LATIN1;
  case CE_BYTES:
    return charport_enc::CE_BYTES;
  default:
    return charsxp_is_ascii(x) ? charport_enc::CE_ASCII
                               : charport_enc::CE_NATIVE;
  }
}

// Store a CHARSXP into Store (charvec_data or charvec_shard) at idx,
// normalizing to the backend emission policy: records are only ever
// CE_ASCII, CE_UTF8, CE_BYTES, or NA. latin1/native inputs are translated
// via Rf_translateCharUTF8 (R_alloc scratch, released here via
// vmaxget/vmaxset; can raise an R error on invalid bytes, so call only where
// a longjmp is safe).
template <typename Store>
inline void assign_charsxp(Store & store, size_t idx, SEXP x) {
  if(x == NA_STRING) {
    store.assign(idx, nullptr, 0, charport_enc::CE_NA);
    return;
  }
  const charport_enc enc = classify_charsxp(x);
  if(enc == charport_enc::CE_LATIN1 || enc == charport_enc::CE_NATIVE) {
    const void * vmax = vmaxget();
    const char * translated = Rf_translateCharUTF8(x);
    const size_t len = std::strlen(translated);
    // native input can translate to pure ASCII even when the source bytes weren't
    const charport_enc out_enc = check_ascii(translated, len)
      ? charport_enc::CE_ASCII : charport_enc::CE_UTF8;
    store.assign(idx, translated, len, out_enc);
    vmaxset(vmax);
    return;
  }
  store.assign(idx, CHAR(x), static_cast<size_t>(Rf_xlength(x)), enc);
}

} // namespace internal
} // namespace charport

#endif
