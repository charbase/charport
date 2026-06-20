#include "charvec_altrep.h"
#include "charport_registry.h"

#include <cinttypes>
#include <cstdio>

R_altrep_class_t charvec_altrep::class_t;

namespace {

bool is_charvec_sexp(SEXP x) {
  return R_altrep_inherits(x, charvec_altrep::class_t) == TRUE;
}

cpi::charvec_data & checked_store(SEXP x) {
  if(!is_charvec_sexp(x)) {
    throw std::runtime_error("x must be a charvec");
  }
  if(R_altrep_data2(x) != R_NilValue) {
    throw std::runtime_error("charvec is materialized; its store has been released");
  }
  auto * ptr = charvec_altrep::Ptr(x);
  if(ptr == nullptr) {
    throw std::runtime_error("charvec store pointer is null");
  }
  return *ptr;
}

size_t checked_len_arg(SEXP n_) {
  const double n = Rf_asReal(n_);
  if(ISNAN(n) || n < 0 ||
     n > static_cast<double>(std::numeric_limits<R_xlen_t>::max())) {
    throw std::runtime_error("invalid length");
  }
  return static_cast<size_t>(n);
}

const char * enc_name(charport_enc enc) {
  switch(enc) {
  case charport_enc::CE_NATIVE:        return "native";
  case charport_enc::CE_UTF8:          return "UTF-8";
  case charport_enc::CE_LATIN1:        return "latin1";
  case charport_enc::CE_BYTES:         return "bytes";
  case charport_enc::CE_ASCII_OR_UTF8: return "ascii_or_utf8";
  case charport_enc::CE_ASCII:         return "ascii";
  case charport_enc::CE_NA:            return "na";
  }
  return "invalid";
}

} // namespace

extern "C" {

SEXP C_charvec_from_character(SEXP x) {
  return charport_sexp_guard("charvec_from_character", [&]() -> SEXP {
    if(TYPEOF(x) != STRSXP) {
      throw std::runtime_error("x must be a character vector");
    }
    const R_xlen_t n = Rf_xlength(x);
    auto store = cp::charvec::Builder::build_store(n,
      [&](cpi::charvec_shard & sh, charport_strview * rec, size_t nn) {
        for(R_xlen_t i = 0; i < n; ++i) {
          cpi::copy_record(sh, rec, nn, static_cast<size_t>(i),
                           cpi::charsxp_to_view(STRING_ELT(x, i)));
        }
      });
    return charvec_altrep::Make(store.release(), true);
  });
}

SEXP C_charvec_alloc(SEXP n_) {
  return charport_sexp_guard("charvec_alloc", [&]() -> SEXP {
    const size_t n = checked_len_arg(n_);
    return charvec_altrep::Make(new cpi::charvec_data(n), true);
  });
}

SEXP C_is_charvec(SEXP x) {
  return Rf_ScalarLogical(is_charvec_sexp(x) ? TRUE : FALSE);
}

SEXP C_charport_materialize(SEXP x) {
  return charport_sexp_guard("charport_materialize", [&]() -> SEXP {
    if(is_charvec_sexp(x)) {
      charvec_altrep::Materialize(x);
      return x;
    }
    if(TYPEOF(x) != STRSXP) {
      throw std::runtime_error("x must be a character vector");
    }
    return x;
  });
}

SEXP C_charvec_stats(SEXP x) {
  return charport_sexp_guard("charvec_stats", [&]() -> SEXP {
    if(!is_charvec_sexp(x)) {
      throw std::runtime_error("x must be a charvec");
    }
    const bool materialized = R_altrep_data2(x) != R_NilValue;

    // The store keeps no byte/cursor accounting; n_slices (a chain walk) is the
    // only structural diagnostic left, NA once materialized (store released).
    const char * names[] = {"length", "n_slices", "materialized", ""};
    SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
    if(materialized) {
      SET_VECTOR_ELT(out, 0, Rf_ScalarReal(static_cast<double>(Rf_xlength(R_altrep_data2(x)))));
      SET_VECTOR_ELT(out, 1, Rf_ScalarReal(NA_REAL));
    } else {
      auto & store = checked_store(x);
      SET_VECTOR_ELT(out, 0, Rf_ScalarReal(static_cast<double>(store.records.size())));
      SET_VECTOR_ELT(out, 1, Rf_ScalarReal(static_cast<double>(store.slice_count())));
    }
    SET_VECTOR_ELT(out, 2, Rf_ScalarLogical(materialized ? TRUE : FALSE));
    UNPROTECT(1);
    return out;
  });
}

SEXP C_charvec_assign(SEXP x, SEXP i_, SEXP value) {
  return charport_sexp_guard("charvec_assign", [&]() -> SEXP {
    if(!is_charvec_sexp(x)) {
      throw std::runtime_error("x must be a charvec");
    }
    if(TYPEOF(value) != STRSXP || Rf_xlength(value) != 1) {
      throw std::runtime_error("value must be a character vector of length 1");
    }
    const double i = Rf_asReal(i_);
    const R_xlen_t len = charvec_altrep::Length(x);
    if(ISNAN(i) || i < 1 || i > static_cast<double>(len)) {
      throw std::runtime_error("index out of bounds");
    }
    charvec_altrep::string_Set_elt(x, static_cast<R_xlen_t>(i) - 1, STRING_ELT(value, 0));
    return x;
  });
}

SEXP C_charvec_element_addr(SEXP x, SEXP i_) {
  return charport_sexp_guard("charvec_element_addr", [&]() -> SEXP {
    auto & store = checked_store(x);
    const double i = Rf_asReal(i_);
    if(ISNAN(i) || i < 1 || i > static_cast<double>(store.records.size())) {
      throw std::runtime_error("index out of bounds");
    }
    const charport_strview & rec = store.view(static_cast<size_t>(i) - 1);
    if(rec.is_na()) {
      return Rf_ScalarString(NA_STRING);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%" PRIxPTR, reinterpret_cast<uintptr_t>(rec.ptr));
    return Rf_mkString(buf);
  });
}

SEXP C_charvec_compact(SEXP x) {
  return charport_sexp_guard("charvec_compact", [&]() -> SEXP {
    checked_store(x).compact();
    return x;
  });
}

SEXP C_charvec_encodings(SEXP x) {
  return charport_sexp_guard("charvec_encodings", [&]() -> SEXP {
    auto & store = checked_store(x);
    const size_t n = store.records.size();
    SEXP out = PROTECT(Rf_allocVector(STRSXP, static_cast<R_xlen_t>(n)));
    for(size_t i = 0; i < n; ++i) {
      const charport_strview & rec = store.view(i);
      if(rec.is_na()) {
        SET_STRING_ELT(out, static_cast<R_xlen_t>(i), NA_STRING);
      } else {
        SET_STRING_ELT(out, static_cast<R_xlen_t>(i), Rf_mkChar(enc_name(rec.enc)));
      }
    }
    UNPROTECT(1);
    return out;
  });
}

// Sharded construction driven serially (the builder contract: caller brings
// the threading framework; this exercises record sharing + block-move merge).
SEXP C_charvec_build_sharded(SEXP chunks) {
  return charport_sexp_guard("charvec_build_sharded", [&]() -> SEXP {
    if(TYPEOF(chunks) != VECSXP) {
      throw std::runtime_error("chunks must be a list of character vectors");
    }
    const R_xlen_t n_chunks = Rf_xlength(chunks);
    size_t total = 0;
    for(R_xlen_t j = 0; j < n_chunks; ++j) {
      SEXP ch = VECTOR_ELT(chunks, j);
      if(TYPEOF(ch) != STRSXP) {
        throw std::runtime_error("chunks must be a list of character vectors");
      }
      total = cpi::checked_add_size(total, static_cast<size_t>(Rf_xlength(ch)), "sharded length");
    }
    if(total > static_cast<size_t>(std::numeric_limits<R_xlen_t>::max())) {
      throw std::runtime_error("sharded length exceeds R_xlen_t");
    }

    if(n_chunks == 0) {
      return charvec_altrep::Make(new cpi::charvec_data(), true);  // empty list -> empty charvec
    }

    // one shard per chunk, each writing its chunk's contiguous output range
    cp::charvec::BuilderMT b(static_cast<R_xlen_t>(total), static_cast<size_t>(n_chunks));
    R_xlen_t offset = 0;
    for(R_xlen_t j = 0; j < n_chunks; ++j) {
      SEXP ch = VECTOR_ELT(chunks, j);
      const R_xlen_t ch_len = Rf_xlength(ch);
      for(R_xlen_t i = 0; i < ch_len; ++i) {
        b.set(static_cast<size_t>(j), offset + i, cpi::charsxp_to_view(STRING_ELT(ch, i)));
      }
      offset += ch_len;
    }
    auto store = b.release_store();
    return charvec_altrep::Make(store.release(), true);
  });
}

static const R_CallMethodDef call_entries[] = {
  {"C_charvec_from_character", (DL_FUNC) &C_charvec_from_character, 1},
  {"C_charvec_alloc",          (DL_FUNC) &C_charvec_alloc,          1},
  {"C_is_charvec",             (DL_FUNC) &C_is_charvec,             1},
  {"C_charport_materialize",   (DL_FUNC) &C_charport_materialize,   1},
  {"C_charvec_stats",          (DL_FUNC) &C_charvec_stats,          1},
  {"C_charvec_assign",         (DL_FUNC) &C_charvec_assign,         3},
  {"C_charvec_element_addr",   (DL_FUNC) &C_charvec_element_addr,   2},
  {"C_charvec_compact",        (DL_FUNC) &C_charvec_compact,        1},
  {"C_charvec_encodings",      (DL_FUNC) &C_charvec_encodings,      1},
  {"C_charvec_build_sharded",  (DL_FUNC) &C_charvec_build_sharded,  1},
  {"C_charport_backends",                (DL_FUNC) &C_charport_backends,                0},
  {"C_charport_backend_of",              (DL_FUNC) &C_charport_backend_of,              1},
  {"C_charport_reader_read_all",         (DL_FUNC) &C_charport_reader_read_all,         1},
  {"C_charport_reader_info",             (DL_FUNC) &C_charport_reader_info,             1},
  {"C_charport_test_unregister_charvec", (DL_FUNC) &C_charport_test_unregister_charvec, 0},
  {"C_charport_test_register_charvec",   (DL_FUNC) &C_charport_test_register_charvec,   0},
  {"C_charport_ccallable_check",         (DL_FUNC) &C_charport_ccallable_check,         0},
  {"C_cp_reader_roundtrip",              (DL_FUNC) &C_cp_reader_roundtrip,              1},
  {"C_cp_builder_from_reader",           (DL_FUNC) &C_cp_builder_from_reader,           2},
  {"C_cp_builder_reserve",               (DL_FUNC) &C_cp_builder_reserve,               2},
  {"C_cp_builder_errors",                (DL_FUNC) &C_cp_builder_errors,                0},
  {NULL, NULL, 0}
};

void R_init_charport(DllInfo * dll) {
  R_registerRoutines(dll, NULL, call_entries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  R_forceSymbols(dll, TRUE);
  charvec_altrep::Init(dll);
  charport_registry_init(dll);
}

} // extern "C"
