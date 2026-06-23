#include "charvec_altrep.h"
#include "charport_registry.h"

#include <cstdio>
#include <exception>
#include <vector>

#include <Rversion.h>

namespace {

struct altrep_entry {
  R_altrep_class_t cls;
  charport_init_fn init;
  charport_get_strview_fn get;
  bool view_persistence;
  bool thread_safe_access;
};

std::vector<altrep_entry> & altrep_registry() {
  static std::vector<altrep_entry> reg;
  return reg;
}

charport_strview direct_get(void * state, R_xlen_t i) {
  const SEXP cs = static_cast<const SEXP *>(state)[i];
  if(cs == NA_STRING) {
    return make_strview(nullptr, 0, charport_enc::CE_NA);
  }
  return make_strview(CHAR(cs),
                      static_cast<uint32_t>(Rf_xlength(cs)),
                      cpi::classify_charsxp(cs));
}

charport_reader direct_reader(SEXP x, const void * direct, charport_reader_kind kind) {
  charport_reader r;
  r.n = Rf_xlength(x);
  r.state = const_cast<void *>(direct);
  r.get = direct_get;
  r.view_persistence = true;
  r.thread_safe_access = false;
  r.kind = kind;
  return r;
}

} // namespace

extern "C" void charport_register_altrep(R_altrep_class_t cls,
                                         charport_init_fn init,
                                         charport_get_strview_fn get_strview,
                                         bool view_persistence,
                                         bool thread_safe_access) {
  if(R_SEXP(cls) == NULL || init == NULL || get_strview == NULL) {
    Rf_error("charport_register_altrep: cls, init, and get_strview must be non-NULL");
  }
  for(altrep_entry & entry : altrep_registry()) {
    if(R_SEXP(entry.cls) == R_SEXP(cls)) {
      Rf_error("charport_register_altrep: ALTREP class is already registered");
    }
  }
  try {
    altrep_registry().push_back(altrep_entry{
      cls, init, get_strview, view_persistence, thread_safe_access
    });
  } catch(const std::exception & e) {
    Rf_error("charport_register_altrep: %s", e.what());
  }
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

  const bool is_altrep = ALTREP(x) == TRUE;
  if(const void * direct = DATAPTR_OR_NULL(x)) {
    return direct_reader(x, direct,
                         is_altrep ? charport_reader_kind::MATERIALIZED_ALTREP :
                                      charport_reader_kind::PLAIN);
  }

  charport_reader r;
  r.n = Rf_xlength(x);
  for(const altrep_entry & entry : altrep_registry()) {
    if(R_altrep_inherits(x, entry.cls) == TRUE) {
      if(void * state = entry.init(x)) {
        r.state = state;
        r.get = entry.get;
        r.view_persistence = entry.view_persistence;
        r.thread_safe_access = entry.thread_safe_access;
        r.kind = charport_reader_kind::REGISTERED_ALTREP;
        return r;
      }
      break;
    }
  }
  return direct_reader(x, STRING_PTR_RO(x),
                       is_altrep ? charport_reader_kind::FALLBACK_ALTREP :
                                    charport_reader_kind::PLAIN);
}

extern "C" int charport_abi_version(void) {
  return CHARPORT_ABI_VERSION;
}

extern "C" SEXP C_charport_classes(void) {
  const std::vector<altrep_entry> & reg = altrep_registry();
  const char * names[] = {"n", "view_persistence", "thread_safe_access",
                          "reentrant", ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SEXP n = PROTECT(Rf_ScalarInteger(static_cast<int>(reg.size())));
  SEXP view_persistence = PROTECT(Rf_allocVector(LGLSXP, static_cast<R_xlen_t>(reg.size())));
  SEXP thread_safe_access = PROTECT(Rf_allocVector(LGLSXP, static_cast<R_xlen_t>(reg.size())));
  SEXP reentrant = PROTECT(Rf_allocVector(LGLSXP, static_cast<R_xlen_t>(reg.size())));
  SET_VECTOR_ELT(out, 0, n);
  SET_VECTOR_ELT(out, 1, view_persistence);
  SET_VECTOR_ELT(out, 2, thread_safe_access);
  SET_VECTOR_ELT(out, 3, reentrant);
  for(size_t i = 0; i < reg.size(); ++i) {
    LOGICAL(view_persistence)[i] = reg[i].view_persistence ? TRUE : FALSE;
    LOGICAL(thread_safe_access)[i] = reg[i].thread_safe_access ? TRUE : FALSE;
    LOGICAL(reentrant)[i] =
      (reg[i].view_persistence && reg[i].thread_safe_access) ? TRUE : FALSE;
  }
  UNPROTECT(5);
  return out;
}

extern "C" SEXP C_charport_class_of(SEXP x) {
  if(TYPEOF(x) != STRSXP) {
    Rf_error("charport_class_of: x must be a character vector");
  }
  for(const altrep_entry & entry : altrep_registry()) {
    if(R_altrep_inherits(x, entry.cls) == TRUE) {
#if (R_VERSION >= R_Version(4, 6, 0))
      const char * cls_name = CHAR(Rf_asChar(R_altrep_class_name(x)));
      const char * pkg_name = CHAR(Rf_asChar(R_altrep_class_package(x)));
      char buf[256];
      std::snprintf(buf, sizeof(buf), "%s::%s", pkg_name, cls_name);
      return Rf_ScalarString(Rf_mkCharCE(buf, CE_UTF8));
#else
      return Rf_mkString("<registered ALTREP class>");
#endif
    }
  }
  return Rf_ScalarString(NA_STRING);
}

extern "C" SEXP C_unregister_charvec(void) {
  charport_unregister_altrep(charvec_altrep::class_t);
  return R_NilValue;
}

extern "C" SEXP C_register_charvec(void) {
  charport_register_altrep(charvec_altrep::class_t,
                           charvec_altrep::reader_init,
                           charvec_altrep::reader_get,
                           true,
                           true);
  return R_NilValue;
}

extern "C" void charport_registry_init(DllInfo * dll) {
  R_RegisterCCallable("charport", "charport_register_altrep",
                      reinterpret_cast<DL_FUNC>(&charport_register_altrep));
  R_RegisterCCallable("charport", "charport_unregister_altrep",
                      reinterpret_cast<DL_FUNC>(&charport_unregister_altrep));
  R_RegisterCCallable("charport", "charport_resolve",
                      reinterpret_cast<DL_FUNC>(&charport_resolve));
  R_RegisterCCallable("charport", "charport_abi_version",
                      reinterpret_cast<DL_FUNC>(&charport_abi_version));
  R_RegisterCCallable("charport", "charport_charvec_wrap",
                      reinterpret_cast<DL_FUNC>(&charport_charvec_wrap));
  (void)dll;
}
