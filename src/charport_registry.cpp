#include "charvec_altrep.h"
#include "charport_registry.h"

#include <cstdio>
#include <exception>
#include <vector>

#include <Rversion.h>

namespace {

struct backend_entry {
  R_altrep_class_t cls;
  charport_init_fn init;
  charport_get_strview_fn get;
  bool reentrant;
};

std::vector<backend_entry> & backend_registry() {
  static std::vector<backend_entry> reg;
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

charport_reader direct_reader(SEXP x, const void * direct) {
  charport_reader r;
  r.n = Rf_xlength(x);
  r.state = const_cast<void *>(direct);
  r.get = direct_get;
  r.reentrant = false;
  return r;
}

void register_charvec_backend() {
  charport_register_backend(charvec_altrep::class_t,
                            charvec_altrep::reader_init,
                            charvec_altrep::reader_get,
                            true);
}

} // namespace

extern "C" void charport_register_backend(R_altrep_class_t cls,
                                           charport_init_fn init,
                                           charport_get_strview_fn get_strview,
                                           bool reentrant) {
  if(R_SEXP(cls) == NULL || init == NULL || get_strview == NULL) {
    Rf_error("charport_register_backend: cls, init, and get_strview must be non-NULL");
  }
  for(backend_entry & entry : backend_registry()) {
    if(R_SEXP(entry.cls) == R_SEXP(cls)) {
      entry.init = init;
      entry.get = get_strview;
      entry.reentrant = reentrant;
      return;
    }
  }
  try {
    backend_registry().push_back(backend_entry{cls, init, get_strview, reentrant});
  } catch(const std::exception & e) {
    Rf_error("charport_register_backend: %s", e.what());
  }
}

extern "C" void charport_unregister_backend(R_altrep_class_t cls) {
  std::vector<backend_entry> & reg = backend_registry();
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
  for(const backend_entry & entry : backend_registry()) {
    if(R_altrep_inherits(x, entry.cls) == TRUE) {
      if(void * state = entry.init(x)) {
        r.state = state;
        r.get = entry.get;
        r.reentrant = entry.reentrant;
        return r;
      }
      break;
    }
  }
  return direct_reader(x, STRING_PTR_RO(x));
}

extern "C" int charport_abi_version(void) {
  return CHARPORT_ABI_VERSION;
}

extern "C" SEXP C_charport_backends(void) {
  const std::vector<backend_entry> & reg = backend_registry();
  const char * names[] = {"n", "reentrant", ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SET_VECTOR_ELT(out, 0, Rf_ScalarInteger(static_cast<int>(reg.size())));
  SEXP flags = Rf_allocVector(LGLSXP, static_cast<R_xlen_t>(reg.size()));
  SET_VECTOR_ELT(out, 1, flags);
  for(size_t i = 0; i < reg.size(); ++i) {
    LOGICAL(flags)[i] = reg[i].reentrant ? TRUE : FALSE;
  }
  UNPROTECT(1);
  return out;
}

extern "C" SEXP C_charport_backend_of(SEXP x) {
  if(TYPEOF(x) != STRSXP) {
    Rf_error("charport_backend_of: x must be a character vector");
  }
  for(const backend_entry & entry : backend_registry()) {
    if(R_altrep_inherits(x, entry.cls) == TRUE) {
#if (R_VERSION >= R_Version(4, 6, 0))
      const char * cls_name = CHAR(Rf_asChar(R_altrep_class_name(x)));
      const char * pkg_name = CHAR(Rf_asChar(R_altrep_class_package(x)));
      char buf[256];
      std::snprintf(buf, sizeof(buf), "%s::%s", pkg_name, cls_name);
      return Rf_ScalarString(Rf_mkCharCE(buf, CE_UTF8));
#else
      return Rf_mkString("<registered backend>");
#endif
    }
  }
  return Rf_ScalarString(NA_STRING);
}

extern "C" SEXP C_unregister_charvec_backend(void) {
  charport_unregister_backend(charvec_altrep::class_t);
  return R_NilValue;
}

extern "C" SEXP C_register_charvec_backend(void) {
  register_charvec_backend();
  return R_NilValue;
}

extern "C" void charport_registry_init(DllInfo * dll) {
  R_RegisterCCallable("charport", "charport_register_backend",
                      reinterpret_cast<DL_FUNC>(&charport_register_backend));
  R_RegisterCCallable("charport", "charport_unregister_backend",
                      reinterpret_cast<DL_FUNC>(&charport_unregister_backend));
  R_RegisterCCallable("charport", "charport_resolve",
                      reinterpret_cast<DL_FUNC>(&charport_resolve));
  R_RegisterCCallable("charport", "charport_abi_version",
                      reinterpret_cast<DL_FUNC>(&charport_abi_version));
  R_RegisterCCallable("charport", "charport_charvec_wrap",
                      reinterpret_cast<DL_FUNC>(&charport_charvec_wrap));
  (void)dll;
}
