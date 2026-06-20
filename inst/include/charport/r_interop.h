#ifndef CHARPORT_R_INTEROP_H
#define CHARPORT_R_INTEROP_H

// The R-facing edge of the charvec store: CHARSXP -> record classification
// and record -> CHARSXP materialization. Everything here runs on the main R
// thread only (R API calls throughout).
//
// Encoding policy: the store keeps whatever encoding the input carried -- it
// does not translate or reject. An incoming CHARSXP is classified to UTF-8,
// LATIN1, NATIVE, BYTES, ASCII (an unmarked all-< 0x80 string), or NA, stored
// verbatim, and materialized back to the matching base cetype_t so the
// encoding mark round-trips unchanged.
//
// API-status note: uses Rf_getCharCE, Rf_mkCharLenCE, Rf_charIsASCII
// (>= 4.5.0), and Rf_xlength on CHARSXPs. All believed to be on the
// supported-API list after the R 4.4-4.6 non-API cleanup; re-audit against
// the current classification before the first CRAN release.

#define R_NO_REMAP
#include <Rinternals.h>
#include <Rversion.h>

#include "calc.h"
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

// Classify an incoming CHARSXP (not NA_STRING) to the encoding the store will
// keep, preserving the mark for fidelity: a marked CHARSXP keeps its mark
// (UTF-8 / LATIN1 / BYTES); an unmarked (native) string is probed and becomes
// CE_ASCII if all bytes are < 0x80, else CE_NATIVE. R never encoding-marks a
// pure-ASCII string, so the probe only needs to run on the unmarked branch --
// and keeping the mark elsewhere means a UTF-8/latin1 string round-trips with
// its original mark, not collapsed to ASCII. Nothing is translated or rejected.
inline charport_enc classify_charsxp(SEXP x) {
  switch(Rf_getCharCE(x)) {
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

// Classify a CHARSXP into a borrowed strview the store keeps verbatim:
// NA_STRING -> {NULL, 0, CE_NA}; otherwise the CHARSXP's bytes (borrowed, valid
// while x is) with the classified encoding -- no translation, no allocation, no
// R error path. The caller writes it into a builder (Builder::set) or the
// persistent store (charvec_data::assign).
inline charport_strview charsxp_to_view(SEXP x) {
  if(x == NA_STRING) {
    return make_strview(nullptr, 0, charport_enc::CE_NA);
  }
  return make_strview(CHAR(x), static_cast<uint32_t>(Rf_xlength(x)), classify_charsxp(x));
}

} // namespace internal
} // namespace charport

#endif
