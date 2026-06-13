// The broker: backend registry + charport_resolve + the direct (CHARSXP)
// accessor. Lookup is a linear scan calling R_altrep_inherits per entry --
// that is a deliberate choice, not a placeholder. R_altrep_inherits is
// `ALTREP(x) && ALTREP_CLASS(x) == R_SEXP(cls)` (two loads and a pointer
// compare), there is no public accessor that extracts ALTREP_CLASS(x) as a
// hash key, and the registry holds one entry per backend *package* loaded
// in the session -- single digits. The scan runs once per vector, not per
// element.

#include "charvec_altrep.h"
#include "charport_registry.h"

#include <cstdio>
#include <vector>

#include <Rversion.h>

namespace {

struct backend_entry {
  R_altrep_class_t cls;
  charport_init_fn init;
  charport_get_strview_fn get;
  bool reentrant;
};

// Registration and resolve are main-R-thread-only (both take SEXPs), so the
// registry is unlocked. The stored class objects cannot be GC'd: R
// preserves every ALTREP class in its own serialization registry.
std::vector<backend_entry> & backend_registry() {
  static std::vector<backend_entry> reg;
  return reg;
}

// The broker's accessor for CHARSXP-backed storage: state is the
// STRING_PTR_RO array. Plain reads of immutable materialized CHARSXPs
// (R_CHAR / getCharCE / charIsASCII flag bits), so the direct path is
// always reentrant. Unlike registered backends, it can surface CE_NATIVE /
// CE_LATIN1 marks from old CHARSXPs; consumers that require UTF-8
// translate those elements themselves.
charport_strview charport_direct_get(void * state, R_xlen_t i) {
  const SEXP cs = static_cast<const SEXP *>(state)[i];
  if(cs == NA_STRING) {
    return make_strview(nullptr, 0, charport_enc::CE_NA);
  }
  charport_strview out;
  out.ptr = CHAR(cs);
  out.len = static_cast<uint32_t>(Rf_xlength(cs));  // CHARSXP length <= INT_MAX
  out.enc = cpi::classify_charsxp(cs);
  return out;
}

void register_charvec_backend() {
  charport_register_backend(charvec_altrep::class_t,
                            charvec_altrep::reader_init,
                            charvec_altrep::reader_get,
                            true);
}

} // namespace

extern "C" {

void charport_register_backend(R_altrep_class_t cls,
                               charport_init_fn init,
                               charport_get_strview_fn get_strview,
                               bool reentrant) {
  if(R_SEXP(cls) == NULL || init == NULL || get_strview == NULL) {
    Rf_error("charport_register_backend: cls, init, and get_strview must be non-NULL");
  }
  for(backend_entry & e : backend_registry()) {
    if(R_SEXP(e.cls) == R_SEXP(cls)) {  // re-registration replaces
      e.init = init;
      e.get = get_strview;
      e.reentrant = reentrant;
      return;
    }
  }
  try {
    backend_registry().push_back(backend_entry{cls, init, get_strview, reentrant});
  } catch(const std::exception & e) {
    Rf_error("charport_register_backend: %s", e.what());
  }
}

void charport_unregister_backend(R_altrep_class_t cls) {
  std::vector<backend_entry> & reg = backend_registry();
  for(auto it = reg.begin(); it != reg.end(); ++it) {
    if(R_SEXP(it->cls) == R_SEXP(cls)) {
      reg.erase(it);
      return;
    }
  }
}

charport_reader charport_resolve(SEXP x) {
  if(TYPEOF(x) != STRSXP) {
    Rf_error("charport_resolve: x must be a character vector");
  }
  charport_reader r;
  r.n = Rf_xlength(x);
  for(const backend_entry & e : backend_registry()) {
    if(R_altrep_inherits(x, e.cls) == TRUE) {
      if(void * state = e.init(x)) {
        r.state = state;
        r.get = e.get;
        r.reentrant = e.reentrant;
        return r;
      }
      break;  // class matched but cannot serve this instance: go direct
    }
  }
  // plain vectors are already CHARSXP storage; unregistered ALTREP pays a
  // one-time materialization here (the status-quo cost, once, at resolve)
  r.state = const_cast<void *>(static_cast<const void *>(STRING_PTR_RO(x)));
  r.get = charport_direct_get;
  r.reentrant = true;
  return r;
}

int charport_abi_version(void) {
  return CHARPORT_ABI_VERSION;
}

// ---- R-visible diagnostics + test hooks ---------------------------------

// Names require an *instance* (R_altrep_class_name takes SEXP x, not the
// class object), so the registry can only report counts and capability
// flags; see charport_backend_of for names.
SEXP C_charport_backends(void) {
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

// Which registered backend claims x's class -- a class-membership question,
// answered without calling init (no side effects, never materializes).
// "package::class" on R >= 4.6.0, a placeholder string before that;
// NA_character_ when no registered backend matches.
SEXP C_charport_backend_of(SEXP x) {
  if(TYPEOF(x) != STRSXP) {
    Rf_error("charport_backend_of: x must be a character vector");
  }
  for(const backend_entry & e : backend_registry()) {
    if(R_altrep_inherits(x, e.cls) == TRUE) {
#if (R_VERSION >= R_Version(4, 6, 0))
      // class name/package are symbols; Rf_asChar dodges PRINTNAME's
      // unsettled API status
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

// Equivalence-test hook: loop the resolved reader and rebuild a plain
// character vector from the strviews. Must be value- and mark-identical to
// STRING_ELT + CHAR over the same input.
SEXP C_charport_reader_read_all(SEXP x) {
  return charport_sexp_guard("reader_read_all", [&]() -> SEXP {
    const charport_reader r = charport_resolve(x);
    SEXP out = PROTECT(Rf_allocVector(STRSXP, r.n));
    for(R_xlen_t i = 0; i < r.n; ++i) {
      SET_STRING_ELT(out, i, cpi::make_charsxp(r.get(r.state, i)));
    }
    UNPROTECT(1);
    return out;
  });
}

SEXP C_charport_reader_info(SEXP x) {
  const charport_reader r = charport_resolve(x);
  const char * names[] = {"n", "reentrant", "path", ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SET_VECTOR_ELT(out, 0, Rf_ScalarReal(static_cast<double>(r.n)));
  SET_VECTOR_ELT(out, 1, Rf_ScalarLogical(r.reentrant ? TRUE : FALSE));
  SET_VECTOR_ELT(out, 2, Rf_mkString(r.get == charport_direct_get ? "direct" : "backend"));
  UNPROTECT(1);
  return out;
}

SEXP C_charport_test_unregister_charvec(void) {
  charport_unregister_backend(charvec_altrep::class_t);
  return R_NilValue;
}

SEXP C_charport_test_register_charvec(void) {
  register_charvec_backend();
  return R_NilValue;
}

// Smoke-test that the R_GetCCallable route hands back these exact entry
// points (tests/threaded_consumer.cpp exercises the same route from a
// genuinely separate DSO).
SEXP C_charport_ccallable_check(void) {
  const bool ok =
    R_GetCCallable("charport", "charport_register_backend")
      == reinterpret_cast<DL_FUNC>(&charport_register_backend) &&
    R_GetCCallable("charport", "charport_unregister_backend")
      == reinterpret_cast<DL_FUNC>(&charport_unregister_backend) &&
    R_GetCCallable("charport", "charport_resolve")
      == reinterpret_cast<DL_FUNC>(&charport_resolve) &&
    R_GetCCallable("charport", "charport_abi_version")
      == reinterpret_cast<DL_FUNC>(&charport_abi_version) &&
    R_GetCCallable("charport", "charport_charvec_wrap")
      == reinterpret_cast<DL_FUNC>(&charport_charvec_wrap) &&
    charport_abi_version() == CHARPORT_ABI_VERSION;
  return Rf_ScalarLogical(ok ? TRUE : FALSE);
}

void charport_registry_init(DllInfo * dll) {
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
  register_charvec_backend();
}

} // extern "C"
