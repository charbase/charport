#include <cpp11.hpp>
#include "charport.h"
#include <cpp11/declarations.hpp>

namespace {

int cleanup_count = 0;

struct cleanup_probe {
  ~cleanup_probe() noexcept { ++cleanup_count; }
};

} // namespace

extern "C" SEXP C_cpp11_native_cleanup_count(void) {
  return Rf_ScalarInteger(cleanup_count);
}

extern "C" SEXP C_cpp11_native_cleanup_run(SEXP expression, SEXP environment) {
  BEGIN_CPP11
  cleanup_probe probe;
  return charport::detail::call_with_cpp11(
    [&]() -> SEXP { return Rf_eval(expression, environment); }
  );
  END_CPP11
}
