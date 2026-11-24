#include "charvec_altrep.h"
#include "charport_registry.h"

#include <cstdio>
#include <exception>
#include <limits>
#include <memory>

namespace {

bool is_charvec_sexp(SEXP x) {
  return R_altrep_inherits(x, charvec_altrep::class_t) == TRUE;
}

struct bulk_builder_state {
  charport::charvec::Builder * builder;
};

SEXP bulk_builder_to_sexp(void * data) noexcept {
  bulk_builder_state * state = static_cast<bulk_builder_state *>(data);
  return state->builder->to_sexp();
}

void bulk_builder_cleanup(void * data) noexcept {
  bulk_builder_state * state = static_cast<bulk_builder_state *>(data);
  delete state->builder;
  state->builder = nullptr;
}

} // namespace

// Public Builder objects retain ownership of their Store member; this ABI
// wrapper only borrows that member while MoveStore performs the public move.
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
    if(!cpi::valid_view_encoding(encodings[i])) {
      Rf_error("charport charvec from views: invalid encoding at index %lld",
               static_cast<long long>(i));
    }
    if(!cpi::view_is_na(ptrs[i], lengths[i], encodings[i]) && lengths[i] < 0) {
      Rf_error("charport charvec from views: negative length at index %lld",
               static_cast<long long>(i));
    }
  }

  charport::charvec::Builder * builder = nullptr;
  char message[512];
  bool cpp_error = false;
  try {
    builder = new charport::charvec::Builder(n);
    for(R_xlen_t i = 0; i < n; ++i) {
      if(cpi::view_is_na(ptrs[i], lengths[i], encodings[i])) {
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
    Rf_error("charport charvec from views: %s", message);
  }

  bulk_builder_state state{builder};
  return R_ExecWithCleanup(
    &bulk_builder_to_sexp, &state, &bulk_builder_cleanup, &state);
}

// Ordinary vectors are read directly; ALTREP input goes through the broker in
// fixed batches, each copied before the next read. Median times for 1e6 short
// strings: ordinary input, direct 23.5 ms, batches 25 ms, one Reader call per
// element 27 ms, one call for the whole vector 34 ms (its three n-length view
// arrays fall out of cache); charpack input, batches 8.3 ms, whole vector
// 9.7 ms, per element 10.7 ms.
extern "C" SEXP C_as_charvec(SEXP x) {
  if(TYPEOF(x) != STRSXP) {
    Rf_error("charport as_charvec: x must be a character vector");
  }
  if(ALTREP(x) != TRUE) {
    const R_xlen_t n = Rf_xlength(x);
    const SEXP * src = STRING_PTR_RO(x);
    return charport_sexp_guard("as_charvec", [&]() -> SEXP {
      charvec_detail::owned_store * store;
      {
        cpv::Builder builder(n);
        for(R_xlen_t i = 0; i < n; ++i) {
          builder.set(i, cpi::charsxp_to_view(src[i]));
        }
        store = charvec_detail::heap_store(builder.release_store());
      }
      return charvec_detail::wrap_store_with_cleanup(store);
    });
  }

  const charport_reader resolved = charport::resolve(x);
  return charport_sexp_guard("as_charvec", [&]() -> SEXP {
    charvec_detail::owned_store * store;
    {
      charport::Reader reader(resolved);
      const R_xlen_t n = reader.size();
      const R_xlen_t batch = 256;
      const char * ptrs[256];
      int lengths[256];
      cetype_ext_t encodings[256];
      cpv::Builder builder(n);
      for(R_xlen_t start = 0; start < n; start += batch) {
        const R_xlen_t count = n - start < batch ? n - start : batch;
        reader.views(start, count, ptrs, lengths, encodings);
        for(R_xlen_t j = 0; j < count; ++j) {
          builder.set(start + j, make_strview(ptrs[j], lengths[j], encodings[j]));
        }
      }
      store = charvec_detail::heap_store(builder.release_store());
    }
    return charvec_detail::wrap_store_with_cleanup(store);
  });
}

extern "C" SEXP charport_charvec_alloc_na_impl(R_xlen_t n) {
  if(n < 0) {
    Rf_error("charport charvec alloc_na: negative length");
  }
  return charport_sexp_guard("charvec alloc_na", [&]() -> SEXP {
    charvec_detail::owned_store * store =
      charvec_detail::heap_store(cpv::Store(static_cast<size_t>(n), 0));
    for(size_t i = 0; i < store->size(); ++i) store->records.set_na(i);
    return charvec_detail::wrap_store_with_cleanup(store);
  });
}

// No data2 and no Elt cache means R holds no CHARSXP derived from the Store.
// get_store() cannot see other references to the vector, so the caller must
// own it (see writer.h).
extern "C" void * charport_charvec_get_store_impl(SEXP x) {
  if(TYPEOF(x) != STRSXP || ALTREP(x) != TRUE || !is_charvec_sexp(x)) {
    return nullptr;
  }
  if(R_altrep_data2(x) != R_NilValue) {
    return nullptr;
  }
  SEXP xp = R_altrep_data1(x);
  if(R_ExternalPtrProtected(xp) != R_NilValue) {
    return nullptr;
  }
  return R_ExternalPtrAddr(xp);
}

extern "C" SEXP C_is_charvec(SEXP x) {
  return Rf_ScalarLogical(is_charvec_sexp(x) ? TRUE : FALSE);
}

extern "C" SEXP C_charport_materialize(SEXP x) {
  if(TYPEOF(x) != STRSXP) {
    Rf_error("charport charport_materialize: x must be a character vector");
  }
  (void) STRING_PTR_RO(x);
  return x;
}
