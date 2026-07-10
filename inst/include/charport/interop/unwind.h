#ifndef CHARPORT_INTEROP_UNWIND_H
#define CHARPORT_INTEROP_UNWIND_H

// Bridge R errors through live C++ objects. Include a charport umbrella header.

#ifndef R_NO_REMAP
#define R_NO_REMAP
#endif
#include <Rinternals.h>

#include <csetjmp>
#include <cstdio>
#include <exception>
#include <type_traits>
#include <utility>

namespace charport {
namespace unwind_detail {

struct unwind_exception {
  SEXP token;
};

struct jump_buffer {
  std::jmp_buf value;
};

template<typename Fn>
struct call_state {
  Fn * fn;
  std::exception_ptr error;
};

template<typename Fn>
SEXP call_body(void * ptr) noexcept {
  call_state<Fn> * state = static_cast<call_state<Fn> *>(ptr);
  try {
    return (*state->fn)();
  } catch(...) {
    // Return through R's C frames before rethrowing in C++.
    state->error = std::current_exception();
    return R_NilValue;
  }
}

inline void call_cleanup(void * ptr, Rboolean jump) noexcept {
  if(jump == TRUE) {
    longjmp(static_cast<jump_buffer *>(ptr)->value, 1);
  }
}

struct standalone_backend {
  template<typename Fn>
  static SEXP call(Fn && fn) {
    typedef typename std::remove_reference<Fn>::type fun_type;
    call_state<fun_type> state{&fn, std::exception_ptr()};
    jump_buffer jump;
    SEXP token = PROTECT(R_MakeUnwindCont());

    if(setjmp(jump.value) != 0) {
      // The C++ exception may travel through user frames before R resumes.
      R_PreserveObject(token);
      UNPROTECT(1);
      throw unwind_exception{token};
    }

    SEXP out = R_UnwindProtect(
      &call_body<fun_type>, &state,
      &call_cleanup, &jump, token
    );

    // R_UnwindProtect temporarily keeps its result in the token.
    SETCAR(token, R_NilValue);
    UNPROTECT(1);
    if(state.error) {
      std::rethrow_exception(state.error);
    }
    return out;
  }
};

enum class exit_kind : unsigned char {
  success,
  r_error,
  cpp_error
};

struct outcome {
  exit_kind kind;
  SEXP value;
  SEXP token;
  char message[512];
};

template<typename Fn>
outcome run(Fn && fn) noexcept {
  outcome out;
  out.kind = exit_kind::success;
  out.value = R_NilValue;
  out.token = R_NilValue;
  try {
    out.value = std::forward<Fn>(fn)();
  } catch(const unwind_exception & error) {
    out.kind = exit_kind::r_error;
    out.token = error.token;
  } catch(const std::exception & error) {
    out.kind = exit_kind::cpp_error;
    std::snprintf(out.message, sizeof(out.message), "%s", error.what());
  } catch(...) {
    out.kind = exit_kind::cpp_error;
    std::snprintf(out.message, sizeof(out.message), "unknown C++ exception");
  }
  return out;
}

} // namespace unwind_detail

// Provider callbacks have no generated framework wrapper around them.
template<typename Fn>
SEXP provider_unwind_protect(Fn && fn) {
  return unwind_detail::standalone_backend::call(std::forward<Fn>(fn));
}

// Use as the first operation in a hand-written .Call entry point.
template<typename Fn>
SEXP r_boundary(const char * operation, Fn && fn) {
  typedef typename std::decay<Fn>::type body_type;
  static_assert(std::is_trivially_destructible<body_type>::value,
                "r_boundary callback must be trivially destructible");

  unwind_detail::outcome out =
    unwind_detail::run(std::forward<Fn>(fn));

  if(out.kind == unwind_detail::exit_kind::r_error) {
    R_ReleaseObject(out.token);
    R_ContinueUnwind(out.token);
    (Rf_error)("charport %s: failed to continue R error", operation);
  }
  if(out.kind == unwind_detail::exit_kind::cpp_error) {
    (Rf_error)("charport %s: %s", operation, out.message);
  }

  return out.value;
}

// The same outer boundary adapted to a charport_reader_init_fn callback.
template<typename Fn>
void * reader_init_boundary(const char * operation, Fn && fn) {
  typedef typename std::decay<Fn>::type body_type;
  static_assert(std::is_trivially_destructible<body_type>::value,
                "reader_init_boundary callback must be trivially destructible");

  void * result = nullptr;
  (void)r_boundary(operation, [&]() -> SEXP {
    result = std::forward<Fn>(fn)();
    return R_NilValue;
  });
  return result;
}

} // namespace charport

#endif
