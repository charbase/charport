#include "charvec_altrep.h"
#include "charport_registry.h"

R_altrep_class_t charvec_altrep::class_t;

static const R_CallMethodDef call_entries[] = {
  {"C_as_charvec",                  (DL_FUNC) &C_as_charvec,                  1},
  {"C_charvec_alloc",               (DL_FUNC) &C_charvec_alloc,               1},
  {"C_is_charvec",                  (DL_FUNC) &C_is_charvec,                  1},
  {"C_charport_materialize",        (DL_FUNC) &C_charport_materialize,        1},
  {"C_charvec_stats",               (DL_FUNC) &C_charvec_stats,               1},
  {"C_charvec_assign",              (DL_FUNC) &C_charvec_assign,              3},
  {"C_charvec_compact",             (DL_FUNC) &C_charvec_compact,             1},
  {"C_charport_backends",           (DL_FUNC) &C_charport_backends,           0},
  {"C_charport_backend_of",         (DL_FUNC) &C_charport_backend_of,         1},
  {"C_unregister_charvec_backend",  (DL_FUNC) &C_unregister_charvec_backend,  0},
  {"C_register_charvec_backend",    (DL_FUNC) &C_register_charvec_backend,    0},
  {NULL, NULL, 0}
};

extern "C" void R_init_charport(DllInfo * dll) {
  R_registerRoutines(dll, NULL, call_entries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  R_forceSymbols(dll, TRUE);
  charvec_altrep::Init(dll);
  charport_registry_init(dll);
}
