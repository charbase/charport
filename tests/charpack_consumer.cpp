// Consumer of the public charpack and charvec in-place fill API, compiled
// against the installed headers like a downstream package would be.

#include <Rinternals.h>
#include <Rversion.h>
#include "consumer-boundary.h"
#include "charport.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

// Views of an ordinary character vector, gathered through the Reader.
struct Views {
  std::vector<const char *> ptrs;
  std::vector<int> lengths;
  std::vector<cetype_ext_t> encodings;
  std::vector<char> joined;  // non-NA payload, in order
};

void read_views(SEXP strings, Views & out) {
  charport::Reader r(strings);
  const R_xlen_t n = r.size();
  charport::StrViews views(n);
  if(n > 0) {
    r.views(0, n, views);
  }
  for(R_xlen_t i = 0; i < n; ++i) {
    const charport::StrView v = views[i];
    out.ptrs.push_back(v.ptr);
    out.lengths.push_back(v.len);
    out.encodings.push_back(v.enc);
    if(!v.is_na()) {
      out.joined.insert(out.joined.end(), v.ptr, v.ptr + v.len);
    }
  }
}

} // namespace

extern "C" {

SEXP C_cpc_callables(void) {
  const char * names[] = {
    "charport_charvec_alloc_na_v1",
    "charport_charvec_get_store_v1",
    "charport_charpack_placeholder_v1",
    "charport_charpack_fill_views_v1",
    "charport_charpack_fill_slices_v1",
    "charport_charpack_fill_materialized_na_v1",
    "charport_is_charpack_v1",
    ""
  };
  SEXP out = PROTECT(Rf_mkNamed(LGLSXP, names));
  for(int i = 0; i < 7; ++i) {
    LOGICAL(out)[i] = R_GetCCallable("charport", names[i]) != NULL ? TRUE : FALSE;
  }
  UNPROTECT(1);
  return out;
}

// Capabilities of the reader charport resolves for x.
SEXP C_cpc_reader_caps(SEXP x) {
  return charport_consumer::boundary("reader_caps", [&]() -> SEXP {
    charport::Reader r(x);
    const bool persistent = r.persistent_views();
    const bool concurrent = r.concurrent_access();
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(LGLSXP, 2));
      LOGICAL(out)[0] = persistent ? TRUE : FALSE;
      LOGICAL(out)[1] = concurrent ? TRUE : FALSE;
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_cpc_same(SEXP a, SEXP b) {
  return Rf_ScalarLogical(a == b ? TRUE : FALSE);
}

// The vector inside a base wrap_string, or NULL.
SEXP C_cpc_wrapped(SEXP x) {
#if (R_VERSION >= R_Version(4, 6, 0))
  if(ALTREP(x) && R_altrep_class_name(x) == Rf_install("wrap_string") &&
     R_altrep_class_package(x) == Rf_install("base")) {
    return R_altrep_data1(x);
  }
#endif
  (void)x;
  return R_NilValue;
}

SEXP C_cpc_from_views(SEXP strings) {
  return charport_consumer::boundary("from_views", [&]() -> SEXP {
    Views v;
    read_views(strings, v);
    const R_xlen_t n = static_cast<R_xlen_t>(v.ptrs.size());
    return charport_consumer::unwind_protect([&]() -> SEXP {
      return charport::charpack::from_views(n, v.ptrs.data(), v.lengths.data(), v.encodings.data());
    });
  });
}

// The payload is cut into n_slices pieces at arbitrary byte offsets.
SEXP C_cpc_from_slices(SEXP strings, SEXP n_slices_) {
  return charport_consumer::boundary("from_slices", [&]() -> SEXP {
    Views v;
    read_views(strings, v);
    const R_xlen_t n = static_cast<R_xlen_t>(v.ptrs.size());
    const size_t n_slices = static_cast<size_t>(Rf_asInteger(n_slices_));
    std::vector<const char *> slices;
    std::vector<size_t> slice_bytes;
    size_t offset = 0;
    for(size_t k = 0; k < n_slices; ++k) {
      const size_t end = v.joined.size() * (k + 1) / n_slices;
      slices.push_back(v.joined.data() + offset);
      slice_bytes.push_back(end - offset);
      offset = end;
    }
    return charport_consumer::unwind_protect([&]() -> SEXP {
      return charport::charpack::from_slices(
        n, v.lengths.data(), v.encodings.data(), static_cast<R_xlen_t>(n_slices),
        slices.data(), slice_bytes.data());
    });
  });
}

// Fills a placeholder once, then tries again; the second fill must fail.
SEXP C_cpc_fill_twice(SEXP strings) {
  return charport_consumer::boundary("fill_twice", [&]() -> SEXP {
    Views v;
    read_views(strings, v);
    const R_xlen_t n = static_cast<R_xlen_t>(v.ptrs.size());
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP x = PROTECT(charport::charpack::placeholder());
      charport::charpack::fill_views(x, n, v.ptrs.data(), v.lengths.data(), v.encodings.data());
      charport::charpack::fill_views(x, n, v.ptrs.data(), v.lengths.data(), v.encodings.data());
      UNPROTECT(1);
      return x;
    });
  });
}

// fill_materialized_na, then new CHARSXPs written into the returned vector,
// which only x protects.
SEXP C_cpc_fill_materialized_na(SEXP strings) {
  const R_xlen_t n = Rf_xlength(strings);
  SEXP x = PROTECT(charport::charpack::placeholder());
  SEXP out = charport::charpack::fill_materialized_na(x, n);
  for(R_xlen_t i = 0; i < n; ++i) {
    SEXP s = STRING_ELT(strings, i);
    if(s != NA_STRING) {
      SET_STRING_ELT(out, i, Rf_mkCharLenCE(CHAR(s), LENGTH(s), Rf_getCharCE(s)));
    }
  }
  UNPROTECT(1);
  return x;
}

SEXP C_cpc_is_charpack(SEXP x) {
  return Rf_ScalarLogical(charport::charpack::is_charpack(x) ? TRUE : FALSE);
}

// alloc_na, then fill in place through get_store and Writer, in reverse
// order. With partial = TRUE, stop at the midpoint with an out-of-range write,
// which must leave a valid vector whose unwritten records are NA.
SEXP C_cpc_charvec_fill(SEXP strings, SEXP partial_) {
  return charport_consumer::boundary("charvec_fill", [&]() -> SEXP {
    Views v;
    read_views(strings, v);
    const R_xlen_t n = static_cast<R_xlen_t>(v.ptrs.size());
    const bool partial = Rf_asLogical(partial_) == TRUE;
    SEXP x = charport_consumer::unwind_protect([&]() -> SEXP {
      return charport::charvec::alloc_na(n);
    });
    PROTECT(x);
    charport::charvec::Store * store = charport::charvec::get_store(x);
    if(store == nullptr) {
      UNPROTECT(1);
      throw std::runtime_error("get_store returned NULL for a fresh charvec");
    }
    charport::charvec::Writer writer(*store);
    try {
      for(R_xlen_t i = n - 1; i >= 0; --i) {
        if(partial && i == n / 2) {
          writer.set_na(static_cast<size_t>(n));
        }
        const size_t len = v.lengths[i] == NA_INTEGER ? 0 : static_cast<size_t>(v.lengths[i]);
        writer.copy(static_cast<size_t>(i), v.ptrs[i], len, v.encodings[i]);
      }
    } catch(const std::out_of_range &) {
      if(!partial) {
        UNPROTECT(1);
        throw;
      }
    }
    UNPROTECT(1);
    return x;
  });
}

SEXP C_cpc_get_store_is_null(SEXP x) {
  return Rf_ScalarLogical(charport::charvec::get_store(x) == nullptr ? TRUE : FALSE);
}

// Reads x[start + 1, ..., start + size] and x[indices + 1] through every
// Reader access path. Returns list(range, index), each a list of the bytes
// (raw, or NULL for NA), view lengths, byteview lengths, lengths() and
// encodings(), so a charpack can be compared with the same strings in an
// ordinary vector, which the Reader serves through R.
SEXP C_cpc_reader_access(SEXP x, SEXP start_, SEXP size_, SEXP indices_) {
  return charport_consumer::boundary("reader_access", [&]() -> SEXP {
    const R_xlen_t start = static_cast<R_xlen_t>(Rf_asReal(start_));
    const R_xlen_t size = static_cast<R_xlen_t>(Rf_asReal(size_));
    const R_xlen_t n_index = Rf_xlength(indices_);
    std::vector<R_xlen_t> indices(n_index);
    for(R_xlen_t j = 0; j < n_index; ++j) {
      indices[j] = static_cast<R_xlen_t>(REAL(indices_)[j]);
    }
    struct Access {
      std::vector<const char *> ptrs, byte_ptrs;
      std::vector<int> lens, byte_lens, lengths;
      std::vector<cetype_ext_t> encs, encodings;
      explicit Access(R_xlen_t m)
        : ptrs(m + 1), byte_ptrs(m + 1), lens(m + 1), byte_lens(m + 1), lengths(m + 1),
          encs(m + 1), encodings(m + 1) {}
    };
    Access range(size), index(n_index);
    charport::Reader r(x);
    r.views(start, size, range.ptrs.data(), range.lens.data(), range.encs.data());
    r.byteviews(start, size, range.byte_ptrs.data(), range.byte_lens.data());
    r.lengths(start, size, range.lengths.data());
    r.encodings(start, size, range.encodings.data());
    r.views(indices.data(), n_index, index.ptrs.data(), index.lens.data(), index.encs.data());
    r.byteviews(indices.data(), n_index, index.byte_ptrs.data(), index.byte_lens.data());
    r.lengths(indices.data(), n_index, index.lengths.data());
    r.encodings(indices.data(), n_index, index.encodings.data());
    for(R_xlen_t j = 0; j < size; ++j) {
      if(range.ptrs[j] != range.byte_ptrs[j] || range.encs[j] != range.encodings[j]) {
        throw std::runtime_error("views and byteviews or encodings disagree");
      }
    }
    for(R_xlen_t j = 0; j < n_index; ++j) {
      if(index.ptrs[j] != index.byte_ptrs[j] || index.encs[j] != index.encodings[j]) {
        throw std::runtime_error("indexed views and byteviews or encodings disagree");
      }
    }
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(VECSXP, 2));
      Access * parts[2] = {&range, &index};
      const R_xlen_t counts[2] = {size, n_index};
      for(int k = 0; k < 2; ++k) {
        const Access & a = *parts[k];
        const R_xlen_t m = counts[k];
        SEXP part = PROTECT(Rf_allocVector(VECSXP, 5));
        SEXP bytes = PROTECT(Rf_allocVector(VECSXP, m));
        SEXP lens = PROTECT(Rf_allocVector(INTSXP, m));
        SEXP byte_lens = PROTECT(Rf_allocVector(INTSXP, m));
        SEXP lengths = PROTECT(Rf_allocVector(INTSXP, m));
        SEXP encs = PROTECT(Rf_allocVector(INTSXP, m));
        for(R_xlen_t j = 0; j < m; ++j) {
          if(a.ptrs[j] != nullptr) {
            SEXP b = Rf_allocVector(RAWSXP, a.lens[j]);
            SET_VECTOR_ELT(bytes, j, b);
            if(a.lens[j] > 0) std::memcpy(RAW(b), a.ptrs[j], static_cast<size_t>(a.lens[j]));
          }
          INTEGER(lens)[j] = a.lens[j];
          INTEGER(byte_lens)[j] = a.byte_lens[j];
          INTEGER(lengths)[j] = a.lengths[j];
          INTEGER(encs)[j] = a.encs[j].value;
        }
        SET_VECTOR_ELT(part, 0, bytes);
        SET_VECTOR_ELT(part, 1, lens);
        SET_VECTOR_ELT(part, 2, byte_lens);
        SET_VECTOR_ELT(part, 3, lengths);
        SET_VECTOR_ELT(part, 4, encs);
        SET_VECTOR_ELT(out, k, part);
        UNPROTECT(6);
      }
      UNPROTECT(1);
      return out;
    });
  });
}

} // extern "C"
