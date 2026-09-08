#define RCPP_MASK_RF_ERROR
#include <Rcpp.h>
#include "charport.h"

namespace {

int cleanup_count = 0;

struct cleanup_probe {
  ~cleanup_probe() noexcept { ++cleanup_count; }
};

} // namespace

extern "C" SEXP C_rcpp_native_cleanup_count(void) {
  return Rf_ScalarInteger(cleanup_count);
}

extern "C" SEXP C_rcpp_native_cleanup_run(SEXP expression, SEXP environment) {
  BEGIN_RCPP
  cleanup_probe probe;
  return charport::detail::call_with_rcpp(
    [&]() -> SEXP { return Rf_eval(expression, environment); }
  );
  END_RCPP
}
