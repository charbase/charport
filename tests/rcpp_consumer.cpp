#define RCPP_MASK_RF_ERROR
#include <Rcpp.h>
#include "charport.h"

#include <utility>

namespace {

int cleanup_count = 0;

struct cleanup_probe {
  ~cleanup_probe() noexcept { ++cleanup_count; }
};

} // namespace

extern "C" SEXP C_rcpp_charport_cleanup_count(void) {
  return Rf_ScalarInteger(cleanup_count);
}

extern "C" SEXP C_rcpp_charport_test(SEXP x) {
  BEGIN_RCPP
  cleanup_probe probe;
  charport::Reader reader = charport::Reader::with_rcpp(x);
  charport::charvec::Builder output(reader.size());
  for(R_xlen_t i = 0; i < reader.size(); ++i) {
    output.set(i, reader[i]);
  }
  return output.to_sexp_with_rcpp();
  END_RCPP
}

extern "C" SEXP C_rcpp_charport_wrap_test(void) {
  BEGIN_RCPP
  charport::charvec::Store store = charport::charvec::Store::scalar(
    "wrapped", 7, CETYPE_EXT_ASCII);
  return charport::charvec::wrap_with_rcpp(std::move(store));
  END_RCPP
}
