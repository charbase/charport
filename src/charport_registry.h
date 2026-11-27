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
int charport_native_is_utf8_impl(void);
SEXP charport_charvec_wrap(void * store);
SEXP charport_charvec_from_views_impl(
    R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings);
SEXP charport_charvec_placeholder_impl(void);
void charport_charvec_fill_store_impl(SEXP x, void * source);
void * charport_charvec_get_store_impl(SEXP x);

SEXP charport_charpack_placeholder_impl(void);
void charport_charpack_fill_views_impl(
    SEXP x, R_xlen_t n, const char * const * ptrs, const int * lengths,
    const cetype_ext_t * encodings);
void charport_charpack_fill_slices_impl(
    SEXP x, R_xlen_t n, const int * lengths, const cetype_ext_t * encodings,
    R_xlen_t n_slices, const char * const * slices, const size_t * slice_bytes);
SEXP charport_charpack_fill_materialized_na_impl(SEXP x, R_xlen_t n);
int charport_is_charpack_impl(SEXP x);

SEXP C_as_charvec(SEXP x);
SEXP C_as_charpack(SEXP x);
void charport_register_charvec_impl(void);
void charport_register_charpack_impl(void);
SEXP C_is_charvec(SEXP x);
SEXP C_charport_materialize(SEXP x);

SEXP C_charport_classes(void);
SEXP C_charport_info(SEXP x);

SEXP C_is_charpack(SEXP x);

void charport_registry_init(DllInfo * dll);

} // extern "C"

#endif
