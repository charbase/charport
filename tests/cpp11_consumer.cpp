#include <cpp11.hpp>
#include "charport.h"
#include <cpp11/declarations.hpp>

static_assert(
  std::is_same<
    charport::detail::selected_backend,
    charport::unwind_detail::cpp11_backend
  >::value,
  "charport.h did not select the cpp11 adapter"
);

namespace {

int cleanup_count = 0;

struct cleanup_probe {
  ~cleanup_probe() noexcept { ++cleanup_count; }
};

} // namespace

extern "C" SEXP C_cpp11_charport_cleanup_count(void) {
  return Rf_ScalarInteger(cleanup_count);
}

// A hand-written .Call boundary in the same TU: r_boundary must recognize the
// cpp11 unwind exception thrown by the selected backend and continue the
// original R condition.
extern "C" SEXP C_cpp11_charport_boundary(SEXP x, SEXP expression, SEXP environment) {
  return charport::r_boundary("cpp11_boundary", [&]() -> SEXP {
    cleanup_probe probe;
    charport::Reader reader(x);
    return charport::unwind_protect([&]() -> SEXP {
      return Rf_eval(expression, environment);
    });
  });
}

extern "C" SEXP C_cpp11_charport_test(SEXP x, SEXP expression, SEXP environment) {
  BEGIN_CPP11
  cleanup_probe probe;
  if(environment == R_NilValue) {
    return charport::unwind_protect([]() -> SEXP {
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
  END_CPP11
}
