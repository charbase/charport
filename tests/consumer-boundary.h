#ifndef CHARPORT_TESTS_CONSUMER_BOUNDARY_H
#define CHARPORT_TESTS_CONSUMER_BOUNDARY_H

#include <csetjmp>
#include <cstdio>
#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace charport_consumer {
namespace unwind_detail {

struct RUnwind {
  SEXP token;
};

// Flatten the active exception into a fixed buffer, naming its type.
inline void describe_current_exception(char * out, size_t size) noexcept {
  try {
    throw;
  } catch(const std::bad_alloc & error) {
    std::snprintf(out, size, "std::bad_alloc: %s", error.what());
  } catch(const std::out_of_range & error) {
    std::snprintf(out, size, "std::out_of_range: %s", error.what());
  } catch(const std::logic_error & error) {
    std::snprintf(out, size, "std::logic_error: %s", error.what());
  } catch(const std::runtime_error & error) {
    std::snprintf(out, size, "std::runtime_error: %s", error.what());
  } catch(const std::exception & error) {
    std::snprintf(out, size, "std::exception: %s", error.what());
  } catch(...) {
    std::snprintf(out, size, "unknown C++ exception");
  }
}

struct JumpBuffer {
  std::jmp_buf value;
};

template<typename Fn>
struct CallState {
  Fn * fn;
  bool r_unwind;
  SEXP token;
  bool failed;
  char message[512];
};

template<typename Fn>
SEXP call_body(void * data) noexcept {
  CallState<Fn> * state = static_cast<CallState<Fn> *>(data);
  try {
    return (*state->fn)();
  } catch(const RUnwind & error) {
    // An R error from a nested unwind_protect. This one keeps its type: the
    // token is what lets the original R error continue.
    state->r_unwind = true;
    state->token = error.token;
    return R_NilValue;
  } catch(...) {
    // Everything else is flattened to text. std::exception_ptr would corrupt
    // the heap where a libc++ package is loaded into a libstdc++ R, and the
    // C++ type is not observable once this becomes an R condition.
    state->failed = true;
    describe_current_exception(state->message, sizeof(state->message));
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
  unwind_detail::CallState<fun_type> state{&fn, false, R_NilValue, false, {}};
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
  if(state.r_unwind) {
    throw unwind_detail::RUnwind{state.token};
  }
  if(state.failed) {
    throw std::runtime_error(state.message);
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
