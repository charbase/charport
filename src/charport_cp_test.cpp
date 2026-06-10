// charport consuming itself through the public header: these hooks use
// cp::Reader and cp::charvec::Builder/BuilderShard exactly as an external
// consumer would (symbols via R_GetCCallable, store compiled from the
// headers), so the wrapper layer is exercised end to end. Shards are driven
// serially here -- real threads live in tests/threaded_consumer.cpp, a
// separate mini consumer compiled at test time so the package itself needs
// no thread flags.

#include "charvec_altrep.h"
#include "charport_registry.h"

#include "../inst/include/charport.hpp"

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
// n_shards == 0 drives the serial Builder::set convenience; n_shards >= 1
// partitions [0, n) into contiguous disjoint ranges, one BuilderShard each
// (driven serially here; threads below).
SEXP C_cp_builder_from_reader(SEXP x, SEXP n_shards_) {
  return charport_sexp_guard("cp_builder_from_reader", [&]() -> SEXP {
    cp::Reader r(x);
    const R_xlen_t n = r.size();
    const int k = Rf_asInteger(n_shards_);
    if(k == NA_INTEGER || k < 0) {
      throw std::runtime_error("n_shards must be a non-negative integer");
    }

    cp::charvec::Builder b(n);
    if(k == 0) {
      for(R_xlen_t i = 0; i < n; ++i) {
        b.set(i, r[i]);
      }
    } else {
      std::vector<cp::charvec::BuilderShard> shards;
      shards.reserve(static_cast<size_t>(k));
      for(int j = 0; j < k; ++j) {
        shards.push_back(b.shard());
      }
      for(int j = 0; j < k; ++j) {
        const R_xlen_t lo = n * j / k;
        const R_xlen_t hi = n * (j + 1) / k;
        for(R_xlen_t i = lo; i < hi; ++i) {
          shards[static_cast<size_t>(j)].set(i, r[i]);
        }
      }
    }
    return b.finish();
  });
}

// The builder error contract: set throws (emission policy, NA convention,
// bounds), finish/shard are single-shot, and a thrown-into builder can be
// abandoned safely (destructor cleans up).
SEXP C_cp_builder_errors(void) {
  return charport_sexp_guard("cp_builder_errors", [&]() -> SEXP {
    cp::charvec::Builder b(3);
    cp::charvec::BuilderShard s = b.shard();

    bool ok = throws_exception([&]() { s.set(0, "x", 1, charport_enc::CE_LATIN1); });   // policy
    ok = ok && throws_exception([&]() { s.set(0, "x", 1, charport_enc::CE_NATIVE); });
    ok = ok && throws_exception([&]() { s.set(3, "x", 1, charport_enc::CE_UTF8); });    // out of bounds
    ok = ok && throws_exception([&]() { s.set(-1, "x", 1, charport_enc::CE_UTF8); });
    ok = ok && throws_exception([&]() { s.set(0, nullptr, 2, charport_enc::CE_UTF8); }); // NULL ptr with bytes
    ok = ok && throws_exception([&]() { s.set(0, "x", 1, charport_enc::CE_NA); });      // CE_NA with bytes
    ok = ok && !throws_exception([&]() { s.set(1, nullptr, 0, charport_enc::CE_UTF8); }); // NULL, 0: NA (ptr wins)
    ok = ok && !throws_exception([&]() { s.set_na(2); });
    ok = ok && !throws_exception([&]() { s.set(0, "ok", 2, charport_enc::CE_ASCII); }); // shard still usable

    // single-shot finish; post-finish use throws; abandoning is safe
    SEXP out = b.finish();
    ok = ok && TYPEOF(out) == STRSXP && Rf_xlength(out) == 3;
    ok = ok && throws_exception([&]() { b.finish(); });
    ok = ok && throws_exception([&]() { (void)b.shard(); });
    {
      cp::charvec::Builder abandoned(2);
      abandoned.set(0, "z", 1, charport_enc::CE_ASCII);
    } // destructor frees the unfinished store

    return Rf_ScalarLogical(ok ? TRUE : FALSE);
  });
}

} // extern "C"
