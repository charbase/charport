#define RCPP_MASK_RF_ERROR
#include "charport/rcpp.h"

namespace {

int cleanup_count = 0;

struct cleanup_probe {
  ~cleanup_probe() noexcept { ++cleanup_count; }
};

} // namespace

extern "C" SEXP C_rcpp_charport_cleanup_count(void) {
  return Rf_ScalarInteger(cleanup_count);
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
