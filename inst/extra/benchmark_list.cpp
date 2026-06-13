// Kernels for the list-of-small-vectors benchmark, compiled at bench time
// together with benchmark.cpp (whose corpus-ingest and whole-vector kernels
// are reused). The scenario is a large list where every element is a small
// character vector, conceptually as.list(corpus) at chunk = 1, so
// per-vector costs (allocation, ALTREP wrapping, per-resolve work) dominate
// over per-element costs. Read kernels return the same chained FNV-1a hash
// as the whole-vector kernels: iterating the list elements in order visits
// the corpus lines in order, so hashes are comparable across all paths.

#include "charport.h"

#include <cstdint>
#include <vector>

static inline uint64_t fnv1a_(const char * p, uint32_t n, uint64_t h) {
  for(uint32_t i = 0; i < n; ++i) {
    h ^= static_cast<unsigned char>(p[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

static SEXP hash_to_sexp_(uint64_t h, R_xlen_t n_na) {
  SEXP out = Rf_allocVector(REALSXP, 2);
  REAL(out)[0] = static_cast<double>(h % 9007199254740992ULL);  // 2^53
  REAL(out)[1] = static_cast<double>(n_na);
  return out;
}

// Split the source vector into ceil(n / chunk) charvecs of length <= chunk.
// This is the shape a split-like verb produces: many tiny independent charvecs,
// each paying full per-vector cost (store allocation, ALTREP wrap, external
// pointer, finalizer). One cp::charvec::Builder is reused across all outputs
// via reset(), matching an as.list workload that reuses the build context
// rather than reconstructing a Builder per vector.
extern "C" SEXP C_benchl_build_charvec_list(SEXP x, SEXP chunk_) {
  cp::Reader r(x);
  const R_xlen_t n = r.size();
  const R_xlen_t chunk = Rf_asInteger(chunk_);
  if(chunk < 1) Rf_error("chunk must be >= 1");
  const R_xlen_t n_out = (n + chunk - 1) / chunk;
  try {
    SEXP out = PROTECT(Rf_allocVector(VECSXP, n_out));
    cp::charvec::Builder b(0);
    for(R_xlen_t j = 0; j < n_out; ++j) {
      const R_xlen_t lo = j * chunk;
      const R_xlen_t hi = lo + chunk < n ? lo + chunk : n;
      b.reset(hi - lo);
      for(R_xlen_t i = lo; i < hi; ++i) {
        cp::StrView v = r[i];
        if(v.is_na()) b.set_na(i - lo); else b.set(i - lo, v);
      }
      SET_VECTOR_ELT(out, j, b.finish());
    }
    UNPROTECT(1);
    return out;
  } catch(const std::exception & e) {
    Rf_error("build_charvec_list: %s", e.what());
  }
}

// Baseline: the same list shape with plain STRSXP elements built through
// mkCharLenCE. Comparable timings require a cold string cache (fresh R session
// per repetition, handled by benchmark_list.R). On duplicate-heavy corpora,
// the cache reduces memory use by interning repeated strings once, while a
// charvec stores every occurrence.
extern "C" SEXP C_benchl_build_strsxp_list(SEXP x, SEXP chunk_) {
  cp::Reader r(x);
  const R_xlen_t n = r.size();
  const R_xlen_t chunk = Rf_asInteger(chunk_);
  if(chunk < 1) Rf_error("chunk must be >= 1");
  const R_xlen_t n_out = (n + chunk - 1) / chunk;
  SEXP out = PROTECT(Rf_allocVector(VECSXP, n_out));
  for(R_xlen_t j = 0; j < n_out; ++j) {
    const R_xlen_t lo = j * chunk;
    const R_xlen_t hi = lo + chunk < n ? lo + chunk : n;
    SEXP el = Rf_allocVector(STRSXP, hi - lo);
    SET_VECTOR_ELT(out, j, el);  // protects el via out
    for(R_xlen_t i = lo; i < hi; ++i) {
      cp::StrView v = r[i];
      if(v.is_na()) { SET_STRING_ELT(el, i - lo, NA_STRING); continue; }
      const cetype_t ce = v.enc == charport_enc::CE_BYTES ? CE_BYTES : CE_UTF8;
      SET_STRING_ELT(el, i - lo, Rf_mkCharLenCE(v.ptr, static_cast<int>(v.len), ce));
    }
  }
  UNPROTECT(1);
  return out;
}

// Read every string of every list element through cp::Reader. One resolve per
// vector is the per-vector read cost this benchmark isolates.
extern "C" SEXP C_benchl_hash_reader(SEXP lst) {
  const R_xlen_t n_out = Rf_xlength(lst);
  uint64_t h = 14695981039346656037ULL;
  R_xlen_t n_na = 0;
  for(R_xlen_t j = 0; j < n_out; ++j) {
    cp::Reader r(VECTOR_ELT(lst, j));
    for(cp::StrView v : r) {
      if(v.is_na()) { ++n_na; continue; }
      h = fnv1a_(v.ptr, v.len, h);
    }
  }
  return hash_to_sexp_(h, n_na);
}

// Conventional baseline: STRING_ELT loop per list element.
extern "C" SEXP C_benchl_hash_string_elt(SEXP lst) {
  const R_xlen_t n_out = Rf_xlength(lst);
  uint64_t h = 14695981039346656037ULL;
  R_xlen_t n_na = 0;
  for(R_xlen_t j = 0; j < n_out; ++j) {
    SEXP el = VECTOR_ELT(lst, j);
    const R_xlen_t k = Rf_xlength(el);
    for(R_xlen_t i = 0; i < k; ++i) {
      SEXP cs = STRING_ELT(el, i);
      if(cs == NA_STRING) { ++n_na; continue; }
      h = fnv1a_(R_CHAR(cs), static_cast<uint32_t>(Rf_xlength(cs)), h);
    }
  }
  return hash_to_sexp_(h, n_na);
}

// Length sums: almost no per-element work, so with small chunks the timing
// is nearly pure per-vector overhead (resolve / dispatch per list element).
extern "C" SEXP C_benchl_sumlen_reader(SEXP lst) {
  const R_xlen_t n_out = Rf_xlength(lst);
  double total = 0;
  for(R_xlen_t j = 0; j < n_out; ++j) {
    cp::Reader r(VECTOR_ELT(lst, j));
    for(cp::StrView v : r) {
      if(!v.is_na()) total += v.len;
    }
  }
  return Rf_ScalarReal(total);
}

extern "C" SEXP C_benchl_sumlen_elt(SEXP lst) {
  const R_xlen_t n_out = Rf_xlength(lst);
  double total = 0;
  for(R_xlen_t j = 0; j < n_out; ++j) {
    SEXP el = VECTOR_ELT(lst, j);
    const R_xlen_t k = Rf_xlength(el);
    for(R_xlen_t i = 0; i < k; ++i) {
      SEXP cs = STRING_ELT(el, i);
      if(cs != NA_STRING) total += static_cast<double>(Rf_xlength(cs));
    }
  }
  return Rf_ScalarReal(total);
}
