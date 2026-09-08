#define R_NO_REMAP
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <R_ext/Rdynload.h>

#include <cstddef>
#include <cstring>

// The probe includes the package source so its R allocation sites can be
// replaced locally.  It is compiled only by tools/test-native-cleanup.R and is not
// part of the installed package or its public API.
extern "C" SEXP native_cleanup_make_external_ptr(void *, SEXP, SEXP);
extern "C" void native_cleanup_register_finalizer(SEXP, R_CFinalizer_t, Rboolean);
extern "C" SEXP native_cleanup_new_altrep(R_altrep_class_t, SEXP, SEXP);
extern "C" DL_FUNC native_cleanup_get_ccallable(const char *, const char *);

#define R_MakeExternalPtr native_cleanup_make_external_ptr
#define R_RegisterCFinalizerEx native_cleanup_register_finalizer
#define R_new_altrep native_cleanup_new_altrep
#define R_GetCCallable native_cleanup_get_ccallable
#include "charvec_api.cpp"
#undef R_MakeExternalPtr
#undef R_RegisterCFinalizerEx
#undef R_new_altrep
#undef R_GetCCallable

static_assert(sizeof(std::size_t) == 8,
              "the allocation wrappers require 64-bit size_t");

R_altrep_class_t charvec_altrep::class_t;

namespace {

enum failure_point {
  failure_none = 0,
  failure_external_ptr = 1,
  failure_finalizer = 2,
  failure_altrep = 3
};

int failure = failure_none;
bool tracking = false;
void * live[4096] = {};
int outstanding = 0;

void track(void * ptr) noexcept {
  if(!tracking || ptr == nullptr) {
    return;
  }
  for(void *& slot : live) {
    if(slot == nullptr) {
      slot = ptr;
      ++outstanding;
      return;
    }
  }
  Rf_error("native cleanup probe: allocation table is full");
}

void untrack(void * ptr) noexcept {
  if(ptr == nullptr) {
    return;
  }
  for(void *& slot : live) {
    if(slot == ptr) {
      slot = nullptr;
      --outstanding;
      return;
    }
  }
}

R_altrep_class_t error_index_altrep_class;

R_xlen_t error_index_length(SEXP) {
  return 1;
}

void * error_index_dataptr(SEXP, Rboolean) {
  Rf_error("native cleanup probe: injected index data-pointer error");
}

} // namespace

extern "C" void * __real__Znwm(std::size_t);
extern "C" void * __real__Znam(std::size_t);
extern "C" void __real__ZdlPv(void *);
extern "C" void __real__ZdaPv(void *);
extern "C" void __real__ZdlPvm(void *, std::size_t);
extern "C" void __real__ZdaPvm(void *, std::size_t);

extern "C" void * __wrap__Znwm(std::size_t size) {
  void * ptr = __real__Znwm(size);
  track(ptr);
  return ptr;
}

extern "C" void * __wrap__Znam(std::size_t size) {
  void * ptr = __real__Znam(size);
  track(ptr);
  return ptr;
}

extern "C" void __wrap__ZdlPv(void * ptr) {
  untrack(ptr);
  __real__ZdlPv(ptr);
}

extern "C" void __wrap__ZdaPv(void * ptr) {
  untrack(ptr);
  __real__ZdaPv(ptr);
}

extern "C" void __wrap__ZdlPvm(void * ptr, std::size_t size) {
  untrack(ptr);
  __real__ZdlPvm(ptr, size);
}

extern "C" void __wrap__ZdaPvm(void * ptr, std::size_t size) {
  untrack(ptr);
  __real__ZdaPvm(ptr, size);
}

extern "C" SEXP native_cleanup_make_external_ptr(
    void * ptr, SEXP tag, SEXP prot) {
  if(failure == failure_external_ptr) {
    Rf_error("native cleanup probe: injected external-pointer error");
  }
  return R_MakeExternalPtr(ptr, tag, prot);
}

extern "C" void native_cleanup_register_finalizer(
    SEXP xp, R_CFinalizer_t finalizer, Rboolean onexit) {
  if(failure == failure_finalizer) {
    Rf_error("native cleanup probe: injected finalizer-registration error");
  }
  R_RegisterCFinalizerEx(xp, finalizer, onexit);
}

extern "C" SEXP native_cleanup_new_altrep(
    R_altrep_class_t class_, SEXP data1, SEXP data2) {
  if(failure == failure_altrep) {
    Rf_error("native cleanup probe: injected ALTREP-shell error");
  }
  return R_new_altrep(class_, data1, data2);
}

extern "C" DL_FUNC native_cleanup_get_ccallable(
    const char * package, const char * name) {
  if(std::strcmp(package, "charport") == 0 &&
     std::strcmp(name, "charport_charvec_wrap") == 0) {
    return reinterpret_cast<DL_FUNC>(&charport_charvec_wrap);
  }
  return R_GetCCallable(package, name);
}

extern "C" void R_init_native_cleanup_probe(DllInfo * dll) {
  charvec_altrep::Init(dll);
  error_index_altrep_class = R_make_altinteger_class(
    "native_cleanup_error_index", "native_cleanup_probe", dll);
  R_set_altrep_Length_method(error_index_altrep_class, error_index_length);
  R_set_altvec_Dataptr_method(error_index_altrep_class, error_index_dataptr);
}

extern "C" SEXP native_cleanup_set_failure(SEXP point) {
  failure = Rf_asInteger(point);
  tracking = true;
  return R_NilValue;
}

extern "C" SEXP native_cleanup_stop_tracking(void) {
  tracking = false;
  return R_NilValue;
}

extern "C" SEXP native_cleanup_count(void) {
  tracking = false;
  return Rf_ScalarInteger(outstanding);
}

extern "C" SEXP native_cleanup_alloc(SEXP n) {
  return C_charvec_alloc(n);
}

extern "C" SEXP native_cleanup_as(SEXP x) {
  return C_as_charvec(x);
}

extern "C" SEXP native_cleanup_bulk(void) {
  static const char alpha[] = "alpha";
  static const char beta[] = "beta";
  const char * ptrs[] = {alpha, beta, nullptr};
  const int lengths[] = {5, 4, NA_INTEGER};
  const cetype_ext_t encodings[] = {
    CETYPE_EXT_ASCII, CETYPE_EXT_ASCII, CETYPE_EXT_NA
  };
  return charport_charvec_from_views_impl(3, ptrs, lengths, encodings);
}

extern "C" SEXP native_cleanup_duplicate(SEXP x) {
  return charvec_altrep::Duplicate(x, FALSE);
}

extern "C" SEXP native_cleanup_subset(SEXP x, SEXP index) {
  return charvec_altrep::Extract_subset(x, index, R_NilValue);
}

extern "C" SEXP native_cleanup_serialize(SEXP x) {
  return charvec_altrep::Serialized_state(x);
}

extern "C" SEXP native_cleanup_unserialize(SEXP state) {
  return charvec_altrep::Unserialize(R_NilValue, state);
}

extern "C" SEXP native_cleanup_error_index(void) {
  return R_new_altrep(error_index_altrep_class, R_NilValue, R_NilValue);
}
