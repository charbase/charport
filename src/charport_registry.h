#ifndef CHARPORT_REGISTRY_H
#define CHARPORT_REGISTRY_H

#include "../inst/include/charport.h"

extern "C" {

void charport_register_altrep(R_altrep_class_t cls,
                              charport_reader_state_fns state_fns,
                              charport_reader_range_fns range_fns,
                              charport_reader_index_fns index_fns,
                              charport_reader_capabilities capabilities);
void charport_unregister_altrep(R_altrep_class_t cls);
charport_reader charport_resolve(SEXP x);
charport_sexp_info charport_get_sexp_info(SEXP x);
int charport_abi_version(void);
SEXP charport_charvec_wrap(void * store);
SEXP charport_charvec_from_views_impl(
    R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings);

SEXP C_as_charvec(SEXP x);
SEXP C_charvec_alloc(SEXP n);
SEXP C_is_charvec(SEXP x);
SEXP C_charport_materialize(SEXP x);
SEXP C_charvec_stats(SEXP x);
SEXP C_charvec_assign(SEXP x, SEXP i, SEXP value);
SEXP C_charvec_compact(SEXP x);

SEXP C_charport_classes(void);
SEXP C_charport_info(SEXP x);
SEXP C_unregister_charvec(void);
SEXP C_register_charvec(void);

void charport_registry_init(DllInfo * dll);

} // extern "C"

#endif
