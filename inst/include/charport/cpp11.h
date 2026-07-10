#ifndef CHARPORT_CPP11_H
#define CHARPORT_CPP11_H

// Umbrella header for functions entered through a cpp11 generated wrapper.

#include <cpp11.hpp>
#include "interop/unwind.h"

namespace charport {
namespace unwind_detail {

struct cpp11_backend {
  template<typename Fn>
  static SEXP call(Fn && fn) {
    typedef typename std::remove_reference<Fn>::type fun_type;
    call_state<fun_type> state{&fn, std::exception_ptr()};
    SEXP out = cpp11::unwind_protect([&]() -> SEXP {
      return call_body<fun_type>(&state);
    });
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
#define CHARPORT_API_BACKEND 3
#define CHARPORT_UNWIND_BACKEND ::charport::unwind_detail::cpp11_backend
#include "api.h"
#undef CHARPORT_UNWIND_BACKEND

#endif
