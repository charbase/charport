#ifndef CHARPORT_H
#define CHARPORT_H

// charport -- interconnect for ALTREP character vector backends.
//
// Umbrella header: it only aggregates the component headers. Include this and
// you get everything the language allows:
//
//   charport/strview.h                 element type (C and C++)
//   charport/base.h                    reader + registration ABI (C and C++),
//                                      cp:: resolve / Reader / check_abi (C++)
//   charport/builder_single_threaded.h cp::charvec::Builder      (C++)
//   charport/builder_multi_threaded.h  cp::charvec::BuilderMT     (C++)
//
// A C consumer (a C compiler defines no __cplusplus) gets the element type and
// the reader/registration ABI -- enough to resolve a vector and loop over it,
// or to register a backend -- and the C++-only builders are skipped. A C++
// consumer additionally gets cp::Reader and the two builders. Pick the right
// builder by threading model: Builder for serial, BuilderMT for parallel.

#include "charport/strview.h"
#include "charport/base.h"

#ifdef __cplusplus
#include "charport/builder_single_threaded.h"
#include "charport/builder_multi_threaded.h"
#endif

#endif
