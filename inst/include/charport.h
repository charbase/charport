#ifndef CHARPORT_H
#define CHARPORT_H

// Umbrella header for hand-written .Call consumers.

#ifdef __cplusplus
#ifdef CHARPORT_API_BACKEND
#error "include only one charport umbrella header"
#endif
#define CHARPORT_API_BACKEND 1
#define CHARPORT_UNWIND_BACKEND ::charport::unwind_detail::standalone_backend
#include "charport/api.h"
#undef CHARPORT_UNWIND_BACKEND
#else
#include "charport/interop/reader.h"
#endif

#endif
