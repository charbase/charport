#ifndef CHARPORT_REGISTRY_H
#define CHARPORT_REGISTRY_H

// Broker internals: the C ABI entry points defined in charport_registry.cpp
// (consumers reach them via R_GetCCallable; only this package links them
// directly) plus the R-visible .Call hooks they back.

#include "../inst/include/charport.h"

extern "C" {

void charport_register_backend(R_altrep_class_t cls,
                               charport_init_fn init,
                               charport_get_strview_fn get_strview,
                               bool reentrant);
void charport_unregister_backend(R_altrep_class_t cls);
charport_reader charport_resolve(SEXP x);
int charport_abi_version(void);

SEXP charport_charvec_wrap(void * store);

SEXP C_charport_backends(void);
SEXP C_charport_backend_of(SEXP x);
SEXP C_charport_reader_read_all(SEXP x);
SEXP C_charport_reader_info(SEXP x);
SEXP C_charport_test_unregister_charvec(void);
SEXP C_charport_test_register_charvec(void);
SEXP C_charport_ccallable_check(void);

// cp:: wrapper exercises (charport consuming itself through R_GetCCallable;
// defined in charport_cp_test.cpp)
SEXP C_cp_reader_roundtrip(SEXP x);
SEXP C_cp_builder_from_reader(SEXP x, SEXP n_shards_);
SEXP C_cp_builder_reserve(SEXP x, SEXP n_shards_);
SEXP C_cp_builder_errors(void);

// R_RegisterCCallable for the four ABI symbols + charvec's own backend
// registration (dogfooding the public path). Called from R_init_charport
// after charvec_altrep::Init.
void charport_registry_init(DllInfo * dll);

} // extern "C"

#endif
