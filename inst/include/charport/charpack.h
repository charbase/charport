#ifndef CHARPORT_CHARPACK_H
#define CHARPORT_CHARPACK_H

// charpack: a compact ALTREP character vector whose strings live in one R raw
// vector. Include charport.h from packages.
//
// A charvec keeps its strings in native memory owned through an external
// pointer and a finalizer, which suits long vectors. For many short vectors
// that per-vector cost dominates. A charpack copies its strings into a single
// RAWSXP that R owns: no external pointer, no finalizer, and R accounts for
// the memory. The first element access materializes the whole vector.
//
// Construction always copies. Fill an unfilled placeholder once, or use a
// from_ constructor, which allocates a placeholder and fills it:
//
//   placeholder()  an unfilled charpack, length 0 until it is filled
//   fill_views     strings from pointer, length and encoding arrays
//   fill_slices    strings from byte slices, split in order by the lengths
//   fill_materialized_na
//                  skip packing: make x an ordinary character vector of n NA
//                  strings, already materialized, and return that vector to
//                  fill with SET_STRING_ELT; x keeps it alive
//
// Lengths use NA_INTEGER for NA, as in charport_charvec_from_views. A fill
// raises an R error if x is not an unfilled placeholder, so a filled or
// materialized charpack is never refilled. Reading or duplicating a
// placeholder never yields another placeholder, so a read before the fill
// makes the fill fail. Fills call R and must run on the main thread.

#include "interop/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef SEXP (*charport_charpack_placeholder_t)(void);
typedef void (*charport_charpack_fill_views_t)(
    SEXP x, R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings);
typedef void (*charport_charpack_fill_slices_t)(
    SEXP x, R_xlen_t n, const int * lengths, const cetype_ext_t * encodings,
    R_xlen_t n_slices, const char * const * slices, const size_t * slice_bytes);
typedef SEXP (*charport_charpack_fill_materialized_na_t)(SEXP x, R_xlen_t n);
typedef int (*charport_is_charpack_t)(SEXP x);

#ifdef __cplusplus
#define CHARPORT_CHARPACK_CALLABLE(type, name) \
  reinterpret_cast<type>(R_GetCCallable("charport", name))
#else
#define CHARPORT_CHARPACK_CALLABLE(type, name) \
  ((type) R_GetCCallable("charport", name))
#endif

static inline SEXP charport_charpack_placeholder(void) {
  static charport_charpack_placeholder_t fn = NULL;
  if(fn == NULL) {
    fn = CHARPORT_CHARPACK_CALLABLE(charport_charpack_placeholder_t,
                                    "charport_charpack_placeholder_v1");
  }
  return fn();
}

static inline void charport_charpack_fill_views(
    SEXP x, R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings) {
  static charport_charpack_fill_views_t fn = NULL;
  if(fn == NULL) {
    fn = CHARPORT_CHARPACK_CALLABLE(charport_charpack_fill_views_t,
                                    "charport_charpack_fill_views_v1");
  }
  fn(x, n, ptrs, lengths, encodings);
}

static inline void charport_charpack_fill_slices(
    SEXP x, R_xlen_t n, const int * lengths, const cetype_ext_t * encodings,
    R_xlen_t n_slices, const char * const * slices, const size_t * slice_bytes) {
  static charport_charpack_fill_slices_t fn = NULL;
  if(fn == NULL) {
    fn = CHARPORT_CHARPACK_CALLABLE(charport_charpack_fill_slices_t,
                                    "charport_charpack_fill_slices_v1");
  }
  fn(x, n, lengths, encodings, n_slices, slices, slice_bytes);
}

static inline SEXP charport_charpack_fill_materialized_na(SEXP x, R_xlen_t n) {
  static charport_charpack_fill_materialized_na_t fn = NULL;
  if(fn == NULL) {
    fn = CHARPORT_CHARPACK_CALLABLE(charport_charpack_fill_materialized_na_t,
                                    "charport_charpack_fill_materialized_na_v1");
  }
  return fn(x, n);
}

static inline int charport_is_charpack(SEXP x) {
  static charport_is_charpack_t fn = NULL;
  if(fn == NULL) {
    fn = CHARPORT_CHARPACK_CALLABLE(charport_is_charpack_t, "charport_is_charpack_v1");
  }
  return fn(x);
}

static inline SEXP charport_charpack_from_views(
    R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings) {
  SEXP x = PROTECT(charport_charpack_placeholder());
  charport_charpack_fill_views(x, n, ptrs, lengths, encodings);
  UNPROTECT(1);
  return x;
}

static inline SEXP charport_charpack_from_slices(
    R_xlen_t n, const int * lengths, const cetype_ext_t * encodings,
    R_xlen_t n_slices, const char * const * slices, const size_t * slice_bytes) {
  SEXP x = PROTECT(charport_charpack_placeholder());
  charport_charpack_fill_slices(x, n, lengths, encodings, n_slices, slices, slice_bytes);
  UNPROTECT(1);
  return x;
}

#undef CHARPORT_CHARPACK_CALLABLE

#ifdef __cplusplus
} // extern "C"

namespace charport {
namespace charpack {

inline SEXP placeholder() {
  return charport_charpack_placeholder();
}

inline void fill_views(SEXP x, R_xlen_t n, const char * const * ptrs,
                       const int * lengths, const cetype_ext_t * encodings) {
  charport_charpack_fill_views(x, n, ptrs, lengths, encodings);
}

inline void fill_slices(SEXP x, R_xlen_t n, const int * lengths,
                        const cetype_ext_t * encodings, R_xlen_t n_slices,
                        const char * const * slices, const size_t * slice_bytes) {
  charport_charpack_fill_slices(x, n, lengths, encodings, n_slices, slices, slice_bytes);
}

inline SEXP fill_materialized_na(SEXP x, R_xlen_t n) {
  return charport_charpack_fill_materialized_na(x, n);
}

inline bool is_charpack(SEXP x) {
  return charport_is_charpack(x) != 0;
}

inline SEXP from_views(R_xlen_t n, const char * const * ptrs, const int * lengths,
                       const cetype_ext_t * encodings) {
  return charport_charpack_from_views(n, ptrs, lengths, encodings);
}

inline SEXP from_slices(R_xlen_t n, const int * lengths, const cetype_ext_t * encodings,
                        R_xlen_t n_slices, const char * const * slices,
                        const size_t * slice_bytes) {
  return charport_charpack_from_slices(n, lengths, encodings, n_slices, slices, slice_bytes);
}

} // namespace charpack
} // namespace charport
#endif

#endif
