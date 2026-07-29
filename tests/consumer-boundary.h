#ifndef CHARPORT_TESTS_CONSUMER_BOUNDARY_H
#define CHARPORT_TESTS_CONSUMER_BOUNDARY_H

#include <csetjmp>
#include <cstdio>
#include <exception>
#include <type_traits>
#include <utility>

namespace charport_consumer {
namespace unwind_detail {

struct RUnwind {
  SEXP token;
};

struct JumpBuffer {
  std::jmp_buf value;
};

template<typename Fn>
struct CallState {
  Fn * fn;
  std::exception_ptr error;
};

template<typename Fn>
SEXP call_body(void * data) noexcept {
  CallState<Fn> * state = static_cast<CallState<Fn> *>(data);
  try {
    return (*state->fn)();
  } catch(...) {
    state->error = std::current_exception();
    return R_NilValue;
  }
}

inline void call_cleanup(void * data, Rboolean jump) noexcept {
  if(jump == TRUE) {
    longjmp(static_cast<JumpBuffer *>(data)->value, 1);
  }
}

} // namespace unwind_detail

// Test and benchmark consumers own the bridge for their raw .Call entries.
template<typename Fn>
SEXP unwind_protect(Fn && fn) {
  typedef typename std::remove_reference<Fn>::type fun_type;
  unwind_detail::CallState<fun_type> state{&fn, std::exception_ptr()};
  unwind_detail::JumpBuffer jump;
  SEXP token = PROTECT(R_MakeUnwindCont());

  if(setjmp(jump.value) != 0) {
    R_PreserveObject(token);
    UNPROTECT(1);
    throw unwind_detail::RUnwind{token};
  }

  SEXP out = R_UnwindProtect(
    &unwind_detail::call_body<fun_type>, &state,
    &unwind_detail::call_cleanup, &jump, token
  );

  SETCAR(token, R_NilValue);
  UNPROTECT(1);
  if(state.error) {
    std::rethrow_exception(state.error);
  }
  return out;
}

[[noreturn]] inline void continue_r_unwind(SEXP token) {
  R_ReleaseObject(token);
  R_ContinueUnwind(token);
  Rf_error("charport consumer: failed to continue R error");
}

template<typename Fn>
SEXP boundary(const char * operation, Fn fn) {
  typedef typename std::decay<Fn>::type body_type;
  static_assert(std::is_trivially_destructible<body_type>::value,
                "boundary callback must be trivially destructible");

  SEXP value = R_NilValue;
  SEXP token = R_NilValue;
  bool has_r_error = false;
  bool has_cpp_error = false;
  char message[512];

  try {
    value = fn();
  } catch(const unwind_detail::RUnwind & error) {
    token = error.token;
    has_r_error = true;
  } catch(const std::exception & error) {
    std::snprintf(message, sizeof(message), "%s", error.what());
    has_cpp_error = true;
  } catch(...) {
    std::snprintf(message, sizeof(message), "unknown C++ exception");
    has_cpp_error = true;
  }

  if(has_r_error) {
    continue_r_unwind(token);
  }
  if(has_cpp_error) {
    Rf_error("%s: %s", operation, message);
  }
  return value;
}

} // namespace charport_consumer

#endif
