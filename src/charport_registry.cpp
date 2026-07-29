#include "charvec_altrep.h"
#include "charport_registry.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#include <Rversion.h>

namespace {

struct altrep_entry {
  R_altrep_class_t cls;
  charport_reader_state_fns state;
  charport_reader_range_fns range;
  charport_reader_index_fns index;
  charport_reader_capabilities capabilities;
};

std::vector<altrep_entry> & altrep_registry() {
  static std::vector<altrep_entry> reg;
  return reg;
}

void direct_fill_strview(SEXP cs, R_xlen_t out_i, const char ** out_ptrs,
                         int * out_lens, cetype_ext_t * out_encs) {
  if(cs == NA_STRING) {
    out_ptrs[out_i] = nullptr;
    out_lens[out_i] = NA_INTEGER;
    out_encs[out_i] = cetype_ext_t::CE_NA;
    return;
  }
  out_ptrs[out_i] = CHAR(cs);
  out_lens[out_i] = LENGTH(cs);
  out_encs[out_i] = cpi::classify_charsxp(cs);
}

void direct_fill_byteview(SEXP cs, R_xlen_t out_i, const char ** out_ptrs,
                          int * out_lens) {
  if(cs == NA_STRING) {
    out_ptrs[out_i] = nullptr;
    out_lens[out_i] = NA_INTEGER;
    return;
  }
  out_ptrs[out_i] = CHAR(cs);
  out_lens[out_i] = LENGTH(cs);
}

int direct_strviews_range(
    void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
    int * out_lens, cetype_ext_t * out_encs) {
  const SEXP * ptr = static_cast<const SEXP *>(state);
  for(R_xlen_t j = 0; j < size; ++j) {
    direct_fill_strview(ptr[start + j], j, out_ptrs, out_lens, out_encs);
  }
  return CHARPORT_STATUS_OK;
}

int direct_strviews_index(
    void * state, const R_xlen_t * indices, R_xlen_t size,
    const char ** out_ptrs, int * out_lens, cetype_ext_t * out_encs) {
  const SEXP * ptr = static_cast<const SEXP *>(state);
  for(R_xlen_t j = 0; j < size; ++j) {
    direct_fill_strview(ptr[indices[j]], j, out_ptrs, out_lens, out_encs);
  }
  return CHARPORT_STATUS_OK;
}

int direct_byteviews_range(
    void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
    int * out_lens) {
  const SEXP * ptr = static_cast<const SEXP *>(state);
  for(R_xlen_t j = 0; j < size; ++j) {
    direct_fill_byteview(ptr[start + j], j, out_ptrs, out_lens);
  }
  return CHARPORT_STATUS_OK;
}

int direct_byteviews_index(
    void * state, const R_xlen_t * indices, R_xlen_t size,
    const char ** out_ptrs, int * out_lens) {
  const SEXP * ptr = static_cast<const SEXP *>(state);
  for(R_xlen_t j = 0; j < size; ++j) {
    direct_fill_byteview(ptr[indices[j]], j, out_ptrs, out_lens);
  }
  return CHARPORT_STATUS_OK;
}

int direct_lengths_range(
    void * state, R_xlen_t start, R_xlen_t size, int * out_lens) {
  const SEXP * ptr = static_cast<const SEXP *>(state);
  for(R_xlen_t j = 0; j < size; ++j) {
    const SEXP cs = ptr[start + j];
    out_lens[j] = cs == NA_STRING ? NA_INTEGER : LENGTH(cs);
  }
  return CHARPORT_STATUS_OK;
}

int direct_lengths_index(
    void * state, const R_xlen_t * indices, R_xlen_t size, int * out_lens) {
  const SEXP * ptr = static_cast<const SEXP *>(state);
  for(R_xlen_t j = 0; j < size; ++j) {
    const SEXP cs = ptr[indices[j]];
    out_lens[j] = cs == NA_STRING ? NA_INTEGER : LENGTH(cs);
  }
  return CHARPORT_STATUS_OK;
}

int direct_encodings_range(
    void * state, R_xlen_t start, R_xlen_t size, cetype_ext_t * out_encs) {
  const SEXP * ptr = static_cast<const SEXP *>(state);
  for(R_xlen_t j = 0; j < size; ++j) {
    const SEXP cs = ptr[start + j];
    out_encs[j] = cs == NA_STRING ? cetype_ext_t::CE_NA : cpi::classify_charsxp(cs);
  }
  return CHARPORT_STATUS_OK;
}

int direct_encodings_index(
    void * state, const R_xlen_t * indices, R_xlen_t size,
    cetype_ext_t * out_encs) {
  const SEXP * ptr = static_cast<const SEXP *>(state);
  for(R_xlen_t j = 0; j < size; ++j) {
    const SEXP cs = ptr[indices[j]];
    out_encs[j] = cs == NA_STRING ? cetype_ext_t::CE_NA : cpi::classify_charsxp(cs);
  }
  return CHARPORT_STATUS_OK;
}

const altrep_entry * find_registered_altrep(SEXP x) {
  for(const altrep_entry & entry : altrep_registry()) {
    if(R_altrep_inherits(x, entry.cls) == TRUE) {
      return &entry;
    }
  }
  return nullptr;
}

charport_reader direct_reader(SEXP x, const void * direct) {
  charport_reader r;
  r.n = Rf_xlength(x);
  r.state = const_cast<void *>(direct);
  r.release = nullptr;
  r.range = charport_reader_range_fns{
    direct_strviews_range,
    direct_byteviews_range,
    direct_lengths_range,
    direct_encodings_range
  };
  r.index = charport_reader_index_fns{
    direct_strviews_index,
    direct_byteviews_index,
    direct_lengths_index,
    direct_encodings_index
  };
  r.capabilities = charport_reader_capabilities{true, false};
  return r;
}

bool range_fns_complete(charport_reader_range_fns range_fns) {
  return range_fns.strviews != NULL &&
         range_fns.byteviews != NULL &&
         range_fns.lengths != NULL &&
         range_fns.encodings != NULL;
}

bool index_fns_complete(charport_reader_index_fns index_fns) {
  return index_fns.strviews != NULL &&
         index_fns.byteviews != NULL &&
         index_fns.lengths != NULL &&
         index_fns.encodings != NULL;
}

const char * atom_string(SEXP x) {
  if(x == R_NilValue) {
    return nullptr;
  }
  switch(TYPEOF(x)) {
  case SYMSXP:
    return CHAR(PRINTNAME(x));
  case CHARSXP:
    return CHAR(x);
  default:
    return nullptr;
  }
}

SEXP string_or_na(const char * x) {
  return x == nullptr ? Rf_ScalarString(NA_STRING)
                      : Rf_ScalarString(Rf_mkCharCE(x, CE_UTF8));
}

SEXP class_string_or_na(const char * pkg, const char * cls) {
  if(pkg == nullptr || cls == nullptr) {
    return Rf_ScalarString(NA_STRING);
  }
  const size_t pkg_len = std::strlen(pkg);
  const size_t cls_len = std::strlen(cls);
  char * buf = static_cast<char *>(R_alloc(pkg_len + cls_len + 3, sizeof(char)));
  std::memcpy(buf, pkg, pkg_len);
  std::memcpy(buf + pkg_len, "::", 2);
  std::memcpy(buf + pkg_len + 2, cls, cls_len + 1);
  return Rf_ScalarString(Rf_mkCharCE(buf, CE_UTF8));
}

} // namespace

extern "C" void charport_register_altrep(R_altrep_class_t cls,
                                         charport_reader_state_fns state_fns,
                                         charport_reader_range_fns range_fns,
                                         charport_reader_index_fns index_fns,
                                         charport_reader_capabilities capabilities) {
  if(R_SEXP(cls) == NULL || state_fns.init == NULL ||
     !range_fns_complete(range_fns) || !index_fns_complete(index_fns)) {
    Rf_error("charport_register_altrep: reader callbacks must be non-NULL");
  }
  for(altrep_entry & entry : altrep_registry()) {
    if(R_SEXP(entry.cls) == R_SEXP(cls)) {
      Rf_error("charport_register_altrep: ALTREP class is already registered");
    }
  }
  char msg[512];
  try {
    altrep_registry().push_back(altrep_entry{
      cls, state_fns, range_fns, index_fns, capabilities
    });
    return;
  } catch(const std::exception & e) {
    std::snprintf(msg, sizeof(msg), "%s", e.what());
  } catch(...) {
    std::snprintf(msg, sizeof(msg), "unknown C++ exception");
  }
  Rf_error("charport_register_altrep: %s", msg);
}

extern "C" void charport_unregister_altrep(R_altrep_class_t cls) {
  std::vector<altrep_entry> & reg = altrep_registry();
  for(auto it = reg.begin(); it != reg.end(); ++it) {
    if(R_SEXP(it->cls) == R_SEXP(cls)) {
      reg.erase(it);
      return;
    }
  }
}

extern "C" charport_reader charport_resolve(SEXP x) {
  if(TYPEOF(x) != STRSXP) {
    Rf_error("charport_resolve: x must be a character vector");
  }

  if(const void * direct = DATAPTR_OR_NULL(x)) {
    return direct_reader(x, direct);
  }

  charport_reader r;
  r.n = Rf_xlength(x);
  if(ALTREP(x) == TRUE) {
    if(const altrep_entry * entry = find_registered_altrep(x)) {
      if(void * state = entry->state.init(x)) {
        r.state = state;
        r.release = entry->state.release;
        r.range = entry->range;
        r.index = entry->index;
        r.capabilities = entry->capabilities;
        return r;
      }
    }
  }
  return direct_reader(x, STRING_PTR_RO(x));
}

extern "C" int charport_abi_version(void) {
  return CHARPORT_ABI_VERSION;
}

extern "C" charport_sexp_info charport_get_sexp_info(SEXP x) {
  charport_sexp_info out;
  out.is_strsxp = TYPEOF(x) == STRSXP;
  out.length = out.is_strsxp ? Rf_xlength(x) : static_cast<R_xlen_t>(-1);
  out.is_altrep = out.is_strsxp && ALTREP(x) == TRUE;
  out.is_materialized = out.is_strsxp && DATAPTR_OR_NULL(x) != nullptr;
  out.is_registered = false;
  out.persistent_views = false;
  out.concurrent_access = false;
  out.stateful_reader = false;
  out.altrep_class_name = nullptr;
  out.altrep_class_package = nullptr;

  if(!out.is_altrep) {
    return out;
  }

#if (R_VERSION >= R_Version(4, 6, 0))
  out.altrep_class_name = atom_string(R_altrep_class_name(x));
  out.altrep_class_package = atom_string(R_altrep_class_package(x));
#else
  SEXP cls = ALTREP_CLASS(x);
  SEXP attrs = ATTRIB(cls);
  out.altrep_class_name = attrs == R_NilValue ? nullptr : atom_string(CAR(attrs));
  SEXP pkg = attrs == R_NilValue ? R_NilValue : CDR(attrs);
  out.altrep_class_package = pkg == R_NilValue ? nullptr : atom_string(CAR(pkg));
#endif

  if(const altrep_entry * entry = find_registered_altrep(x)) {
    out.is_registered = true;
    out.persistent_views = entry->capabilities.persistent_views;
    out.concurrent_access = entry->capabilities.concurrent_access;
    out.stateful_reader = entry->state.release != nullptr;
  }

  return out;
}

extern "C" SEXP C_charport_classes(void) {
  const std::vector<altrep_entry> & reg = altrep_registry();
  const char * names[] = {"n", "persistent_views", "concurrent_access",
                          "reentrant", ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SEXP n = PROTECT(Rf_ScalarInteger(static_cast<int>(reg.size())));
  SEXP persistent_views = PROTECT(Rf_allocVector(LGLSXP, static_cast<R_xlen_t>(reg.size())));
  SEXP concurrent_access = PROTECT(Rf_allocVector(LGLSXP, static_cast<R_xlen_t>(reg.size())));
  SEXP reentrant = PROTECT(Rf_allocVector(LGLSXP, static_cast<R_xlen_t>(reg.size())));
  SET_VECTOR_ELT(out, 0, n);
  SET_VECTOR_ELT(out, 1, persistent_views);
  SET_VECTOR_ELT(out, 2, concurrent_access);
  SET_VECTOR_ELT(out, 3, reentrant);
  for(size_t i = 0; i < reg.size(); ++i) {
    LOGICAL(persistent_views)[i] = reg[i].capabilities.persistent_views ? TRUE : FALSE;
    LOGICAL(concurrent_access)[i] = reg[i].capabilities.concurrent_access ? TRUE : FALSE;
    LOGICAL(reentrant)[i] =
      (reg[i].capabilities.persistent_views && reg[i].capabilities.concurrent_access) ? TRUE : FALSE;
  }
  UNPROTECT(5);
  return out;
}

extern "C" SEXP C_charport_info(SEXP x) {
  charport_sexp_info info = charport_get_sexp_info(x);
  const char * names[] = {
    "is_strsxp", "length", "is_altrep", "is_materialized", "is_registered",
    "persistent_views", "concurrent_access", "stateful_reader", "reentrant",
    "altrep_class_name", "altrep_class_package", "altrep_class", ""
  };
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SET_VECTOR_ELT(out, 0, Rf_ScalarLogical(info.is_strsxp ? TRUE : FALSE));
  SET_VECTOR_ELT(out, 1, Rf_ScalarReal(static_cast<double>(info.length)));
  SET_VECTOR_ELT(out, 2, Rf_ScalarLogical(info.is_altrep ? TRUE : FALSE));
  SET_VECTOR_ELT(out, 3, Rf_ScalarLogical(info.is_materialized ? TRUE : FALSE));
  SET_VECTOR_ELT(out, 4, Rf_ScalarLogical(info.is_registered ? TRUE : FALSE));
  SET_VECTOR_ELT(out, 5, Rf_ScalarLogical(info.persistent_views ? TRUE : FALSE));
  SET_VECTOR_ELT(out, 6, Rf_ScalarLogical(info.concurrent_access ? TRUE : FALSE));
  SET_VECTOR_ELT(out, 7, Rf_ScalarLogical(info.stateful_reader ? TRUE : FALSE));
  SET_VECTOR_ELT(out, 8, Rf_ScalarLogical(info.reentrant() ? TRUE : FALSE));
  SET_VECTOR_ELT(out, 9, string_or_na(info.altrep_class_name));
  SET_VECTOR_ELT(out, 10, string_or_na(info.altrep_class_package));
  SET_VECTOR_ELT(out, 11, class_string_or_na(info.altrep_class_package,
                                             info.altrep_class_name));
  UNPROTECT(1);
  return out;
}

extern "C" SEXP C_unregister_charvec(void) {
  charport_unregister_altrep(charvec_altrep::class_t);
  return R_NilValue;
}

extern "C" SEXP C_register_charvec(void) {
  charport_register_altrep(
    charvec_altrep::class_t,
    charport_reader_state_fns{charvec_altrep::reader_init, nullptr},
    charport_reader_range_fns{
      charvec_altrep::reader_strviews_range,
      charvec_altrep::reader_byteviews_range,
      charvec_altrep::reader_lengths_range,
      charvec_altrep::reader_encodings_range
    },
    charport_reader_index_fns{
      charvec_altrep::reader_strviews_index,
      charvec_altrep::reader_byteviews_index,
      charvec_altrep::reader_lengths_index,
      charvec_altrep::reader_encodings_index
    },
    charport_reader_capabilities{true, true}
  );
  return R_NilValue;
}

extern "C" void charport_registry_init(DllInfo * dll) {
  R_RegisterCCallable("charport", "charport_register_altrep",
                      reinterpret_cast<DL_FUNC>(&charport_register_altrep));
  R_RegisterCCallable("charport", "charport_unregister_altrep",
                      reinterpret_cast<DL_FUNC>(&charport_unregister_altrep));
  R_RegisterCCallable("charport", "charport_resolve",
                      reinterpret_cast<DL_FUNC>(&charport_resolve));
  R_RegisterCCallable("charport", "charport_sexp_info",
                      reinterpret_cast<DL_FUNC>(&charport_get_sexp_info));
  R_RegisterCCallable("charport", "charport_abi_version",
                      reinterpret_cast<DL_FUNC>(&charport_abi_version));
  R_RegisterCCallable("charport", "charport_charvec_wrap",
                      reinterpret_cast<DL_FUNC>(&charport_charvec_wrap));
  R_RegisterCCallable(
    "charport", "charport_charvec_from_views",
    reinterpret_cast<DL_FUNC>(&charport_charvec_from_views_impl)
  );
  (void)dll;
}
