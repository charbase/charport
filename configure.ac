AC_INIT([charport])
AC_CONFIG_SRCDIR([src/charpack_altrep.h])

: "${R_HOME:=$(R RHOME)}"
CXX=$("${R_HOME}/bin/R" CMD config CXX) || exit 1
CXXFLAGS=$("${R_HOME}/bin/R" CMD config CXXFLAGS) || exit 1
CPPFLAGS=$("${R_HOME}/bin/R" CMD config CPPFLAGS) || exit 1
LDFLAGS=$("${R_HOME}/bin/R" CMD config LDFLAGS) || exit 1
AC_PROG_CXX
AC_LANG([C++])

dnl Keep the pointer alignment assumptions the same as in charpack_altrep.h.
m4_define([CHARPORT_ATOMIC_TEST], [AC_LANG_PROGRAM([[
#include <cstdint>

std::uint64_t load_head(const unsigned char * raw) {
  return __atomic_load_n(reinterpret_cast<const std::uint64_t *>(raw), __ATOMIC_RELAXED);
}

void store_head(unsigned char * raw, std::uint64_t head) {
  __atomic_store_n(reinterpret_cast<std::uint64_t *>(raw), head, __ATOMIC_RELAXED);
}
]], [[
  std::uint64_t head = 0;
  auto * raw = reinterpret_cast<unsigned char *>(&head);
  auto * volatile load = &load_head;
  auto * volatile store = &store_head;
  store(raw, 1);
  return load(raw) != 1;
]])])

ATOMIC_LIBS=
AC_MSG_CHECKING([whether charpack atomic operations link without -latomic])
AC_LINK_IFELSE([CHARPORT_ATOMIC_TEST], [atomic_link=yes], [atomic_link=no])
AC_MSG_RESULT([$atomic_link])
AS_IF([test "$atomic_link" = no], [
  saved_LIBS=$LIBS
  LIBS="$LIBS -latomic"
  AC_MSG_CHECKING([whether charpack atomic operations link with -latomic])
  AC_LINK_IFELSE([CHARPORT_ATOMIC_TEST], [atomic_link=yes], [atomic_link=no])
  AC_MSG_RESULT([$atomic_link])
  LIBS=$saved_LIBS
  AS_IF([test "$atomic_link" = no], [
    AC_MSG_ERROR([cannot link the 64-bit atomic operations used by charpack])
  ])
  ATOMIC_LIBS=-latomic
])

AC_SUBST([ATOMIC_LIBS])
AC_CONFIG_FILES([src/Makevars])
AC_OUTPUT
