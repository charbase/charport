// A minimal external charport consumer, compiled at test time with
// R CMD SHLIB (see test_threaded_consumer.R) -- a separate DSO that knows
// charport only through the installed public header and R_GetCCallable,
// exactly like a downstream package. Thread flags live in the test-local
// Makevars, so charport itself carries none.
//
// The parallel-consumer pattern: worker threads read x through copies of
// the reader POD and write disjoint ranges through their own shards -- no R
// API off the main thread on either side. Worker exceptions are caught in
// the worker and re-raised after the join.

#include "charport.hpp"

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename Fun>
SEXP guarded(const char * op, Fun fun) {
  try {
    return fun();
  } catch(const std::exception & e) {
    Rf_error("threaded_consumer %s: %s", op, e.what());
  } catch(...) {
    Rf_error("threaded_consumer %s: unknown C++ exception", op);
  }
}

} // namespace

extern "C" {

SEXP C_consumer_abi_ok(void) {
  cp::check_abi();
  return Rf_ScalarLogical(TRUE);
}

SEXP C_consumer_threaded_rebuild(SEXP x, SEXP n_threads_) {
  return guarded("threaded_rebuild", [&]() -> SEXP {
    cp::Reader r(x);
    const R_xlen_t n = r.size();
    const int k = Rf_asInteger(n_threads_);
    if(k == NA_INTEGER || k < 1 || k > 16) {
      throw std::runtime_error("n_threads must be in 1..16");
    }
    if(!r.reentrant()) {
      throw std::runtime_error("input reader is not reentrant");
    }

    cp::charvec::Builder b(n);
    std::vector<cp::charvec::BuilderShard> shards;
    shards.reserve(static_cast<size_t>(k));
    for(int j = 0; j < k; ++j) {
      shards.push_back(b.shard());
    }

    const charport_reader raw = r.raw();
    std::vector<std::string> worker_errors(static_cast<size_t>(k));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(k));
    for(int j = 0; j < k; ++j) {
      const R_xlen_t lo = n * j / k;
      const R_xlen_t hi = n * (j + 1) / k;
      const cp::charvec::BuilderShard shard = shards[static_cast<size_t>(j)];
      std::string * err = &worker_errors[static_cast<size_t>(j)];
      threads.emplace_back([raw, shard, lo, hi, err]() {
        try {
          const cp::Reader worker_reader(raw);  // adopt the POD on the worker
          for(R_xlen_t i = lo; i < hi; ++i) {
            shard.set(i, worker_reader[i]);
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
    return b.finish();
  });
}

} // extern "C"
