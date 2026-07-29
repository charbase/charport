// Benchmark kernels, compiled at bench time with R CMD SHLIB.

#include "charport.h"
#include "consumer-boundary.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> corpus_lines;
bool corpus_ready = false;

template<typename Fn>
SEXP guarded(const char * operation, Fn fn) {
  return charport_consumer::boundary(operation, fn);
}

static void join_all(std::vector<std::thread> & threads) {
  for(std::thread & thread : threads) {
    if(thread.joinable()) {
      thread.join();
    }
  }
}

static inline uint64_t fnv1a_string(const char * p, uint32_t n) {
  uint64_t h = 14695981039346656037ULL;
  for(uint32_t i = 0; i < n; ++i) {
    h ^= static_cast<unsigned char>(p[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

static SEXP hash_to_sexp(uint64_t h, R_xlen_t n_na) {
  SEXP out = Rf_allocVector(REALSXP, 2);
  REAL(out)[0] = static_cast<double>(h % 9007199254740992ULL);  // 2^53
  REAL(out)[1] = static_cast<double>(n_na);
  return out;
}

static SEXP protected_hash_to_sexp(uint64_t h, R_xlen_t n_na) {
  return charport_consumer::unwind_protect([&]() -> SEXP {
    return hash_to_sexp(h, n_na);
  });
}

static size_t reader_block_size() noexcept {
  return 8192;
}

static void require_corpus() {
  if(!corpus_ready) {
    throw std::runtime_error("corpus is not loaded");
  }
}

static SEXP corpus_hash() {
  require_corpus();
  uint64_t h = 0;
  for(const std::string & s : corpus_lines) {
    h ^= fnv1a_string(s.data(), static_cast<uint32_t>(s.size()));
  }
  return hash_to_sexp(h, 0);
}

static double corpus_total_bytes() {
  require_corpus();
  double total = 0;
  for(const std::string & s : corpus_lines) {
    total += static_cast<double>(s.size());
  }
  return total;
}

static SEXP corpus_info() {
  require_corpus();
  const char * names[] = {"n", "total_bytes", "hash", ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SEXP hash = PROTECT(corpus_hash());
  SET_VECTOR_ELT(out, 0, Rf_ScalarReal(static_cast<double>(corpus_lines.size())));
  SET_VECTOR_ELT(out, 1, Rf_ScalarReal(corpus_total_bytes()));
  SET_VECTOR_ELT(out, 2, hash);
  UNPROTECT(2);
  return out;
}

static SEXP build_charvec_serial() {
  require_corpus();
  const R_xlen_t n = static_cast<R_xlen_t>(corpus_lines.size());

  charport::charvec::Builder b(n);
  for(R_xlen_t i = 0; i < n; ++i) {
    const std::string & s = corpus_lines[static_cast<size_t>(i)];
    b.set(i, s.data(), s.size(), cetype_ext_t::CE_UTF8);
  }
  return b.to_sexp();
}

static SEXP build_charvec_parallel(int n_threads) {
  require_corpus();
  if(n_threads < 1) {
    throw std::runtime_error("n_threads must be at least 1");
  }
  const R_xlen_t n = static_cast<R_xlen_t>(corpus_lines.size());

  charport::charvec::ParallelBuilder b(n, static_cast<size_t>(n_threads));
  std::vector<std::thread> workers;
  std::vector<std::exception_ptr> errors(static_cast<size_t>(n_threads));
  workers.reserve(static_cast<size_t>(n_threads));
  try {
    for(int t = 0; t < n_threads; ++t) {
      const R_xlen_t lo = n * t / n_threads;
      const R_xlen_t hi = n * (t + 1) / n_threads;
      workers.emplace_back([&, t, lo, hi]() {
        try {
          const size_t shard = static_cast<size_t>(t);
          for(R_xlen_t i = lo; i < hi; ++i) {
            const std::string & s = corpus_lines[static_cast<size_t>(i)];
            b.set(shard, i, s.data(), s.size(), cetype_ext_t::CE_UTF8);
          }
        } catch(...) {
          errors[static_cast<size_t>(t)] = std::current_exception();
        }
      });
    }
  } catch(...) {
    join_all(workers);
    throw;
  }
  join_all(workers);
  for(const std::exception_ptr & error : errors) {
    if(error) {
      std::rethrow_exception(error);
    }
  }
  return b.to_sexp();
}

} // namespace

extern "C" SEXP C_prepare_data_for_benchmark(SEXP path_) {
  const char * path = CHAR(STRING_ELT(path_, 0));
  return guarded("prepare_data_for_benchmark",
                 [&]() -> SEXP {
    typedef std::unique_ptr<std::FILE, int (*)(std::FILE *)> file_ptr;
    file_ptr file(std::fopen(path, "rb"), &std::fclose);
    if(!file) {
      throw std::runtime_error(std::string("cannot open ") + path);
    }
    std::fseek(file.get(), 0, SEEK_END);
    const long fsize = std::ftell(file.get());
    std::fseek(file.get(), 0, SEEK_SET);
    if(fsize < 0) {
      throw std::runtime_error(std::string("cannot size ") + path);
    }
    std::unique_ptr<char[]> buf(new char[static_cast<size_t>(fsize)]);
    const size_t got = std::fread(
      buf.get(), 1, static_cast<size_t>(fsize), file.get()
    );
    file.reset();
    if(got != static_cast<size_t>(fsize)) {
      throw std::runtime_error(std::string("short read on ") + path);
    }

    size_t n = 0;
    for(size_t i = 0; i < got; ++i) {
      if(buf[i] == '\n') { ++n; }
    }
    if(got > 0 && buf[got - 1] != '\n') { ++n; }

    corpus_ready = false;
    corpus_lines.clear();
    corpus_lines.reserve(n);

    size_t start = 0;
    for(size_t line = 0; line < n; ++line) {
      size_t end = start;
      while(end < got && buf[end] != '\n') { ++end; }
      corpus_lines.emplace_back(buf.get() + start, end - start);
      start = end + 1;
    }

    corpus_ready = true;
    buf.reset();
    return charport_consumer::unwind_protect([]() -> SEXP { return corpus_info(); });
  });
}

extern "C" SEXP C_bench_SET_STRING_ELT(void) {
  return guarded("SET_STRING_ELT", []() -> SEXP {
    require_corpus();
    return charport_consumer::unwind_protect([]() -> SEXP {
      SEXP out = PROTECT(
        Rf_allocVector(STRSXP, static_cast<R_xlen_t>(corpus_lines.size()))
      );
      for(size_t i = 0; i < corpus_lines.size(); ++i) {
        const std::string & s = corpus_lines[i];
        if(s.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
          throw std::runtime_error("string size exceeds R string size");
        }
        SET_STRING_ELT(out, static_cast<R_xlen_t>(i),
                       Rf_mkCharLenCE(s.data(), static_cast<int>(s.size()), CE_UTF8));
      }
      UNPROTECT(1);
      return out;
    });
  });
}

extern "C" SEXP C_bench_charvec_Builder(void) {
  return guarded("charvec_Builder", []() -> SEXP {
    return build_charvec_serial();
  });
}

extern "C" SEXP C_bench_charvec_ParallelBuilder(SEXP n_threads_) {
  const int n_threads = Rf_asInteger(n_threads_);
  return guarded("charvec_ParallelBuilder",
                 [n_threads]() -> SEXP {
    return build_charvec_parallel(n_threads);
  });
}

extern "C" SEXP C_bench_STRING_PTR_RO_hash(SEXP x) {
  const R_xlen_t n = Rf_xlength(x);
  const SEXP * ptr = STRING_PTR_RO(x);
  uint64_t h = 0;
  R_xlen_t n_na = 0;
  for(R_xlen_t i = 0; i < n; ++i) {
    SEXP cs = ptr[i];
    if(cs == NA_STRING) { ++n_na; continue; }
    h ^= fnv1a_string(R_CHAR(cs), static_cast<uint32_t>(Rf_xlength(cs)));
  }
  return hash_to_sexp(h, n_na);
}

extern "C" SEXP C_bench_charport_Reader_hash(SEXP x) {
  return guarded("Reader_hash", [&]() -> SEXP {
    charport::Reader r(x);
    uint64_t h = 0;
    R_xlen_t n_na = 0;
    charport::ByteViews byteviews(reader_block_size());
    for(R_xlen_t start = 0;
        start < r.size();
        start += static_cast<R_xlen_t>(byteviews.size())) {
      const R_xlen_t m = std::min<R_xlen_t>(
        static_cast<R_xlen_t>(byteviews.size()), r.size() - start);
      r.byteviews(start, m, byteviews);
      for(R_xlen_t j = 0; j < m; ++j) {
        const charport::ByteView v = byteviews[j];
        if(v.is_na()) { ++n_na; continue; }
        h ^= fnv1a_string(v.ptr, static_cast<uint32_t>(v.len));
      }
    }
    return protected_hash_to_sexp(h, n_na);
  });
}

extern "C" SEXP C_bench_charport_Reader_hash_scalar(SEXP x) {
  return guarded("Reader_hash_scalar",
                 [&]() -> SEXP {
    charport::Reader r(x);
    uint64_t h = 0;
    R_xlen_t n_na = 0;
    for(R_xlen_t i = 0; i < r.size(); ++i) {
      charport::ByteView v = r.byteview(i);
      if(v.is_na()) { ++n_na; continue; }
      h ^= fnv1a_string(v.ptr, static_cast<uint32_t>(v.len));
    }
    return protected_hash_to_sexp(h, n_na);
  });
}

extern "C" SEXP C_bench_charport_Reader_hash_block1(SEXP x) {
  return guarded("Reader_hash_block1",
                 [&]() -> SEXP {
    charport::Reader r(x);
    uint64_t h = 0;
    R_xlen_t n_na = 0;
    const char * ptr = nullptr;
    int len = NA_INTEGER;
    for(R_xlen_t i = 0; i < r.size(); ++i) {
      r.byteviews(i, 1, &ptr, &len);
      const charport::ByteView v = make_byteview(ptr, len);
      if(v.is_na()) { ++n_na; continue; }
      h ^= fnv1a_string(v.ptr, static_cast<uint32_t>(v.len));
    }
    return protected_hash_to_sexp(h, n_na);
  });
}

extern "C" SEXP C_bench_charport_Reader_hash_threads(SEXP x, SEXP n_threads_) {
  const int k = Rf_asInteger(n_threads_);
  return guarded("Reader_hash_threads",
                 [&, k]() -> SEXP {
    if(k < 1) {
      throw std::runtime_error("n_threads must be at least 1");
    }
    charport::Reader r(x);
    if(!r.reentrant()) {
      throw std::runtime_error("reader is not reentrant");
    }
    const R_xlen_t n = r.size();
    std::vector<uint64_t> hashes(static_cast<size_t>(k), 0);
    std::vector<R_xlen_t> nas(static_cast<size_t>(k), 0);
    std::vector<std::exception_ptr> errors(static_cast<size_t>(k));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(k));
    try {
      for(int t = 0; t < k; ++t) {
        const R_xlen_t lo = n * t / k, hi = n * (t + 1) / k;
        workers.emplace_back([&, t, lo, hi]() {
          try {
            uint64_t h = 0;
            R_xlen_t n_na = 0;
            charport::ByteViews byteviews(reader_block_size());
            for(R_xlen_t start = lo;
                start < hi;
                start += static_cast<R_xlen_t>(byteviews.size())) {
              const R_xlen_t m = std::min<R_xlen_t>(
                static_cast<R_xlen_t>(byteviews.size()), hi - start);
              r.byteviews(start, m, byteviews);
              for(R_xlen_t j = 0; j < m; ++j) {
                const charport::ByteView v = byteviews[j];
                if(v.is_na()) { ++n_na; continue; }
                h ^= fnv1a_string(v.ptr, static_cast<uint32_t>(v.len));
              }
            }
            hashes[static_cast<size_t>(t)] = h;
            nas[static_cast<size_t>(t)] = n_na;
          } catch(...) {
            errors[static_cast<size_t>(t)] = std::current_exception();
          }
        });
      }
    } catch(...) {
      join_all(workers);
      throw;
    }
    join_all(workers);
    for(const std::exception_ptr & error : errors) {
      if(error) {
        std::rethrow_exception(error);
      }
    }

    uint64_t h = 0;
    R_xlen_t n_na = 0;
    for(int t = 0; t < k; ++t) {
      h ^= hashes[static_cast<size_t>(t)];
      n_na += nas[static_cast<size_t>(t)];
    }
    return protected_hash_to_sexp(h, n_na);
  });
}

extern "C" SEXP C_probe_STRING_PTR_RO_length_sum(SEXP x) {
  const R_xlen_t n = Rf_xlength(x);
  const SEXP * ptr = STRING_PTR_RO(x);
  double total = 0;
  for(R_xlen_t i = 0; i < n; ++i) {
    SEXP cs = ptr[i];
    if(cs != NA_STRING) total += static_cast<double>(Rf_xlength(cs));
  }
  return Rf_ScalarReal(total);
}

extern "C" SEXP C_probe_charport_Reader_length_sum(SEXP x) {
  return guarded("Reader_length_sum",
                 [&]() -> SEXP {
    charport::Reader r(x);
    double total = 0;
    for(R_xlen_t i = 0; i < r.size(); ++i) {
      const int len = r.length(i);
      if(len >= 0) total += static_cast<double>(len);
    }
    return charport_consumer::unwind_protect(
      [total]() -> SEXP { return Rf_ScalarReal(total); }
    );
  });
}

extern "C" SEXP C_probe_charport_Reader_length_sum_block1(SEXP x) {
  return guarded("Reader_length_sum_block1",
                 [&]() -> SEXP {
    charport::Reader r(x);
    double total = 0;
    int len;
    for(R_xlen_t i = 0; i < r.size(); ++i) {
      r.lengths(i, 1, &len);
      if(len >= 0) total += static_cast<double>(len);
    }
    return charport_consumer::unwind_protect(
      [total]() -> SEXP { return Rf_ScalarReal(total); }
    );
  });
}

extern "C" SEXP C_probe_charport_Reader_length_sum_range(SEXP x) {
  return guarded("Reader_length_sum_range",
                 [&]() -> SEXP {
    charport::Reader r(x);
    double total = 0;
    std::vector<int> len(reader_block_size());
    for(R_xlen_t start = 0;
        start < r.size();
        start += static_cast<R_xlen_t>(len.size())) {
      const R_xlen_t m = std::min<R_xlen_t>(
        static_cast<R_xlen_t>(len.size()), r.size() - start);
      r.lengths(start, m, len.data());
      for(R_xlen_t j = 0; j < m; ++j) {
        const int value = len[static_cast<size_t>(j)];
        if(value >= 0) total += static_cast<double>(value);
      }
    }
    return charport_consumer::unwind_protect(
      [total]() -> SEXP { return Rf_ScalarReal(total); }
    );
  });
}
