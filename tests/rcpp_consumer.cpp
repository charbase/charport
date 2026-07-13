#define RCPP_MASK_RF_ERROR
#include <Rcpp.h>
#include "charport.h"

static_assert(
  std::is_same<
    charport::detail::selected_backend,
    charport::unwind_detail::rcpp_backend
  >::value,
  "charport.h did not select the Rcpp adapter"
);

namespace {

int cleanup_count = 0;

struct cleanup_probe {
  ~cleanup_probe() noexcept { ++cleanup_count; }
};

} // namespace

extern "C" SEXP C_rcpp_charport_cleanup_count(void) {
  return Rf_ScalarInteger(cleanup_count);
}

// A hand-written .Call boundary in the same TU: r_boundary must recognize the
// Rcpp unwind exception thrown by the selected backend and continue the
// original R condition.
extern "C" SEXP C_rcpp_charport_boundary(SEXP x, SEXP expression, SEXP environment) {
  return charport::r_boundary("rcpp_boundary", [&]() -> SEXP {
    cleanup_probe probe;
    charport::Reader reader(x);
    return charport::unwind_protect([&]() -> SEXP {
      return Rf_eval(expression, environment);
    });
  });
}

extern "C" SEXP C_rcpp_charport_test(SEXP x, SEXP expression, SEXP environment) {
  BEGIN_RCPP
  cleanup_probe probe;
  if(environment == R_NilValue) {
    return charport::charvec::builder_detail::wrap_store<
      charport::detail::selected_backend
    >([]() -> std::unique_ptr<charport::charvec::Store> {
      throw std::runtime_error("injected builder C++ error");
    });
  }

  charport::Reader reader(x);
  if(expression != R_NilValue) {
    return charport::unwind_protect([&]() -> SEXP {
      return Rf_eval(expression, environment);
    });
  }

  charport::charvec::Builder output(reader.size());
  for(R_xlen_t i = 0; i < reader.size(); ++i) {
    output.set(i, reader[i]);
  }
  return output.to_sexp();
  END_RCPP
}
