// charport consuming itself through the public header: these hooks use
// cp::Reader, cp::charvec::Builder (serial) and cp::charvec::BuilderMT
// (parallel) exactly as an external consumer would (symbols via
// R_GetCCallable, store compiled from the headers), so the wrapper layer is
// exercised end to end. The BuilderMT shards are driven serially here -- real
// threads live in tests/threaded_consumer.cpp, a separate mini consumer
// compiled at test time so the package itself needs no thread flags.

#include "charvec_altrep.h"
#include "charport_registry.h"

#include "../inst/include/charport.h"

#include <cstring>
#include <vector>

template<typename Fn>
bool throws_exception(Fn fn) {
  try {
    fn();
    return false;
  } catch(const std::exception &) {
    return true;
  }
}

extern "C" {

// Loop a cp::Reader (range-for over the iterator) and rebuild a plain
// character vector: must match C_charport_reader_read_all exactly.
SEXP C_cp_reader_roundtrip(SEXP x) {
  return charport_sexp_guard("cp_reader_roundtrip", [&]() -> SEXP {
    cp::Reader r(x);
    SEXP out = PROTECT(Rf_allocVector(STRSXP, r.size()));
    R_xlen_t i = 0;
    for(cp::StrView s : r) {
      SET_STRING_ELT(out, i++, cpi::make_charsxp(s));
    }
    UNPROTECT(1);
    return out;
  });
}

// The end-to-end interop scenario: read x through the reader, build a new
// charvec through the builder, no CHARSXP creation in between.
// n_shards == 0 drives the serial cp::charvec::Builder; n_shards >= 1 uses
// cp::charvec::BuilderMT, partitioning [0, n) into contiguous disjoint ranges,
// one shard each (driven serially here; threads in tests/threaded_consumer.cpp).
SEXP C_cp_builder_from_reader(SEXP x, SEXP n_shards_) {
  return charport_sexp_guard("cp_builder_from_reader", [&]() -> SEXP {
    cp::Reader r(x);
    const R_xlen_t n = r.size();
    const int k = Rf_asInteger(n_shards_);
    if(k == NA_INTEGER || k < 0) {
      throw std::runtime_error("n_shards must be a non-negative integer");
    }

    if(k == 0) {
      cp::charvec::Builder b(n);
      for(R_xlen_t i = 0; i < n; ++i) {
        b.set(i, r[i]);
      }
      return b.finish();
    }

    cp::charvec::BuilderMT b(n, static_cast<size_t>(k));
    for(int j = 0; j < k; ++j) {
      const R_xlen_t lo = n * j / k;
      const R_xlen_t hi = n * (j + 1) / k;
      for(R_xlen_t i = lo; i < hi; ++i) {
        b.set(static_cast<size_t>(j), i, r[i]);
      }
    }
    return b.finish();
  });
}

// Same rebuild as C_cp_builder_from_reader, but through the zero-copy
// reserve() path: ask the builder for a buffer per element and memcpy the
// reader's bytes straight into it, no set() copy through the builder. n_shards
// == 0 exercises Builder::reserve; >= 1 exercises BuilderMT::reserve over
// contiguous disjoint ranges. (Inputs must carry emittable encodings -- the
// reserve path rejects latin1/native, exactly like set -- so the R test feeds
// it as_charvec content, which is already normalized.)
SEXP C_cp_builder_reserve(SEXP x, SEXP n_shards_) {
  return charport_sexp_guard("cp_builder_reserve", [&]() -> SEXP {
    cp::Reader r(x);
    const R_xlen_t n = r.size();
    const int k = Rf_asInteger(n_shards_);
    if(k == NA_INTEGER || k < 0) {
      throw std::runtime_error("n_shards must be a non-negative integer");
    }

    if(k == 0) {
      cp::charvec::Builder b(n);
      for(R_xlen_t i = 0; i < n; ++i) {
        const cp::StrView v = r[i];
        if(v.is_na()) { b.set_na(i); continue; }
        char * dst = b.reserve(i, v.len, v.enc);
        if(v.len > 0) { std::memcpy(dst, v.ptr, v.len); }
      }
      return b.finish();
    }

    cp::charvec::BuilderMT b(n, static_cast<size_t>(k));
    for(int j = 0; j < k; ++j) {
      const R_xlen_t lo = n * j / k;
      const R_xlen_t hi = n * (j + 1) / k;
      const size_t shard = static_cast<size_t>(j);
      for(R_xlen_t i = lo; i < hi; ++i) {
        const cp::StrView v = r[i];
        if(v.is_na()) { b.set_na(shard, i); continue; }
        char * dst = b.reserve(shard, i, v.len, v.enc);
        if(v.len > 0) { std::memcpy(dst, v.ptr, v.len); }
      }
    }
    return b.finish();
  });
}

// The builder error contract for both builders: no encoding policy (any
// encoding passes through, ptr == NULL is NA), so set/reserve throw only in
// genuinely-broken cases (index out of range, len > INT_MAX); finish is
// single-shot; BuilderMT validates its shard index and n_shards; and a
// thrown-into builder can be abandoned safely (destructor cleans up).
SEXP C_cp_builder_errors(void) {
  return charport_sexp_guard("cp_builder_errors", [&]() -> SEXP {
    cp::charvec::Builder b(3);

    // No encoding policy: latin1/native and any other encoding pass through.
    // ptr == NULL (or CE_NA) is NA regardless of the rest. The builder throws
    // only in genuinely-broken cases: index out of range, len > INT_MAX.
    bool ok = !throws_exception([&]() { b.set(0, "x", 1, charport_enc::CE_LATIN1); });  // stored, not rejected
    ok = ok && !throws_exception([&]() { b.set(0, "x", 1, charport_enc::CE_NATIVE); });
    ok = ok && !throws_exception([&]() { b.set(0, nullptr, 2, charport_enc::CE_UTF8); }); // NULL ptr -> NA
    ok = ok && !throws_exception([&]() { b.set(0, "x", 1, charport_enc::CE_NA); });      // CE_NA -> NA
    ok = ok && !throws_exception([&]() { b.set(1, nullptr, 0, charport_enc::CE_UTF8); }); // NULL, 0 -> NA
    ok = ok && !throws_exception([&]() { b.set_na(2); });
    ok = ok && !throws_exception([&]() { b.set(0, "ok", 2, charport_enc::CE_ASCII); });

    ok = ok && throws_exception([&]() { b.set(3, "x", 1, charport_enc::CE_UTF8); });    // out of bounds
    ok = ok && throws_exception([&]() { b.set(-1, "x", 1, charport_enc::CE_UTF8); });

    // reserve(): out-of-range throws; an in-range call returns a writable buffer
    ok = ok && throws_exception([&]() { (void)b.reserve(3, 1, charport_enc::CE_UTF8); });
    ok = ok && throws_exception([&]() { (void)b.reserve(-1, 1, charport_enc::CE_UTF8); });
    if(ok) {
      char * dst = b.reserve(1, 3, charport_enc::CE_UTF8);
      std::memcpy(dst, "abc", 3);
      char * empty = b.reserve(2, 0, charport_enc::CE_ASCII);  // len 0: valid pointer, no write
      ok = empty != nullptr;
    }

    // single-shot finish; post-finish finish throws
    SEXP out = b.finish();
    ok = ok && TYPEOF(out) == STRSXP && Rf_xlength(out) == 3;
    ok = ok && throws_exception([&]() { b.finish(); });

    // BuilderMT: n_shards must be >= 1, and set() validates the shard index
    ok = ok && throws_exception([&]() { cp::charvec::BuilderMT bad(4, 0); });
    {
      cp::charvec::BuilderMT mt(4, 2);
      ok = ok && throws_exception([&]() { mt.set(2, 0, "x", 1, charport_enc::CE_UTF8); }); // shard OOB
      ok = ok && throws_exception([&]() { mt.set(0, 4, "x", 1, charport_enc::CE_UTF8); }); // element OOB
      ok = ok && !throws_exception([&]() { mt.set(0, 0, "x", 1, charport_enc::CE_LATIN1); }); // latin1 passes through
      ok = ok && !throws_exception([&]() { mt.set(0, 0, "a", 1, charport_enc::CE_ASCII); });
    }

    // abandoning an unfinished builder is safe (destructor frees the store)
    {
      cp::charvec::Builder abandoned(2);
      abandoned.set(0, "z", 1, charport_enc::CE_ASCII);
    }
    {
      cp::charvec::BuilderMT abandoned(2, 2);
      abandoned.set(0, 0, "z", 1, charport_enc::CE_ASCII);
    }

    return Rf_ScalarLogical(ok ? TRUE : FALSE);
  });
}

} // extern "C"
