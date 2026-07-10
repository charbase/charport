#ifndef CHARPORT_RCPP_H
#define CHARPORT_RCPP_H

// Umbrella header for functions entered through an Rcpp generated wrapper.

#include <Rcpp.h>
#include "interop/unwind.h"

namespace charport {
namespace unwind_detail {

struct rcpp_backend {
  template<typename Fn>
  static SEXP call(Fn && fn) {
    typedef typename std::remove_reference<Fn>::type fun_type;
    call_state<fun_type> state{&fn, std::exception_ptr()};
    SEXP out = Rcpp::unwindProtect(&call_body<fun_type>, &state);
    if(state.error) {
      std::rethrow_exception(state.error);
    }
    return out;
  }
};

} // namespace unwind_detail
} // namespace charport

#ifdef CHARPORT_API_BACKEND
#error "include only one charport umbrella header"
#endif
#define CHARPORT_API_BACKEND 2
#define CHARPORT_UNWIND_BACKEND ::charport::unwind_detail::rcpp_backend
#include "api.h"
#undef CHARPORT_UNWIND_BACKEND

#endif
