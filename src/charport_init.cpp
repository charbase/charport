#include "charvec_altrep.h"
#include "charpack_altrep.h"
#include "charport_registry.h"

R_altrep_class_t charvec_altrep::class_t;
R_altrep_class_t charpack_altrep::class_t;

static const R_CallMethodDef call_entries[] = {
  {"C_as_charvec",                  (DL_FUNC) &C_as_charvec,                  1},
  {"C_charvec_alloc",               (DL_FUNC) &C_charvec_alloc,               1},
  {"C_is_charvec",                  (DL_FUNC) &C_is_charvec,                  1},
  {"C_charport_materialize",        (DL_FUNC) &C_charport_materialize,        1},
  {"C_charvec_stats",               (DL_FUNC) &C_charvec_stats,               1},
  {"C_charvec_assign",              (DL_FUNC) &C_charvec_assign,              3},
  {"C_charvec_compact",             (DL_FUNC) &C_charvec_compact,             1},
  {"C_charport_classes",            (DL_FUNC) &C_charport_classes,            0},
  {"C_charport_info",               (DL_FUNC) &C_charport_info,               1},
  {"C_unregister_charvec",          (DL_FUNC) &C_unregister_charvec,          0},
  {"C_register_charvec",            (DL_FUNC) &C_register_charvec,            0},
  {"C_charpack_placeholder",        (DL_FUNC) &C_charpack_placeholder,        0},
  {"C_charpack_fill_from",          (DL_FUNC) &C_charpack_fill_from,          4},
  {"C_charpack_fill_materialized_na", (DL_FUNC) &C_charpack_fill_materialized_na, 2},
  {"C_is_charpack",                 (DL_FUNC) &C_is_charpack,                 1},
  {"C_charpack_state",              (DL_FUNC) &C_charpack_state,              1},
  {"C_charpack_data1",              (DL_FUNC) &C_charpack_data1,              1},
  {"C_charpack_serialized_state",   (DL_FUNC) &C_charpack_serialized_state,   1},
  {"C_charpack_from_state",         (DL_FUNC) &C_charpack_from_state,         1},
  {"C_unregister_charpack",         (DL_FUNC) &C_unregister_charpack,         0},
  {"C_register_charpack",           (DL_FUNC) &C_register_charpack,           0},
  {NULL, NULL, 0}
};

extern "C" void R_init_charport(DllInfo * dll) {
  R_registerRoutines(dll, NULL, call_entries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  R_forceSymbols(dll, TRUE);
  charvec_altrep::Init(dll);
  charpack_altrep::Init(dll);
  charport_registry_init(dll);
  // Lookup follows registration order; charpack comes first because it is
  // meant for many short vectors, where per-vector resolve cost matters most.
  (void)C_register_charpack();
  (void)C_register_charvec();
}
