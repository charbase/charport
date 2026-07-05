// Benchmark kernels, compiled at bench time with R CMD SHLIB.

#include "charport.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> corpus_lines;
bool corpus_ready = false;

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
  const R_xlen_t n = static_cast<R_xlen_t>(corpus_lines.size());

  charport::charvec::ParallelBuilder b(n, static_cast<size_t>(n_threads));
  std::vector<std::thread> workers;
  std::vector<std::string> errors(static_cast<size_t>(n_threads));
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
      } catch(const std::exception & e) {
        errors[static_cast<size_t>(t)] = e.what();
      }
    });
  }
  for(std::thread & w : workers) { w.join(); }
  for(const std::string & e : errors) {
    if(!e.empty()) { throw std::runtime_error(e); }
  }
  return b.to_sexp();
}

} // namespace

extern "C" SEXP C_prepare_data_for_benchmark(SEXP path_) {
  try {
    const char * path = CHAR(STRING_ELT(path_, 0));

    std::FILE * f = std::fopen(path, "rb");
    if(f == nullptr) {
      throw std::runtime_error(std::string("cannot open ") + path);
    }
    std::fseek(f, 0, SEEK_END);
    const long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if(fsize < 0) {
      std::fclose(f);
      throw std::runtime_error(std::string("cannot size ") + path);
    }
    std::unique_ptr<char[]> buf(new char[static_cast<size_t>(fsize)]);
    const size_t got = std::fread(buf.get(), 1, static_cast<size_t>(fsize), f);
    std::fclose(f);
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
    return corpus_info();
  } catch(const std::exception & e) {
    Rf_error("prepare_data_for_benchmark: %s", e.what());
  }
}

extern "C" SEXP C_bench_SET_STRING_ELT(void) {
  try {
    require_corpus();
    SEXP out = PROTECT(Rf_allocVector(STRSXP, static_cast<R_xlen_t>(corpus_lines.size())));
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
  } catch(const std::exception & e) {
    Rf_error("SET_STRING_ELT: %s", e.what());
  }
}

extern "C" SEXP C_bench_charvec_Builder(void) {
  try {
    return build_charvec_serial();
  } catch(const std::exception & e) {
    Rf_error("charvec_Builder: %s", e.what());
  }
}

extern "C" SEXP C_bench_charvec_ParallelBuilder(SEXP n_threads_) {
  try {
    return build_charvec_parallel(Rf_asInteger(n_threads_));
  } catch(const std::exception & e) {
    Rf_error("charvec_ParallelBuilder: %s", e.what());
  }
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
  charport::Reader r(x);
  uint64_t h = 0;
  R_xlen_t n_na = 0;
  charport::ByteViews byteviews(reader_block_size());
  for(R_xlen_t start = 0; start < r.size(); start += static_cast<R_xlen_t>(byteviews.size())) {
    const R_xlen_t m = std::min<R_xlen_t>(
      static_cast<R_xlen_t>(byteviews.size()), r.size() - start);
    r.byteviews(start, m, byteviews);
    for(R_xlen_t j = 0; j < m; ++j) {
      const charport::ByteView v = byteviews[j];
      if(v.is_na()) { ++n_na; continue; }
      h ^= fnv1a_string(v.ptr, static_cast<uint32_t>(v.len));
    }
  }
  return hash_to_sexp(h, n_na);
}

extern "C" SEXP C_bench_charport_Reader_hash_scalar(SEXP x) {
  charport::Reader r(x);
  uint64_t h = 0;
  R_xlen_t n_na = 0;
  for(R_xlen_t i = 0; i < r.size(); ++i) {
    charport::ByteView v = r.byteview(i);
    if(v.is_na()) { ++n_na; continue; }
    h ^= fnv1a_string(v.ptr, static_cast<uint32_t>(v.len));
  }
  return hash_to_sexp(h, n_na);
}

extern "C" SEXP C_bench_charport_Reader_hash_block1(SEXP x) {
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
  return hash_to_sexp(h, n_na);
}

extern "C" SEXP C_bench_charport_Reader_hash_threads(SEXP x, SEXP n_threads_) {
  charport::Reader r(x);
  if(!r.reentrant()) Rf_error("reader is not reentrant");
  const int k = Rf_asInteger(n_threads_);
  const R_xlen_t n = r.size();
  std::vector<uint64_t> hashes(static_cast<size_t>(k), 0);
  std::vector<R_xlen_t> nas(static_cast<size_t>(k), 0);
  std::vector<std::thread> workers;
  for(int t = 0; t < k; ++t) {
    const R_xlen_t lo = n * t / k, hi = n * (t + 1) / k;
    workers.emplace_back([&, t, lo, hi]() {
      uint64_t h = 0;
      R_xlen_t n_na = 0;
      charport::ByteViews byteviews(reader_block_size());
      for(R_xlen_t start = lo; start < hi; start += static_cast<R_xlen_t>(byteviews.size())) {
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
    });
  }
  uint64_t h = 0;
  R_xlen_t n_na = 0;
  for(int t = 0; t < k; ++t) {
    workers[static_cast<size_t>(t)].join();
    h ^= hashes[static_cast<size_t>(t)];
    n_na += nas[static_cast<size_t>(t)];
  }
  return hash_to_sexp(h, n_na);
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
  charport::Reader r(x);
  double total = 0;
  for(R_xlen_t i = 0; i < r.size(); ++i) {
    const int len = r.length(i);
    if(len >= 0) total += static_cast<double>(len);
  }
  return Rf_ScalarReal(total);
}

extern "C" SEXP C_probe_charport_Reader_length_sum_block1(SEXP x) {
  charport::Reader r(x);
  double total = 0;
  int len;
  for(R_xlen_t i = 0; i < r.size(); ++i) {
    r.lengths(i, 1, &len);
    if(len >= 0) total += static_cast<double>(len);
  }
  return Rf_ScalarReal(total);
}

extern "C" SEXP C_probe_charport_Reader_length_sum_range(SEXP x) {
  charport::Reader r(x);
  double total = 0;
  std::vector<int> len(reader_block_size());
  for(R_xlen_t start = 0; start < r.size(); start += static_cast<R_xlen_t>(len.size())) {
    const R_xlen_t m = std::min<R_xlen_t>(
      static_cast<R_xlen_t>(len.size()), r.size() - start);
    r.lengths(start, m, len.data());
    for(R_xlen_t j = 0; j < m; ++j) {
      const int x = len[static_cast<size_t>(j)];
      if(x >= 0) total += static_cast<double>(x);
    }
  }
  return Rf_ScalarReal(total);
}
