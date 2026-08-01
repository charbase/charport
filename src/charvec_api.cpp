#include "charvec_altrep.h"
#include "charport_registry.h"

#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {

bool is_charvec_sexp(SEXP x) {
  return R_altrep_inherits(x, charvec_altrep::class_t) == TRUE;
}

cpv::Store & checked_store(SEXP x) {
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

bool valid_bulk_encoding(cetype_ext_t enc) noexcept {
  switch(enc.value) {
  case CETYPE_EXT_NATIVE.value:
  case CETYPE_EXT_UTF8.value:
  case CETYPE_EXT_LATIN1.value:
  case CETYPE_EXT_BYTES.value:
  case CETYPE_EXT_ASCII_OR_UTF8.value:
  case CETYPE_EXT_ASCII.value:
  case CETYPE_EXT_NA.value:
    return true;
  }
  return false;
}

bool bulk_view_is_na(const char * ptr, int len, cetype_ext_t enc) noexcept {
  return ptr == nullptr || len == NA_INTEGER || enc == CETYPE_EXT_NA;
}

struct bulk_builder_state {
  charport::charvec::Builder * builder;
};

SEXP bulk_builder_to_sexp(void * data) noexcept {
  bulk_builder_state * state = static_cast<bulk_builder_state *>(data);
  return state->builder->to_sexp();
}

void bulk_builder_cleanup(void * data, Rboolean) noexcept {
  bulk_builder_state * state = static_cast<bulk_builder_state *>(data);
  delete state->builder;
  state->builder = nullptr;
}

} // namespace

extern "C" SEXP charport_charvec_wrap(void * store_) {
  return charvec_altrep::MoveStore(static_cast<cpv::Store *>(store_));
}

extern "C" SEXP charport_charvec_from_views_impl(
    R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings) {
  if(n < 0) {
    Rf_error("charport charvec from views: negative length");
  }
  if(n > 0 && (ptrs == nullptr || lengths == nullptr || encodings == nullptr)) {
    Rf_error("charport charvec from views: input arrays must not be NULL");
  }
  for(R_xlen_t i = 0; i < n; ++i) {
    if(!valid_bulk_encoding(encodings[i])) {
      Rf_error("charport charvec from views: invalid encoding at index %lld",
               static_cast<long long>(i));
    }
    if(!bulk_view_is_na(ptrs[i], lengths[i], encodings[i]) && lengths[i] < 0) {
      Rf_error("charport charvec from views: negative length at index %lld",
               static_cast<long long>(i));
    }
  }

  SEXP token = PROTECT(R_MakeUnwindCont());
  charport::charvec::Builder * builder = nullptr;
  char message[512];
  bool cpp_error = false;
  try {
    builder = new charport::charvec::Builder(n);
    for(R_xlen_t i = 0; i < n; ++i) {
      if(bulk_view_is_na(ptrs[i], lengths[i], encodings[i])) {
        builder->set_na(i);
      } else {
        builder->set(i, ptrs[i], static_cast<size_t>(lengths[i]), encodings[i]);
      }
    }
  } catch(const std::exception & error) {
    std::snprintf(message, sizeof(message), "%s", error.what());
    cpp_error = true;
  } catch(...) {
    std::snprintf(message, sizeof(message), "unknown C++ exception");
    cpp_error = true;
  }

  if(cpp_error) {
    delete builder;
    UNPROTECT(1);
    Rf_error("charport charvec from views: %s", message);
  }

  bulk_builder_state state{builder};
  SEXP out = R_UnwindProtect(
    &bulk_builder_to_sexp, &state,
    &bulk_builder_cleanup, &state, token
  );
  SETCAR(token, R_NilValue);
  UNPROTECT(1);
  return out;
}

extern "C" SEXP C_as_charvec(SEXP x) {
  return charport_sexp_guard("as_charvec", [&]() -> SEXP {
    if(TYPEOF(x) != STRSXP) {
      throw std::runtime_error("x must be a character vector");
    }
    const R_xlen_t n = Rf_xlength(x);
    const SEXP * ptr = STRING_PTR_RO(x);
    charport::charvec::Builder builder(n);
    for(R_xlen_t i = 0; i < n; ++i) {
      builder.set(i, cpi::charsxp_to_view(ptr[i]));
    }
    cpv::Store store = builder.release_store();
    return charvec_altrep::MoveStore(&store);
  });
}

extern "C" SEXP C_charvec_alloc(SEXP n_) {
  return charport_sexp_guard("charvec_alloc", [&]() -> SEXP {
    cpv::Store store(checked_len_arg(n_), 0);
    return charvec_altrep::MoveStore(&store);
  });
}

extern "C" SEXP C_is_charvec(SEXP x) {
  return Rf_ScalarLogical(is_charvec_sexp(x) ? TRUE : FALSE);
}

extern "C" SEXP C_charport_materialize(SEXP x) {
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

extern "C" SEXP C_charvec_stats(SEXP x) {
  return charport_sexp_guard("charvec_stats", [&]() -> SEXP {
    if(!is_charvec_sexp(x)) {
      throw std::runtime_error("x must be a charvec");
    }
    const bool materialized = R_altrep_data2(x) != R_NilValue;
    const char * names[] = {"length", "n_slices", "materialized", ""};
    SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
    if(materialized) {
      SET_VECTOR_ELT(out, 0, Rf_ScalarReal(static_cast<double>(Rf_xlength(R_altrep_data2(x)))));
      SET_VECTOR_ELT(out, 1, Rf_ScalarReal(NA_REAL));
    } else {
      auto & store = checked_store(x);
      SET_VECTOR_ELT(out, 0, Rf_ScalarReal(static_cast<double>(store.size())));
      SET_VECTOR_ELT(out, 1, Rf_ScalarReal(static_cast<double>(store.slices.count())));
    }
    SET_VECTOR_ELT(out, 2, Rf_ScalarLogical(materialized ? TRUE : FALSE));
    UNPROTECT(1);
    return out;
  });
}

extern "C" SEXP C_charvec_assign(SEXP x, SEXP i_, SEXP value) {
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

extern "C" SEXP C_charvec_compact(SEXP x) {
  return charport_sexp_guard("charvec_compact", [&]() -> SEXP {
    charvec_detail::compact_store(checked_store(x));
    return x;
  });
}
