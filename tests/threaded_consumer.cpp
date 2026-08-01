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
#include "consumer-boundary.h"

#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename Fun>
SEXP guarded(const char * op, Fun fun) {
  return charport_consumer::boundary(op, fun);
}

// Workers report failures as text. std::exception_ptr would corrupt the heap
// where a libc++ package is loaded into a libstdc++ R, and these errors only
// ever become R conditions.
std::string current_exception_text() {
  char buffer[512];
  charport_consumer::unwind_detail::describe_current_exception(buffer, sizeof(buffer));
  return std::string(buffer);
}

void join_all(std::vector<std::thread> & threads) {
  for(std::thread & thread : threads) {
    if(thread.joinable()) {
      thread.join();
    }
  }
}

int thread_error_strviews(
    void *, R_xlen_t, R_xlen_t size, const char ** out_ptrs,
    int * out_lens, cetype_ext_t * out_encodings) {
  for(R_xlen_t i = 0; i < size; ++i) {
    out_ptrs[i] = nullptr;
    out_lens[i] = NA_INTEGER;
    out_encodings[i] = CETYPE_EXT_NA;
  }
  return CHARPORT_STATUS_ERROR;
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

    std::vector<std::string> worker_errors(static_cast<size_t>(k));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(k));
    try {
      for(int j = 0; j < k; ++j) {
        const R_xlen_t lo = n * j / k;
        const R_xlen_t hi = n * (j + 1) / k;
        const size_t shard = static_cast<size_t>(j);
        std::string * error = &worker_errors[static_cast<size_t>(j)];
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
            *error = current_exception_text();
          }
        });
      }
    } catch(...) {
      join_all(threads);
      throw;
    }
    join_all(threads);
    for(const std::string & error : worker_errors) {
      if(!error.empty()) {
        throw std::runtime_error(error);
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

    std::vector<std::string> worker_errors(static_cast<size_t>(k));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(k));
    try {
      for(int j = 0; j < k; ++j) {
        const R_xlen_t lo = n * j / k;
        const R_xlen_t hi = n * (j + 1) / k;
        std::string * error = &worker_errors[static_cast<size_t>(j)];
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
            *error = current_exception_text();
          }
        });
      }
    } catch(...) {
      join_all(threads);
      throw;
    }
    join_all(threads);
    for(const std::string & error : worker_errors) {
      if(!error.empty()) {
        throw std::runtime_error(error);
      }
    }

    return charport_consumer::unwind_protect([&]() -> SEXP {
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
    std::vector<std::string> worker_errors(2);
    std::vector<std::thread> threads;
    try {
      for(int j = 0; j < 2; ++j) {
        std::string * error = &worker_errors[static_cast<size_t>(j)];
        threads.emplace_back([&b, j, error]() {
          try {
            if(j == 1) {
              throw std::runtime_error("injected worker failure");
            }
            b.set(static_cast<size_t>(j), j, "ok", 2, CETYPE_EXT_ASCII);
          } catch(...) {
            *error = current_exception_text();
          }
        });
      }
    } catch(...) {
      join_all(threads);
      throw;
    }
    join_all(threads);
    for(const std::string & error : worker_errors) {
      if(!error.empty()) {
        throw std::runtime_error(error);
      }
    }
    return b.to_sexp();
  });
}

SEXP C_consumer_threaded_access_errors(void) {
  return guarded("threaded_access_errors", []() -> SEXP {
    charport_reader raw{};
    raw.n = 2;
    raw.range.strviews = thread_error_strviews;
    raw.capabilities = charport_reader_capabilities{false, true};
    charport::Reader reader(raw);

    std::vector<std::string> errors(2);
    std::vector<std::thread> threads;
    threads.reserve(2);
    try {
      for(R_xlen_t i = 0; i < 2; ++i) {
        threads.emplace_back([&reader, &errors, i]() {
          try {
            (void)reader.view(i);
          } catch(...) {
            errors[static_cast<size_t>(i)] = current_exception_text();
          }
        });
      }
    } catch(...) {
      join_all(threads);
      throw;
    }
    join_all(threads);

    bool ok = true;
    for(size_t i = 0; i < errors.size(); ++i) {
      if(errors[i].empty()) {
        ok = false;
        continue;
      }
      try {
        throw std::runtime_error(errors[i]);
      } catch(const std::runtime_error & error) {
        ok = ok && std::strcmp(
          error.what(),
          "std::runtime_error: charport Reader access failed") == 0;
      } catch(...) {
        ok = false;
      }
    }

    return charport_consumer::unwind_protect(
      [ok]() -> SEXP { return Rf_ScalarLogical(ok ? TRUE : FALSE); }
    );
  });
}

} // extern "C"
