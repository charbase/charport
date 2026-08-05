// Kernels for a list-of-small-character-vectors benchmark. At chunk = 1,
// this isolates the per-vector cost paid by split-like results. The matching
// R driver compiles this file with benchmark.cpp and checks every result.

#include "charport.h"
#include "consumer-boundary.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace {

template<typename Fn>
SEXP guarded(const char * operation, Fn fn) {
  return charport_consumer::boundary(operation, fn);
}

R_xlen_t checked_chunk(int value) {
  if(value == NA_INTEGER || value < 1) {
    throw std::runtime_error("chunk must be at least 1");
  }
  return static_cast<R_xlen_t>(value);
}

R_xlen_t output_size(R_xlen_t n, R_xlen_t chunk) {
  return n / chunk + (n % chunk != 0);
}

uint64_t fnv1a(const char * ptr, uint32_t len, uint64_t hash) {
  for(uint32_t i = 0; i < len; ++i) {
    hash ^= static_cast<unsigned char>(ptr[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

SEXP hash_result(uint64_t hash, R_xlen_t n_na) {
  SEXP out = Rf_allocVector(REALSXP, 2);
  REAL(out)[0] = static_cast<double>(hash % 9007199254740992ULL);
  REAL(out)[1] = static_cast<double>(n_na);
  return out;
}

SEXP protected_hash_result(uint64_t hash, R_xlen_t n_na) {
  return charport_consumer::unwind_protect(
    [hash, n_na]() -> SEXP { return hash_result(hash, n_na); }
  );
}

cetype_t r_encoding(cetype_ext_t enc) {
  if(enc == cetype_ext_t::CE_BYTES) {
    return CE_BYTES;
  }
  if(enc == cetype_ext_t::CE_LATIN1) {
    return CE_LATIN1;
  }
  return CE_UTF8;
}

} // namespace

// Reuse one fixed-size Builder across output vectors. This preserves the
// historical benchmark arm while exercising the current to_sexp() API.
extern "C" SEXP C_benchl_build_builder_list(SEXP x, SEXP chunk_) {
  const int chunk_value = Rf_asInteger(chunk_);
  return guarded("build_builder_list", [&, chunk_value]() -> SEXP {
    const R_xlen_t chunk = checked_chunk(chunk_value);
    charport::Reader reader(x);
    const R_xlen_t n = reader.size();
    const R_xlen_t n_out = output_size(n, chunk);
    SEXP out = charport_consumer::unwind_protect(
      [n_out]() -> SEXP { return Rf_allocVector(VECSXP, n_out); }
    );
    PROTECT(out);

    charport::charvec::Builder builder(0);
    for(R_xlen_t j = 0; j < n_out; ++j) {
      const R_xlen_t lo = j * chunk;
      const R_xlen_t hi = std::min<R_xlen_t>(lo + chunk, n);
      builder.reset(hi - lo);
      for(R_xlen_t i = lo; i < hi; ++i) {
        builder.set(i - lo, reader.view(i));
      }
      SET_VECTOR_ELT(out, j, builder.to_sexp());
    }

    UNPROTECT(1);
    return out;
  });
}

// Store::scalar is the current fast path for a known one-element result.
extern "C" SEXP C_benchl_build_scalar_list(SEXP x) {
  return guarded("build_scalar_list", [&]() -> SEXP {
    charport::Reader reader(x);
    const R_xlen_t n = reader.size();
    SEXP out = charport_consumer::unwind_protect(
      [n]() -> SEXP { return Rf_allocVector(VECSXP, n); }
    );
    PROTECT(out);

    for(R_xlen_t i = 0; i < n; ++i) {
      const charport::StrView value = reader.view(i);
      charport::charvec::Store store = value.is_na()
        ? charport::charvec::Store::scalar(
            nullptr, 0, cetype_ext_t::CE_NA)
        : charport::charvec::Store::scalar(
            value.ptr, static_cast<size_t>(value.len), value.enc);
      SET_VECTOR_ELT(out, i, charport::charvec::wrap(std::move(store)));
    }

    UNPROTECT(1);
    return out;
  });
}

// Plain STRSXPs need a cold R string cache for a meaningful construction
// comparison. The R driver therefore runs this kernel in a fresh process.
extern "C" SEXP C_benchl_build_strsxp_list(SEXP x, SEXP chunk_) {
  const int chunk_value = Rf_asInteger(chunk_);
  return guarded("build_strsxp_list", [&, chunk_value]() -> SEXP {
    const R_xlen_t chunk = checked_chunk(chunk_value);
    charport::Reader reader(x);
    const R_xlen_t n = reader.size();
    const R_xlen_t n_out = output_size(n, chunk);

    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(VECSXP, n_out));
      for(R_xlen_t j = 0; j < n_out; ++j) {
        const R_xlen_t lo = j * chunk;
        const R_xlen_t hi = std::min<R_xlen_t>(lo + chunk, n);
        SEXP element = Rf_allocVector(STRSXP, hi - lo);
        SET_VECTOR_ELT(out, j, element);
        for(R_xlen_t i = lo; i < hi; ++i) {
          const charport::StrView value = reader.view(i);
          if(value.is_na()) {
            SET_STRING_ELT(element, i - lo, NA_STRING);
          } else {
            SET_STRING_ELT(
              element, i - lo,
              Rf_mkCharLenCE(value.ptr, value.len, r_encoding(value.enc))
            );
          }
        }
      }
      UNPROTECT(1);
      return out;
    });
  });
}

// One Reader resolve per list element is the intended cost under test.
extern "C" SEXP C_benchl_hash_reader(SEXP list) {
  return guarded("hash_reader_list", [&]() -> SEXP {
    const R_xlen_t n = Rf_xlength(list);
    uint64_t hash = 14695981039346656037ULL;
    R_xlen_t n_na = 0;
    for(R_xlen_t j = 0; j < n; ++j) {
      charport::Reader reader(VECTOR_ELT(list, j));
      for(charport::StrView value : reader) {
        if(value.is_na()) {
          ++n_na;
        } else {
          hash = fnv1a(value.ptr, static_cast<uint32_t>(value.len), hash);
        }
      }
    }
    return protected_hash_result(hash, n_na);
  });
}

// Measure one consumer-owned unwind boundary around the complete traversal.
// Each resolve finishes before its Reader is constructed, and access callbacks
// cannot call R, so no Reader is live when an R error can occur.
extern "C" SEXP C_benchl_hash_resolved_reader(SEXP list) {
  return guarded("hash_resolved_reader_list", [&]() -> SEXP {
    const R_xlen_t n = Rf_xlength(list);
    uint64_t hash = 14695981039346656037ULL;
    R_xlen_t n_na = 0;
    charport_consumer::unwind_protect([&]() -> SEXP {
      for(R_xlen_t j = 0; j < n; ++j) {
        charport_reader resolved = charport::resolve(VECTOR_ELT(list, j));
        charport::Reader reader(resolved);
        for(charport::StrView value : reader) {
          if(value.is_na()) {
            ++n_na;
          } else {
            hash = fnv1a(value.ptr, static_cast<uint32_t>(value.len), hash);
          }
        }
      }
      return R_NilValue;
    });
    return protected_hash_result(hash, n_na);
  });
}

extern "C" SEXP C_benchl_hash_string_elt(SEXP list) {
  return guarded("hash_string_elt_list", [&]() -> SEXP {
    const R_xlen_t n = Rf_xlength(list);
    uint64_t hash = 14695981039346656037ULL;
    R_xlen_t n_na = 0;
    for(R_xlen_t j = 0; j < n; ++j) {
      SEXP element = VECTOR_ELT(list, j);
      const R_xlen_t size = Rf_xlength(element);
      for(R_xlen_t i = 0; i < size; ++i) {
        SEXP value = STRING_ELT(element, i);
        if(value == NA_STRING) {
          ++n_na;
        } else {
          hash = fnv1a(
            R_CHAR(value), static_cast<uint32_t>(Rf_xlength(value)), hash);
        }
      }
    }
    return protected_hash_result(hash, n_na);
  });
}

extern "C" SEXP C_benchl_sumlen_reader(SEXP list) {
  return guarded("sumlen_reader_list", [&]() -> SEXP {
    const R_xlen_t n = Rf_xlength(list);
    double total = 0;
    for(R_xlen_t j = 0; j < n; ++j) {
      charport::Reader reader(VECTOR_ELT(list, j));
      for(R_xlen_t i = 0; i < reader.size(); ++i) {
        const int len = reader.length(i);
        if(len >= 0) {
          total += static_cast<double>(len);
        }
      }
    }
    return charport_consumer::unwind_protect(
      [total]() -> SEXP { return Rf_ScalarReal(total); }
    );
  });
}

extern "C" SEXP C_benchl_sumlen_resolved_reader(SEXP list) {
  return guarded("sumlen_resolved_reader_list", [&]() -> SEXP {
    const R_xlen_t n = Rf_xlength(list);
    double total = 0;
    charport_consumer::unwind_protect([&]() -> SEXP {
      for(R_xlen_t j = 0; j < n; ++j) {
        charport_reader resolved = charport::resolve(VECTOR_ELT(list, j));
        charport::Reader reader(resolved);
        for(R_xlen_t i = 0; i < reader.size(); ++i) {
          const int len = reader.length(i);
          if(len >= 0) {
            total += static_cast<double>(len);
          }
        }
      }
      return R_NilValue;
    });
    return charport_consumer::unwind_protect(
      [total]() -> SEXP { return Rf_ScalarReal(total); }
    );
  });
}

extern "C" SEXP C_benchl_sumlen_elt(SEXP list) {
  return guarded("sumlen_string_elt_list", [&]() -> SEXP {
    const R_xlen_t n = Rf_xlength(list);
    double total = 0;
    for(R_xlen_t j = 0; j < n; ++j) {
      SEXP element = VECTOR_ELT(list, j);
      const R_xlen_t size = Rf_xlength(element);
      for(R_xlen_t i = 0; i < size; ++i) {
        SEXP value = STRING_ELT(element, i);
        if(value != NA_STRING) {
          total += static_cast<double>(Rf_xlength(value));
        }
      }
    }
    return charport_consumer::unwind_protect(
      [total]() -> SEXP { return Rf_ScalarReal(total); }
    );
  });
}
