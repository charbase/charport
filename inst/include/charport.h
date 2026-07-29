#ifndef CHARPORT_H
#define CHARPORT_H

// Public header for charport consumers. Include a C++ framework first, when used.

#ifdef __cplusplus
#if defined(_MSVC_LANG)
#  if _MSVC_LANG < 201103L
#    error "charport requires C++11 or later"
#  endif
#elif __cplusplus < 201103L
#  error "charport requires C++11 or later"
#endif

// cpp11 uses #pragma once rather than a presence macro. These markers are
// defined by its headers; CPP11_USE_FMT and CPP11_PARTIAL are caller inputs.
#if defined(CPP11_PRIdXLEN_T) || defined(CPP11_UNWIND) || \
    defined(CPP11_ERROR_BUFSIZE) || defined(BEGIN_CPP11) || \
    defined(END_CPP11)
#define CHARPORT_CPP11_INCLUDED 1
#endif

#include "charport/api.h"
#undef CHARPORT_CPP11_INCLUDED
#else
#include "charport/interop/reader.h"
#endif

#endif
