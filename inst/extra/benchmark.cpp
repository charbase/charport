// Benchmark kernels, compiled at bench time with R CMD SHLIB (see
// benchmark.R). Each read kernel returns an FNV-1a hash of everything it
// read so results can be cross-checked between paths and the work cannot
// be optimized away. Timing happens in R around the .Call; one call per
// run makes the .Call overhead negligible against millions of elements.

#include "charport.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static inline uint64_t fnv1a(const char * p, uint32_t n, uint64_t h) {
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

// Ingest a text file straight into a charvec: file bytes -> line views ->
// Builder. No CHARSXP is ever created, which is the point: Rf_mkCharLenCE
// interns every string in R's global string cache, and a string that is
// already interned costs a hash lookup instead of an allocation. Reading
// the corpus with readLines() first would therefore hand the construction
// baseline cache hits and flatter it considerably. Ingesting in C keeps
// the cache cold so the baseline pays the true fresh-string cost.
// Every na_every-th line is stored as NA to exercise the NA paths.
extern "C" SEXP C_bench_read_lines_charvec(SEXP path_, SEXP na_every_) {
  try {
    const char * path = CHAR(STRING_ELT(path_, 0));
    const int na_every = Rf_asInteger(na_every_);

    std::FILE * f = std::fopen(path, "rb");
    if(f == nullptr) { Rf_error("cannot open %s", path); }
    std::fseek(f, 0, SEEK_END);
    const long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::unique_ptr<char[]> buf(new char[static_cast<size_t>(fsize)]);
    const size_t got = std::fread(buf.get(), 1, static_cast<size_t>(fsize), f);
    std::fclose(f);
    if(got != static_cast<size_t>(fsize)) { Rf_error("short read on %s", path); }

    R_xlen_t n = 0;
    for(size_t i = 0; i < got; ++i) {
      if(buf[i] == '\n') { ++n; }
    }
    if(got > 0 && buf[got - 1] != '\n') { ++n; }

    cp::charvec::Builder b(n);
    size_t start = 0;
    for(R_xlen_t line = 0; line < n; ++line) {
      size_t end = start;
      while(end < got && buf[end] != '\n') { ++end; }
      if(na_every > 0 && (line + 1) % na_every == 0) {
        b.set_na(line);
      } else {
        const size_t len = end - start;
        const charport_enc enc =
          charport::internal::check_ascii(buf.get() + start, len)
            ? charport_enc::CE_ASCII : charport_enc::CE_UTF8;
        b.set(line, buf.get() + start, len, enc);
      }
      start = end + 1;
    }
    return b.finish();
  } catch(const std::exception & e) {
    Rf_error("read_lines_charvec: %s", e.what());
  }
}

// The status-quo consumer loop: STRING_ELT + R_CHAR + Rf_xlength per
// element. On ALTREP input, STRING_ELT goes through the Elt method.
extern "C" SEXP C_bench_string_elt(SEXP x) {
  const R_xlen_t n = Rf_xlength(x);
  uint64_t h = 14695981039346656037ULL;
  R_xlen_t n_na = 0;
  for(R_xlen_t i = 0; i < n; ++i) {
    SEXP cs = STRING_ELT(x, i);
    if(cs == NA_STRING) { ++n_na; continue; }
    h = fnv1a(R_CHAR(cs), static_cast<uint32_t>(Rf_xlength(cs)), h);
  }
  return hash_to_sexp(h, n_na);
}

// The charport loop: one resolve, then get(state, i) per element.
extern "C" SEXP C_bench_reader(SEXP x) {
  cp::Reader r(x);
  uint64_t h = 14695981039346656037ULL;
  R_xlen_t n_na = 0;
  for(cp::StrView v : r) {
    if(v.is_na()) { ++n_na; continue; }
    h = fnv1a(v.ptr, v.len, h);
  }
  return hash_to_sexp(h, n_na);
}

// Reader split across threads: each worker adopts a copy of the plain-struct
// reader and hashes its own contiguous range; hashes combine by XOR so the
// result is order-independent (not comparable to the serial hash).
extern "C" SEXP C_bench_reader_threaded(SEXP x, SEXP n_threads_) {
  cp::Reader r(x);
  if(!r.reentrant()) Rf_error("reader is not reentrant");
  const int k = Rf_asInteger(n_threads_);
  const R_xlen_t n = r.size();
  std::vector<uint64_t> hashes(static_cast<size_t>(k), 0);
  std::vector<R_xlen_t> nas(static_cast<size_t>(k), 0);
  const charport_reader raw = r.raw();
  std::vector<std::thread> workers;
  for(int t = 0; t < k; ++t) {
    const R_xlen_t lo = n * t / k, hi = n * (t + 1) / k;
    workers.emplace_back([&, t, lo, hi]() {
      cp::Reader wr(raw);
      uint64_t h = 14695981039346656037ULL;
      R_xlen_t n_na = 0;
      for(R_xlen_t i = lo; i < hi; ++i) {
        cp::StrView v = wr[i];
        if(v.is_na()) { ++n_na; continue; }
        h = fnv1a(v.ptr, v.len, h);
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

// Length-sum kernels: nearly free per element, so they expose the cost of
// the access path itself rather than the work done on the bytes.
extern "C" SEXP C_bench_sumlen_elt(SEXP x) {
  const R_xlen_t n = Rf_xlength(x);
  double total = 0;
  for(R_xlen_t i = 0; i < n; ++i) {
    SEXP cs = STRING_ELT(x, i);
    if(cs != NA_STRING) total += static_cast<double>(Rf_xlength(cs));
  }
  return Rf_ScalarReal(total);
}

extern "C" SEXP C_bench_sumlen_reader(SEXP x) {
  cp::Reader r(x);
  double total = 0;
  for(cp::StrView v : r) {
    if(!v.is_na()) total += v.len;
  }
  return Rf_ScalarReal(total);
}

// Construction baseline: mkCharLenCE + SET_STRING_ELT into a fresh STRSXP.
// Input comes through a reader so all construction benches read identically.
// This timing is comparable only while the strings are not already interned
// in R's string cache; benchmark.R runs each repetition in a fresh R session
// (gc alone is not enough: the cache keeps its grown table afterwards).
extern "C" SEXP C_bench_build_strsxp(SEXP x) {
  cp::Reader r(x);
  SEXP out = PROTECT(Rf_allocVector(STRSXP, r.size()));
  for(R_xlen_t i = 0; i < r.size(); ++i) {
    cp::StrView v = r[i];
    if(v.is_na()) { SET_STRING_ELT(out, i, NA_STRING); continue; }
    const cetype_t ce = v.enc == charport_enc::CE_BYTES ? CE_BYTES : CE_UTF8;
    SET_STRING_ELT(out, i, Rf_mkCharLenCE(v.ptr, static_cast<int>(v.len), ce));
  }
  UNPROTECT(1);
  return out;
}

// Construction via the charvec builder; n_threads == 0 means the serial
// convenience path, otherwise one shard per thread over contiguous ranges.
// The builder never touches the string cache (no CHARSXPs), so cache state
// is irrelevant to these timings.
extern "C" SEXP C_bench_build_charvec(SEXP x, SEXP n_threads_) {
  cp::Reader r(x);
  const int k = Rf_asInteger(n_threads_);
  const R_xlen_t n = r.size();
  try {
    cp::charvec::Builder b(n);
    if(k == 0) {
      for(R_xlen_t i = 0; i < n; ++i) {
        cp::StrView v = r[i];
        if(v.is_na()) b.set_na(i); else b.set(i, v);
      }
      return b.finish();
    }
    if(!r.reentrant()) Rf_error("reader is not reentrant");
    std::vector<cp::charvec::BuilderShard> shards;
    for(int t = 0; t < k; ++t) shards.push_back(b.shard());
    const charport_reader raw = r.raw();
    std::vector<std::thread> workers;
    std::vector<std::string> errors(static_cast<size_t>(k));
    for(int t = 0; t < k; ++t) {
      const R_xlen_t lo = n * t / k, hi = n * (t + 1) / k;
      workers.emplace_back([&, t, lo, hi]() {
        try {
          cp::Reader wr(raw);
          const cp::charvec::BuilderShard & sh = shards[static_cast<size_t>(t)];
          for(R_xlen_t i = lo; i < hi; ++i) {
            cp::StrView v = wr[i];
            if(v.is_na()) sh.set_na(i); else sh.set(i, v);
          }
        } catch(const std::exception & e) {
          errors[static_cast<size_t>(t)] = e.what();
        }
      });
    }
    for(std::thread & w : workers) w.join();
    for(const std::string & e : errors) {
      if(!e.empty()) Rf_error("worker: %s", e.c_str());
    }
    return b.finish();
  } catch(const std::exception & e) {
    Rf_error("build_charvec: %s", e.what());
  }
}
