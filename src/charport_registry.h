#ifndef CHARPORT_REGISTRY_H
#define CHARPORT_REGISTRY_H

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

SEXP C_as_charvec(SEXP x);
SEXP C_charvec_alloc(SEXP n);
SEXP C_is_charvec(SEXP x);
SEXP C_charport_materialize(SEXP x);
SEXP C_charvec_stats(SEXP x);
SEXP C_charvec_assign(SEXP x, SEXP i, SEXP value);
SEXP C_charvec_compact(SEXP x);

SEXP C_charport_backends(void);
SEXP C_charport_backend_of(SEXP x);
SEXP C_unregister_charvec_backend(void);
SEXP C_register_charvec_backend(void);

void charport_registry_init(DllInfo * dll);

} // extern "C"

#endif
