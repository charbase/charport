// Test-only downstream-style consumer of the installed charport headers.

#include "charport.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

R_altrep_class_t release_test_class;
int release_test_count = 0;

struct release_test_state {
  int marker;
};

R_xlen_t release_test_length(SEXP) {
  return 2;
}

SEXP release_test_elt(SEXP, R_xlen_t i) {
  return Rf_mkCharCE(i == 0 ? "alpha" : "beta", CE_UTF8);
}

void * release_test_init(SEXP) {
  return new release_test_state{42};
}

charport_strview release_test_get(void * state, R_xlen_t i) {
  release_test_state * p = static_cast<release_test_state *>(state);
  if(p->marker != 42) {
    return make_strview(nullptr, 0, charport_enc::CE_NA);
  }
  return i == 0 ? make_strview("alpha", 5, charport_enc::CE_ASCII)
                : make_strview("beta", 4, charport_enc::CE_ASCII);
}

void release_test_release(void * state) {
  ++release_test_count;
  delete static_cast<release_test_state *>(state);
}

} // namespace

template<typename Fn>
SEXP test_sexp_guard(const char * op, Fn fn) {
  char msg[512];
  try {
    return fn();
  } catch(const std::exception & e) {
    // snprintf copies the message; Rf_error must run after C++ catch exits.
    std::snprintf(msg, sizeof(msg), "%s", e.what());
  } catch(...) {
    std::snprintf(msg, sizeof(msg), "unknown C++ exception");
  }
  Rf_error("charport consumer %s: %s", op, msg);
  return R_NilValue;
}

template<typename Fn>
bool throws_exception(Fn fn) {
  try {
    fn();
    return false;
  } catch(const std::exception &) {
    return true;
  }
}

cetype_t to_base_encoding(charport_enc enc) noexcept {
  switch(enc) {
  case charport_enc::CE_LATIN1: return CE_LATIN1;
  case charport_enc::CE_BYTES:  return CE_BYTES;
  case charport_enc::CE_UTF8:
  case charport_enc::CE_ASCII_OR_UTF8:
    return CE_UTF8;
  default:
    return CE_NATIVE;
  }
}

SEXP make_charsxp(charport::StrView view) {
  if(view.is_na()) {
    return NA_STRING;
  }
  if(view.len > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("string size exceeds R string size");
  }
  return Rf_mkCharLenCE(view.ptr, static_cast<int>(view.len), to_base_encoding(view.enc));
}

extern "C" {

void R_init_charport_consumer(DllInfo * dll) {
  release_test_class = R_make_altstring_class("release_test", "charport_consumer", dll);
  R_set_altrep_Length_method(release_test_class, release_test_length);
  R_set_altstring_Elt_method(release_test_class, release_test_elt);
}

SEXP C_consumer_abi_ok(void) {
  return Rf_ScalarLogical(charport::check_abi() ? TRUE : FALSE);
}

SEXP C_consumer_register_release_test(void) {
  return test_sexp_guard("register_release_test", []() -> SEXP {
    charport::register_altrep(release_test_class,
                              release_test_init,
                              release_test_get,
                              release_test_release,
                              false,
                              false);
    return R_NilValue;
  });
}

SEXP C_consumer_unregister_release_test(void) {
  charport::unregister_altrep(release_test_class);
  return R_NilValue;
}

SEXP C_consumer_release_test_vector(void) {
  return R_new_altrep(release_test_class, R_NilValue, R_NilValue);
}

SEXP C_consumer_release_test_count(void) {
  return Rf_ScalarInteger(release_test_count);
}

SEXP C_consumer_reader_roundtrip(SEXP x) {
  return test_sexp_guard("reader_roundtrip", [&]() -> SEXP {
    charport::Reader r(x);
    SEXP out = PROTECT(Rf_allocVector(STRSXP, r.size()));
    R_xlen_t i = 0;
    for(charport::StrView s : r) {
      SET_STRING_ELT(out, i++, make_charsxp(s));
    }
    UNPROTECT(1);
    return out;
  });
}

SEXP C_consumer_reader_kind(SEXP x) {
  return test_sexp_guard("reader_kind", [&]() -> SEXP {
    charport::Reader r(x);
    return Rf_ScalarInteger(static_cast<int>(r.kind()));
  });
}

SEXP C_consumer_reader_capabilities(SEXP x) {
  return test_sexp_guard("reader_capabilities", [&]() -> SEXP {
    charport::Reader r(x);
    SEXP out = PROTECT(Rf_allocVector(LGLSXP, 3));
    LOGICAL(out)[0] = r.view_persistence() ? TRUE : FALSE;
    LOGICAL(out)[1] = r.thread_safe_access() ? TRUE : FALSE;
    LOGICAL(out)[2] = r.reentrant() ? TRUE : FALSE;
    UNPROTECT(1);
    return out;
  });
}

SEXP C_consumer_read_scalar(SEXP x) {
  return test_sexp_guard("read_scalar", [&]() -> SEXP {
    return Rf_ScalarString(make_charsxp(charport::Reader::read_scalar(x)));
  });
}

SEXP C_consumer_build_scalar(SEXP x) {
  return test_sexp_guard("build_scalar", [&]() -> SEXP {
    return charport::charvec::build_scalar(charport::Reader::read_scalar(x));
  });
}

SEXP C_consumer_builder_from_reader(SEXP x, SEXP n_shards_) {
  return test_sexp_guard("builder_from_reader", [&]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
    const int k = Rf_asInteger(n_shards_);
    if(k == NA_INTEGER || k < 0) {
      throw std::runtime_error("n_shards must be a non-negative integer");
    }

    if(k == 0) {
      charport::charvec::Builder b(n);
      for(R_xlen_t i = 0; i < n; ++i) {
        b.set(i, r[i]);
      }
      return b.to_sexp();
    }

    charport::charvec::ParallelBuilder b(n, static_cast<size_t>(k));
    for(int j = 0; j < k; ++j) {
      const R_xlen_t lo = n * j / k;
      const R_xlen_t hi = n * (j + 1) / k;
      for(R_xlen_t i = lo; i < hi; ++i) {
        b.set(static_cast<size_t>(j), i, r[i]);
      }
    }
    return b.to_sexp();
  });
}

SEXP C_consumer_builder_reserve(SEXP x, SEXP n_shards_) {
  return test_sexp_guard("builder_reserve", [&]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
    const int k = Rf_asInteger(n_shards_);
    if(k == NA_INTEGER || k < 0) {
      throw std::runtime_error("n_shards must be a non-negative integer");
    }

    if(k == 0) {
      charport::charvec::Builder b(n);
      for(R_xlen_t i = 0; i < n; ++i) {
        const charport::StrView v = r[i];
        if(v.is_na()) { b.set_na(i); continue; }
        char * dst = b.reserve(i, v.len, v.enc);
        if(v.len > 0) { std::memcpy(dst, v.ptr, v.len); }
      }
      return b.to_sexp();
    }

    charport::charvec::ParallelBuilder b(n, static_cast<size_t>(k));
    for(int j = 0; j < k; ++j) {
      const R_xlen_t lo = n * j / k;
      const R_xlen_t hi = n * (j + 1) / k;
      const size_t shard = static_cast<size_t>(j);
      for(R_xlen_t i = lo; i < hi; ++i) {
        const charport::StrView v = r[i];
        if(v.is_na()) { b.set_na(shard, i); continue; }
        char * dst = b.reserve(shard, i, v.len, v.enc);
        if(v.len > 0) { std::memcpy(dst, v.ptr, v.len); }
      }
    }
    return b.to_sexp();
  });
}

SEXP C_consumer_builder_direct(void) {
  return test_sexp_guard("builder_direct", []() -> SEXP {
    charport::charvec::DirectBuilder b(6, 9);
    char * first = b.data_slice();
    std::memcpy(first, "alphabeta", 9);

    char * second = b.allocate_next_slice(5);
    std::memcpy(second, "gamma", 5);

    if(b.data_slice(0) != second || b.data_slice(1) != first) {
      throw std::runtime_error("unexpected direct slice order");
    }
    if(!throws_exception([&]() { (void)b.data_slice(2); })) {
      throw std::runtime_error("out-of-range direct slice did not throw");
    }

    charport::StrView * records = b.records();
    records[0] = charport::StrView{first, 5, charport_enc::CE_ASCII};
    records[1] = charport::StrView{first + 5, 4, charport_enc::CE_ASCII};
    records[2] = charport::StrView{nullptr, 0, charport_enc::CE_NA};
    records[3] = charport::StrView{first, 0, charport_enc::CE_ASCII};
    records[4] = charport::StrView{second, 5, charport_enc::CE_ASCII};
    records[5] = charport::StrView{second, 5, charport_enc::CE_ASCII};

    return b.to_sexp();
  });
}

SEXP C_consumer_builder_errors(void) {
  return test_sexp_guard("builder_errors", [&]() -> SEXP {
    charport::charvec::Builder b(3);

    bool ok = !throws_exception([&]() { b.set(0, "x", 1, charport_enc::CE_LATIN1); });
    ok = ok && !throws_exception([&]() { b.set(0, "x", 1, charport_enc::CE_NATIVE); });
    ok = ok && !throws_exception([&]() { b.set(0, nullptr, 2, charport_enc::CE_UTF8); });
    ok = ok && !throws_exception([&]() { b.set(0, "x", 1, charport_enc::CE_NA); });
    ok = ok && !throws_exception([&]() { b.set(1, nullptr, 0, charport_enc::CE_UTF8); });
    ok = ok && !throws_exception([&]() { b.set_na(2); });
    ok = ok && !throws_exception([&]() { b.set(0, "ok", 2, charport_enc::CE_ASCII); });

    ok = ok && throws_exception([&]() { b.set(3, "x", 1, charport_enc::CE_UTF8); });
    ok = ok && throws_exception([&]() { b.set(-1, "x", 1, charport_enc::CE_UTF8); });

    ok = ok && throws_exception([&]() { (void)b.reserve(3, 1, charport_enc::CE_UTF8); });
    ok = ok && throws_exception([&]() { (void)b.reserve(-1, 1, charport_enc::CE_UTF8); });
    if(ok) {
      char * dst = b.reserve(1, 3, charport_enc::CE_UTF8);
      std::memcpy(dst, "abc", 3);
      char * empty = b.reserve(2, 0, charport_enc::CE_ASCII);  // len 0: valid pointer, no write
      ok = empty != nullptr;
    }

    SEXP out = b.to_sexp();
    ok = ok && TYPEOF(out) == STRSXP && Rf_xlength(out) == 3;

    ok = ok && throws_exception([&]() { charport::charvec::ParallelBuilder bad(4, 0); });
    {
      charport::charvec::ParallelBuilder mt(4, 2);
      ok = ok && throws_exception([&]() { mt.set(2, 0, "x", 1, charport_enc::CE_UTF8); });
      ok = ok && throws_exception([&]() { mt.set(0, 4, "x", 1, charport_enc::CE_UTF8); });
      ok = ok && !throws_exception([&]() { mt.set(0, 0, "x", 1, charport_enc::CE_LATIN1); });
      ok = ok && !throws_exception([&]() { mt.set(0, 0, "a", 1, charport_enc::CE_ASCII); });
    }

    {
      charport::charvec::Builder abandoned(2);
      abandoned.set(0, "z", 1, charport_enc::CE_ASCII);
    }
    {
      charport::charvec::ParallelBuilder abandoned(2, 2);
      abandoned.set(0, 0, "z", 1, charport_enc::CE_ASCII);
    }

    return Rf_ScalarLogical(ok ? TRUE : FALSE);
  });
}

} // extern "C"
