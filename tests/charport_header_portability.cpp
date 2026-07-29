#include "charport.h"

#include <type_traits>
#include <utility>

template<typename T>
class has_with_rcpp {
  template<typename U>
  static auto test(int) -> decltype((void)&U::with_rcpp, std::true_type());

  template<typename>
  static std::false_type test(...);

public:
  static const bool value = decltype(test<T>(0))::value;
};

template<typename T>
class has_with_cpp11 {
  template<typename U>
  static auto test(int) -> decltype((void)&U::with_cpp11, std::true_type());

  template<typename>
  static std::false_type test(...);

public:
  static const bool value = decltype(test<T>(0))::value;
};

template<typename T>
class has_to_sexp_with_rcpp {
  template<typename U>
  static auto test(int) ->
    decltype((void)&U::to_sexp_with_rcpp, std::true_type());

  template<typename>
  static std::false_type test(...);

public:
  static const bool value = decltype(test<T>(0))::value;
};

template<typename T>
class has_to_sexp_with_cpp11 {
  template<typename U>
  static auto test(int) ->
    decltype((void)&U::to_sexp_with_cpp11, std::true_type());

  template<typename>
  static std::false_type test(...);

public:
  static const bool value = decltype(test<T>(0))::value;
};

template<typename T>
class has_wrap_with_rcpp {
  template<typename U>
  static auto test(int) ->
    decltype(wrap_with_rcpp(std::declval<U>()), std::true_type());

  template<typename>
  static std::false_type test(...);

public:
  static const bool value = decltype(test<T>(0))::value;
};

template<typename T>
class has_wrap_with_cpp11 {
  template<typename U>
  static auto test(int) ->
    decltype(wrap_with_cpp11(std::declval<U>()), std::true_type());

  template<typename>
  static std::false_type test(...);

public:
  static const bool value = decltype(test<T>(0))::value;
};

static_assert(!has_with_rcpp<charport::Reader>::value,
              "Reader::with_rcpp requires Rcpp");
static_assert(!has_with_cpp11<charport::Reader>::value,
              "Reader::with_cpp11 requires cpp11");
static_assert(!has_to_sexp_with_rcpp<charport::charvec::Builder>::value,
              "Builder::to_sexp_with_rcpp requires Rcpp");
static_assert(!has_to_sexp_with_cpp11<charport::charvec::Builder>::value,
              "Builder::to_sexp_with_cpp11 requires cpp11");
static_assert(!has_to_sexp_with_rcpp<charport::charvec::ParallelBuilder>::value,
              "ParallelBuilder::to_sexp_with_rcpp requires Rcpp");
static_assert(!has_to_sexp_with_cpp11<charport::charvec::ParallelBuilder>::value,
              "ParallelBuilder::to_sexp_with_cpp11 requires cpp11");
static_assert(!has_to_sexp_with_rcpp<charport::charvec::GrowableBuilder>::value,
              "GrowableBuilder::to_sexp_with_rcpp requires Rcpp");
static_assert(!has_to_sexp_with_cpp11<charport::charvec::GrowableBuilder>::value,
              "GrowableBuilder::to_sexp_with_cpp11 requires cpp11");
static_assert(!has_wrap_with_rcpp<charport::charvec::Store &&>::value,
              "charvec::wrap_with_rcpp requires Rcpp");
static_assert(!has_wrap_with_cpp11<charport::charvec::Store &&>::value,
              "charvec::wrap_with_cpp11 requires cpp11");

static_assert(noexcept(charport::convert_current_exception_to_status()),
              "access exception translation must not throw");

int convert_runtime_error() {
  try {
    throw 1;
  } catch(...) {
    return charport::convert_current_exception_to_status();
  }
}
