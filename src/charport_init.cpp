#include "charvec_altrep.h"
#include "charpack_altrep.h"
#include "charport_registry.h"

R_altrep_class_t charvec_altrep::class_t;
R_altrep_class_t charpack_altrep::class_t;

static const R_CallMethodDef call_entries[] = {
  {"C_as_charpack",                 (DL_FUNC) &C_as_charpack,                 1},
  {"C_as_charvec",                  (DL_FUNC) &C_as_charvec,                  1},
  {"C_is_charvec",                  (DL_FUNC) &C_is_charvec,                  1},
  {"C_charport_materialize",        (DL_FUNC) &C_charport_materialize,        1},
  {"C_charport_classes",            (DL_FUNC) &C_charport_classes,            0},
  {"C_charport_info",               (DL_FUNC) &C_charport_info,               1},
  {"C_is_charpack",                 (DL_FUNC) &C_is_charpack,                 1},
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
  charport_register_charpack_impl();
  charport_register_charvec_impl();
}
