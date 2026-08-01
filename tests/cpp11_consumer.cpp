#include <cpp11.hpp>
#include "charport.h"
#include <cpp11/declarations.hpp>

#include <utility>

namespace {

int cleanup_count = 0;

struct cleanup_probe {
  ~cleanup_probe() noexcept { ++cleanup_count; }
};

} // namespace

extern "C" SEXP C_cpp11_charport_cleanup_count(void) {
  return Rf_ScalarInteger(cleanup_count);
}

extern "C" SEXP C_cpp11_charport_test(SEXP x) {
  BEGIN_CPP11
  cleanup_probe probe;
  charport::Reader reader = charport::Reader::with_cpp11(x);
  charport::charvec::Builder output(reader.size());
  for(R_xlen_t i = 0; i < reader.size(); ++i) {
    output.set(i, reader[i]);
  }
  return output.to_sexp_with_cpp11();
  END_CPP11
}

extern "C" SEXP C_cpp11_charport_wrap_test(void) {
  BEGIN_CPP11
  charport::charvec::Store store = charport::charvec::Store::scalar(
    "wrapped", 7, CETYPE_EXT_ASCII);
  return charport::charvec::wrap_with_cpp11(std::move(store));
  END_CPP11
}
