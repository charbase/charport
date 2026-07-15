// A minimal external charport consumer, compiled at test time with
// R CMD SHLIB (see test_threaded_consumer.R) -- a separately loaded library that knows
// charport only through the installed public header and R_GetCCallable,
// exactly like a downstream package. Thread flags live in the test-local
// Makevars, so charport itself carries none.
//
// The parallel-consumer pattern: worker threads share a reentrant Reader by
// reference and write disjoint ranges through their own ParallelBuilder shard index.
// Worker exceptions are caught in the worker and re-raised after the join.

#include "charport.h"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

template <typename Fun>
SEXP guarded(const char * op, Fun fun) {
  return charport::r_boundary(op, fun);
}

void join_all(std::vector<std::thread> & threads) {
  for(std::thread & thread : threads) {
    if(thread.joinable()) {
      thread.join();
    }
  }
}

} // namespace

extern "C" {

SEXP C_consumer_abi_ok(void) {
  return Rf_ScalarLogical(charport::check_abi() ? TRUE : FALSE);
}

SEXP C_consumer_threaded_rebuild(SEXP x, SEXP n_threads_) {
  const int k = Rf_asInteger(n_threads_);
  return guarded("threaded_rebuild", [&, k]() -> SEXP {
    if(k == NA_INTEGER || k < 1 || k > 16) {
      throw std::runtime_error("n_threads must be in 1..16");
    }

    charport::Reader r(x);
    const R_xlen_t n = r.size();
    if(!r.reentrant()) {
      throw std::runtime_error("input reader is not reentrant");
    }

    charport::charvec::ParallelBuilder b(n, static_cast<size_t>(k));

    std::vector<std::exception_ptr> worker_errors(static_cast<size_t>(k));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(k));
    try {
      for(int j = 0; j < k; ++j) {
        const R_xlen_t lo = n * j / k;
        const R_xlen_t hi = n * (j + 1) / k;
        const size_t shard = static_cast<size_t>(j);
        std::exception_ptr * error = &worker_errors[static_cast<size_t>(j)];
        threads.emplace_back([&b, &r, shard, lo, hi, error]() {
          try {
            const R_xlen_t m = hi - lo;
            charport::StrViews views(m);
            if(m > 0) {
              r.views(lo, m, views);
            }
            for(R_xlen_t i = lo; i < hi; ++i) {
              const size_t j = static_cast<size_t>(i - lo);
              b.set(shard, i, views[static_cast<R_xlen_t>(j)]);
            }
          } catch(...) {
            *error = std::current_exception();
          }
        });
      }
    } catch(...) {
      join_all(threads);
      throw;
    }
    join_all(threads);
    for(const std::exception_ptr & error : worker_errors) {
      if(error) {
        std::rethrow_exception(error);
      }
    }
    return b.to_sexp();
  });
}

SEXP C_consumer_threaded_split(SEXP x, SEXP n_threads_) {
  const int k = Rf_asInteger(n_threads_);
  return guarded("threaded_split", [&, k]() -> SEXP {
    if(k == NA_INTEGER || k < 1 || k > 16) {
      throw std::runtime_error("n_threads must be in 1..16");
    }

    charport::Reader reader(x);
    const R_xlen_t n = reader.size();
    if(!reader.reentrant()) {
      throw std::runtime_error("input reader is not reentrant");
    }

    std::vector<charport::charvec::Store> stores;
    stores.reserve(static_cast<size_t>(k));
    for(int j = 0; j < k; ++j) {
      stores.emplace_back(0, 0);
    }

    std::vector<std::exception_ptr> worker_errors(static_cast<size_t>(k));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(k));
    try {
      for(int j = 0; j < k; ++j) {
        const R_xlen_t lo = n * j / k;
        const R_xlen_t hi = n * (j + 1) / k;
        std::exception_ptr * error = &worker_errors[static_cast<size_t>(j)];
        threads.emplace_back([&reader, &stores, j, lo, hi, error]() {
          try {
            const R_xlen_t count = hi - lo;
            charport::StrViews views(count);
            if(count > 0) {
              reader.views(lo, count, views);
            }

            charport::charvec::GrowableBuilder builder;
            for(R_xlen_t i = 0; i < count; ++i) {
              builder.append(views[i]);
            }
            stores[static_cast<size_t>(j)] = builder.release_store();
          } catch(...) {
            *error = std::current_exception();
          }
        });
      }
    } catch(...) {
      join_all(threads);
      throw;
    }
    join_all(threads);
    for(const std::exception_ptr & error : worker_errors) {
      if(error) {
        std::rethrow_exception(error);
      }
    }

    return charport::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(VECSXP, static_cast<R_xlen_t>(k)));
      for(int j = 0; j < k; ++j) {
        SET_VECTOR_ELT(
          out, static_cast<R_xlen_t>(j),
          charport::charvec::wrap(std::move(stores[static_cast<size_t>(j)])));
      }
      UNPROTECT(1);
      return out;
    });
  });
}

// Exercise the catch-join-re-raise path directly: the builder has no encoding
// policy, so no normal input makes a worker set() throw. One worker throws a
// std::runtime_error; it must be caught in the worker, surface after the join,
// and be re-raised as an R error on the main thread.
SEXP C_consumer_worker_throws(void) {
  return guarded("worker_throws", [&]() -> SEXP {
    charport::charvec::ParallelBuilder b(2, 2);
    std::vector<std::exception_ptr> worker_errors(2);
    std::vector<std::thread> threads;
    try {
      for(int j = 0; j < 2; ++j) {
        std::exception_ptr * error = &worker_errors[static_cast<size_t>(j)];
        threads.emplace_back([&b, j, error]() {
          try {
            if(j == 1) {
              throw std::runtime_error("injected worker failure");
            }
            b.set(static_cast<size_t>(j), j, "ok", 2, cetype_ext_t::CE_ASCII);
          } catch(...) {
            *error = std::current_exception();
          }
        });
      }
    } catch(...) {
      join_all(threads);
      throw;
    }
    join_all(threads);
    for(const std::exception_ptr & error : worker_errors) {
      if(error) {
        std::rethrow_exception(error);
      }
    }
    return b.to_sexp();
  });
}

} // extern "C"
