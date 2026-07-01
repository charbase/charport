// A minimal external charport consumer, compiled at test time with
// R CMD SHLIB (see test_threaded_consumer.R). It is a separate DSO that knows
// charport only through the installed public header and R_GetCCallable,
// exactly like a downstream package. Thread flags live in the test-local
// Makevars, so charport itself carries none.
//
// The parallel-consumer pattern: worker threads share a reentrant Reader by
// reference and write disjoint ranges through their own ParallelBuilder shard index.
// Worker exceptions are caught in the worker and re-raised after the join.

#include "charport.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename Fun>
SEXP guarded(const char * op, Fun fun) {
  char msg[512];
  try {
    return fun();
  } catch(const std::exception & e) {
    // snprintf copies the message; Rf_error must run after C++ catch exits.
    std::snprintf(msg, sizeof(msg), "%s", e.what());
  } catch(...) {
    std::snprintf(msg, sizeof(msg), "unknown C++ exception");
  }
  Rf_error("threaded_consumer %s: %s", op, msg);
  return R_NilValue;
}

} // namespace

extern "C" {

SEXP C_consumer_abi_ok(void) {
  return Rf_ScalarLogical(charport::check_abi() ? TRUE : FALSE);
}

SEXP C_consumer_threaded_rebuild(SEXP x, SEXP n_threads_) {
  return guarded("threaded_rebuild", [&]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
    const int k = Rf_asInteger(n_threads_);
    if(k == NA_INTEGER || k < 1 || k > 16) {
      throw std::runtime_error("n_threads must be in 1..16");
    }
    if(!r.reentrant()) {
      throw std::runtime_error("input reader is not reentrant");
    }

    charport::charvec::ParallelBuilder b(n, static_cast<size_t>(k));

    std::vector<std::string> worker_errors(static_cast<size_t>(k));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(k));
    for(int j = 0; j < k; ++j) {
      const R_xlen_t lo = n * j / k;
      const R_xlen_t hi = n * (j + 1) / k;
      const size_t shard = static_cast<size_t>(j);
      std::string * err = &worker_errors[static_cast<size_t>(j)];
      threads.emplace_back([&b, &r, shard, lo, hi, err]() {
        try {
          for(R_xlen_t i = lo; i < hi; ++i) {
            b.set(shard, i, r[i]);  // distinct shard per worker -> no sync
          }
        } catch(const std::exception & e) {
          *err = e.what();
        } catch(...) {
          *err = "unknown C++ exception";
        }
      });
    }
    for(std::thread & t : threads) {
      t.join();
    }
    for(const std::string & err : worker_errors) {
      if(!err.empty()) {
        throw std::runtime_error(err);
      }
    }
    return b.to_sexp();
  });
}

// Exercise the catch-join-re-raise path directly: the builder has no encoding
// policy, so no normal input makes a worker set() throw. One worker throws a
// std::runtime_error; it must be caught in the worker, surface after the join,
// and be re-raised as an R error on the main thread.
SEXP C_consumer_worker_throws(void) {
  return guarded("worker_throws", [&]() -> SEXP {
    charport::charvec::ParallelBuilder b(2, 2);
    std::vector<std::string> worker_errors(2);
    std::vector<std::thread> threads;
    for(int j = 0; j < 2; ++j) {
      std::string * err = &worker_errors[static_cast<size_t>(j)];
      threads.emplace_back([&b, j, err]() {
        try {
          if(j == 1) {
            throw std::runtime_error("injected worker failure");
          }
          b.set(static_cast<size_t>(j), j, "ok", 2, charport_enc::CE_ASCII);
        } catch(const std::exception & e) {
          *err = e.what();
        }
      });
    }
    for(std::thread & t : threads) {
      t.join();
    }
    for(const std::string & err : worker_errors) {
      if(!err.empty()) {
        throw std::runtime_error(err);
      }
    }
    return b.to_sexp();
  });
}

} // extern "C"
