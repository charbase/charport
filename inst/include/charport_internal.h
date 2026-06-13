#ifndef CHARPORT_INTERNAL_H
#define CHARPORT_INTERNAL_H

// Internal implementation headers for the charport package. These are NOT
// the public interop API: the consumer surface is charport.h
// (charport_resolve, charport_reader, the cp:: wrappers), and that is the
// only thing other packages should depend on directly. Layouts and names in
// here may change without notice.

#include "charport_internal/base.h"
#include "charport_internal/charvec_store.h"
#include "charport_internal/r_interop.h"

#endif
