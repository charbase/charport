#include "charvec_altrep.h"
#include "charport_registry.h"

#include <limits>
#include <memory>
#include <stdexcept>

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

} // namespace

extern "C" SEXP charport_charvec_wrap(void * store_) {
  if(store_ == nullptr) {
    Rf_error("charport charvec wrap: store is NULL");
  }
  std::unique_ptr<cpi::charvec_data> store(static_cast<cpi::charvec_data *>(store_));
  return charport_sexp_guard("charvec wrap", [&]() -> SEXP {
    return charvec_altrep::Make(store.release(), true);
  });
}

extern "C" SEXP C_as_charvec(SEXP x) {
  return charport_sexp_guard("as_charvec", [&]() -> SEXP {
    if(TYPEOF(x) != STRSXP) {
      throw std::runtime_error("x must be a character vector");
    }
    const R_xlen_t n = Rf_xlength(x);
    auto store = charport::charvec::Builder::build_store(n,
      [&](cpi::charvec_shard & shard, charport_strview * rec, size_t nn) {
        for(R_xlen_t i = 0; i < n; ++i) {
          cpi::copy_record(shard, rec, nn, static_cast<size_t>(i),
                           cpi::charsxp_to_view(STRING_ELT(x, i)));
        }
      });
    return charvec_altrep::Make(store.release(), true);
  });
}

extern "C" SEXP C_charvec_alloc(SEXP n_) {
  return charport_sexp_guard("charvec_alloc", [&]() -> SEXP {
    return charvec_altrep::Make(new cpi::charvec_data(checked_len_arg(n_)), true);
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
      SET_VECTOR_ELT(out, 0, Rf_ScalarReal(static_cast<double>(store.records.size())));
      SET_VECTOR_ELT(out, 1, Rf_ScalarReal(static_cast<double>(store.slice_count())));
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
    checked_store(x).compact();
    return x;
  });
}
