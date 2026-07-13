#ifndef CHARPORT_INTEROP_UNWIND_H
#define CHARPORT_INTEROP_UNWIND_H

// Bridge R errors through live C++ objects. Include charport.h.

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
    // A fresh token costs ~20ns over a cpp11-style preserved static (measured;
    // the R_UnwindProtect context setup dominates either way). We pay it: this
    // bridge runs once per Reader/Builder operation, not per element, and a
    // static token is shared state whose safety rests on every caller's
    // destructor discipline. Revisit only if this ever shows up in a profile.
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

#if defined(RCPP_VERSION)
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
#endif

#if defined(CHARPORT_CPP11_INCLUDED)
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
#endif

enum class exit_kind : unsigned char {
  success,
  r_error,
  cpp_error
};

struct outcome {
  exit_kind kind;
  SEXP value;
  SEXP token;
  bool release_token;
  char message[512];
};

// A TU that includes Rcpp or cpp11 before charport.h protects R calls with
// that framework's backend, so an R error reaches a hand-written boundary as
// the framework's unwind exception, not charport's. Recognize all three so
// the original R condition continues instead of degrading to a generic error.
template<typename Fn>
outcome run(Fn && fn) noexcept {
  outcome out;
  out.kind = exit_kind::success;
  out.value = R_NilValue;
  out.token = R_NilValue;
  out.release_token = false;
  try {
    out.value = std::forward<Fn>(fn)();
  } catch(const unwind_exception & error) {
    out.kind = exit_kind::r_error;
    out.token = error.token;
    out.release_token = true;
#if defined(RCPP_VERSION)
  } catch(const Rcpp::LongjumpException & error) {
    // Rcpp preserves its token per throw, like the standalone backend.
    out.kind = exit_kind::r_error;
    out.token = error.token;
    out.release_token = true;
#endif
#if defined(CHARPORT_CPP11_INCLUDED)
  } catch(const cpp11::unwind_exception & error) {
    // cpp11's token is a preserved process-lifetime static; never release it.
    out.kind = exit_kind::r_error;
    out.token = error.token;
#endif
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

// Use as the first operation in a hand-written .Call entry point.
template<typename Fn>
SEXP r_boundary(const char * operation, Fn && fn) {
  typedef typename std::decay<Fn>::type body_type;
  static_assert(std::is_trivially_destructible<body_type>::value,
                "r_boundary callback must be trivially destructible");

  unwind_detail::outcome out =
    unwind_detail::run(std::forward<Fn>(fn));

  if(out.kind == unwind_detail::exit_kind::r_error) {
    if(out.release_token) {
      R_ReleaseObject(out.token);
    }
    R_ContinueUnwind(out.token);
    (Rf_error)("charport %s: failed to continue R error", operation);
  }
  if(out.kind == unwind_detail::exit_kind::cpp_error) {
    (Rf_error)("charport %s: %s", operation, out.message);
  }

  return out.value;
}
} // namespace charport

#endif
