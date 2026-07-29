// Test-only downstream-style consumer of the installed charport headers.

#include "charport.h"
#include "consumer-boundary.h"

static_assert(
  sizeof(charport::Reader) == sizeof(charport_reader),
  "Reader must not store construction policy state"
);
static_assert(
  std::is_nothrow_default_constructible<charport::Reader>::value,
  "Reader empty construction must not throw"
);
static_assert(
  std::is_nothrow_move_assignable<charport::Reader>::value,
  "Reader move assignment must not throw"
);
static_assert(
  std::is_same<
    decltype(std::declval<charport::Reader &>().reset(std::declval<SEXP>())),
    void
  >::value,
  "Reader::reset() must return void"
);

static_assert(
  std::is_same<
    decltype(std::declval<charport::charvec::Builder &>().release_store()),
    charport::charvec::Store
  >::value,
  "Builder::release_store() must return Store by value"
);
static_assert(
  std::is_same<
    decltype(std::declval<charport::charvec::ParallelBuilder &>().release_store()),
    charport::charvec::Store
  >::value,
  "ParallelBuilder::release_store() must return Store by value"
);
static_assert(
  std::is_same<
    decltype(std::declval<charport::charvec::GrowableBuilder &>().release_store()),
    charport::charvec::Store
  >::value,
  "GrowableBuilder::release_store() must return Store by value"
);

static_assert(
  noexcept(std::declval<charport::charvec::Builder &>().release_store()),
  "Builder::release_store() must not throw"
);
static_assert(
  noexcept(std::declval<charport::charvec::ParallelBuilder &>().release_store()),
  "ParallelBuilder::release_store() must not throw"
);
static_assert(
  noexcept(std::declval<charport::charvec::GrowableBuilder &>().release_store()),
  "GrowableBuilder::release_store() must not throw"
);

static_assert(
  noexcept(std::declval<charport::charvec::Builder &>().to_sexp()),
  "Builder::to_sexp() must not throw C++ exceptions"
);
static_assert(
  noexcept(std::declval<charport::charvec::ParallelBuilder &>().to_sexp()),
  "ParallelBuilder::to_sexp() must not throw C++ exceptions"
);
static_assert(
  noexcept(std::declval<charport::charvec::GrowableBuilder &>().to_sexp()),
  "GrowableBuilder::to_sexp() must not throw C++ exceptions"
);
static_assert(
  noexcept(charport::charvec::wrap(
    std::declval<charport::charvec::Store &&>())),
  "charvec::wrap() must not throw C++ exceptions"
);

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

R_altrep_class_t release_test_class;
int release_test_count = 0;
int release_test_access_counts[8] = {};
int unwind_probe_count = 0;
int release_test_access_status = CHARPORT_STATUS_OK;
SEXP release_test_init_condition = R_NilValue;

struct unwind_probe {
  ~unwind_probe() noexcept { ++unwind_probe_count; }
};

enum release_test_access {
  release_strviews_range = 0,
  release_strviews_index,
  release_byteviews_range,
  release_byteviews_index,
  release_lengths_range,
  release_lengths_index,
  release_encodings_range,
  release_encodings_index
};

struct release_test_state {
  int marker;
  explicit release_test_state(int marker_) noexcept : marker(marker_) {}
};

R_xlen_t release_test_length(SEXP) {
  return 2;
}

SEXP release_test_elt(SEXP, R_xlen_t i) {
  return Rf_mkCharCE(i == 0 ? "alpha" : "beta", CE_UTF8);
}

void * release_test_init(SEXP) {
  if(release_test_init_condition != R_NilValue) {
    SEXP call = PROTECT(Rf_lang2(Rf_install("stop"),
                                 release_test_init_condition));
    Rf_eval(call, R_BaseEnv);
    UNPROTECT(1);
  }
  release_test_state * state = new(std::nothrow) release_test_state(42);
  if(state == nullptr) {
    Rf_error("could not allocate release test state");
  }
  return state;
}

charport_strview release_test_view(void * state, R_xlen_t i) {
  release_test_state * p = static_cast<release_test_state *>(state);
  if(p->marker != 42) {
    return make_strview(nullptr, NA_INTEGER, cetype_ext_t::CE_NA);
  }
  return i == 0 ? make_strview("alpha", 5, cetype_ext_t::CE_ASCII)
                : make_strview("beta", 4, cetype_ext_t::CE_ASCII);
}

void release_test_fill_strview(void * state, R_xlen_t i, const char ** out_ptrs,
                               int * out_lens, cetype_ext_t * out_encs,
                               R_xlen_t out_i) {
  const charport_strview view = release_test_view(state, i);
  out_ptrs[out_i] = view.ptr;
  out_lens[out_i] = view.len;
  out_encs[out_i] = view.enc;
}

void release_test_fill_byteview(void * state, R_xlen_t i, const char ** out_ptrs,
                                int * out_lens, R_xlen_t out_i) {
  const charport_strview view = release_test_view(state, i);
  out_ptrs[out_i] = view.ptr;
  out_lens[out_i] = view.len;
}

int release_test_access_result() {
  return release_test_access_status;
}

int release_test_strviews_range(
    void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
    int * out_lens, cetype_ext_t * out_encs) {
  ++release_test_access_counts[release_strviews_range];
  for(R_xlen_t j = 0; j < size; ++j) {
    release_test_fill_strview(state, start + j, out_ptrs, out_lens, out_encs, j);
  }
  return release_test_access_result();
}

int release_test_strviews_index(
    void * state, const R_xlen_t * indices, R_xlen_t size,
    const char ** out_ptrs, int * out_lens, cetype_ext_t * out_encs) {
  ++release_test_access_counts[release_strviews_index];
  for(R_xlen_t j = 0; j < size; ++j) {
    release_test_fill_strview(state, indices[j], out_ptrs, out_lens, out_encs, j);
  }
  return release_test_access_result();
}

int release_test_byteviews_range(
    void * state, R_xlen_t start, R_xlen_t size, const char ** out_ptrs,
    int * out_lens) {
  ++release_test_access_counts[release_byteviews_range];
  for(R_xlen_t j = 0; j < size; ++j) {
    release_test_fill_byteview(state, start + j, out_ptrs, out_lens, j);
  }
  return release_test_access_result();
}

int release_test_byteviews_index(
    void * state, const R_xlen_t * indices, R_xlen_t size,
    const char ** out_ptrs, int * out_lens) {
  ++release_test_access_counts[release_byteviews_index];
  for(R_xlen_t j = 0; j < size; ++j) {
    release_test_fill_byteview(state, indices[j], out_ptrs, out_lens, j);
  }
  return release_test_access_result();
}

int release_test_lengths_range(
    void * state, R_xlen_t start, R_xlen_t size, int * out_lens) {
  ++release_test_access_counts[release_lengths_range];
  for(R_xlen_t j = 0; j < size; ++j) {
    out_lens[j] = release_test_view(state, start + j).len;
  }
  return release_test_access_result();
}

int release_test_lengths_index(
    void * state, const R_xlen_t * indices, R_xlen_t size, int * out_lens) {
  ++release_test_access_counts[release_lengths_index];
  for(R_xlen_t j = 0; j < size; ++j) {
    out_lens[j] = release_test_view(state, indices[j]).len;
  }
  return release_test_access_result();
}

int release_test_encodings_range(
    void * state, R_xlen_t start, R_xlen_t size, cetype_ext_t * out_encs) {
  ++release_test_access_counts[release_encodings_range];
  for(R_xlen_t j = 0; j < size; ++j) {
    out_encs[j] = release_test_view(state, start + j).enc;
  }
  return release_test_access_result();
}

int release_test_encodings_index(
    void * state, const R_xlen_t * indices, R_xlen_t size,
    cetype_ext_t * out_encs) {
  ++release_test_access_counts[release_encodings_index];
  for(R_xlen_t j = 0; j < size; ++j) {
    out_encs[j] = release_test_view(state, indices[j]).enc;
  }
  return release_test_access_result();
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
SEXP test_entrypoint_boundary(const char * op, Fn fn) {
  return charport_consumer::boundary(op, fn);
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

cetype_t to_base_encoding(cetype_ext_t enc) noexcept {
  switch(enc) {
  case cetype_ext_t::CE_LATIN1: return CE_LATIN1;
  case cetype_ext_t::CE_BYTES:  return CE_BYTES;
  case cetype_ext_t::CE_UTF8:
  case cetype_ext_t::CE_ASCII_OR_UTF8:
    return CE_UTF8;
  default:
    return CE_NATIVE;
  }
}

SEXP make_charsxp(charport::StrView view) {
  if(view.is_na()) {
    return NA_STRING;
  }
  if(view.len < 0 || view.len > std::numeric_limits<int>::max()) {
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
    charport::register_altrep(
      release_test_class,
      charport_reader_state_fns{release_test_init, release_test_release},
      charport_reader_range_fns{
        release_test_strviews_range,
        release_test_byteviews_range,
        release_test_lengths_range,
        release_test_encodings_range
      },
      charport_reader_index_fns{
        release_test_strviews_index,
        release_test_byteviews_index,
        release_test_lengths_index,
        release_test_encodings_index
      },
      charport_reader_capabilities{false, false}
    );
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

SEXP C_consumer_unwind_probe_count(void) {
  return Rf_ScalarInteger(unwind_probe_count);
}

SEXP C_consumer_unwind_probe_reset(void) {
  unwind_probe_count = 0;
  return R_NilValue;
}

SEXP C_consumer_release_test_init_condition(SEXP condition) {
  if(release_test_init_condition != R_NilValue) {
    R_ReleaseObject(release_test_init_condition);
  }
  release_test_init_condition = condition;
  if(release_test_init_condition != R_NilValue) {
    R_PreserveObject(release_test_init_condition);
  }
  return R_NilValue;
}

SEXP C_consumer_release_test_access_status(SEXP value) {
  release_test_access_status = Rf_asInteger(value);
  return R_NilValue;
}

SEXP C_consumer_release_test_reset_access_counts(void) {
  for(int & count : release_test_access_counts) {
    count = 0;
  }
  return R_NilValue;
}

SEXP C_consumer_release_test_access_counts(void) {
  const char * names[] = {
    "strviews_range", "strviews_index",
    "byteviews_range", "byteviews_index",
    "lengths_range", "lengths_index",
    "encodings_range", "encodings_index",
    ""
  };
  SEXP out = PROTECT(Rf_mkNamed(INTSXP, names));
  for(int i = 0; i < 8; ++i) {
    INTEGER(out)[i] = release_test_access_counts[i];
  }
  UNPROTECT(1);
  return out;
}

SEXP C_consumer_reader_roundtrip(SEXP x) {
  return test_entrypoint_boundary("reader_roundtrip", [&]() -> SEXP {
    charport::Reader r(x);
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(STRSXP, r.size()));
      R_xlen_t i = 0;
      for(charport::StrView s : r) {
        SET_STRING_ELT(out, i++, make_charsxp(s));
      }
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_resolved_reader_roundtrip(SEXP x) {
  return test_entrypoint_boundary("resolved_reader_roundtrip", [&]() -> SEXP {
    charport::Reader r;
    charport_consumer::unwind_protect([&]() -> SEXP {
      r.reset(x);
      return R_NilValue;
    });
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(STRSXP, r.size()));
      R_xlen_t i = 0;
      for(charport::StrView s : r) {
        SET_STRING_ELT(out, i++, make_charsxp(s));
      }
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_reader_range_roundtrip(SEXP x) {
  return test_entrypoint_boundary("reader_range_roundtrip", [&]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
    charport::StrViews views(n);
    if(n > 0) {
      r.views(0, n, views);
    }
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(STRSXP, n));
      for(R_xlen_t i = 0; i < n; ++i) {
        SET_STRING_ELT(out, i, make_charsxp(views[i]));
      }
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_reader_index_roundtrip(SEXP x) {
  return test_entrypoint_boundary("reader_index_roundtrip", [&]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
    std::vector<R_xlen_t> idx(static_cast<size_t>(n));
    for(R_xlen_t i = 0; i < n; ++i) {
      idx[static_cast<size_t>(i)] = n - i - 1;
    }

    charport::StrViews views(n);
    if(n > 0) {
      r.views(idx.data(), n, views);
    }
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(STRSXP, n));
      for(R_xlen_t i = 0; i < n; ++i) {
        SET_STRING_ELT(out, i, make_charsxp(views[i]));
      }
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_reader_capabilities(SEXP x) {
  return test_entrypoint_boundary("reader_capabilities", [&]() -> SEXP {
    charport::Reader r(x);
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(LGLSXP, 3));
      LOGICAL(out)[0] = r.persistent_views() ? TRUE : FALSE;
      LOGICAL(out)[1] = r.concurrent_access() ? TRUE : FALSE;
      LOGICAL(out)[2] = r.reentrant() ? TRUE : FALSE;
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_reader_lengths(SEXP x) {
  return test_entrypoint_boundary("reader_lengths", [&]() -> SEXP {
    charport::Reader r(x);
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(INTSXP, r.size()));
      for(R_xlen_t i = 0; i < r.size(); ++i) {
        const int len = r.length(i);
        INTEGER(out)[i] = len < 0 ? NA_INTEGER : len;
      }
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_reader_range_lengths(SEXP x) {
  return test_entrypoint_boundary("reader_range_lengths", [&]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
    std::vector<int> len(static_cast<size_t>(n));
    if(n > 0) {
      r.lengths(0, n, len.data());
    }
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(INTSXP, n));
      for(R_xlen_t i = 0; i < n; ++i) {
        const int x = len[static_cast<size_t>(i)];
        INTEGER(out)[i] = x < 0 ? NA_INTEGER : x;
      }
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_reader_index_lengths(SEXP x) {
  return test_entrypoint_boundary("reader_index_lengths", [&]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
    std::vector<R_xlen_t> idx(static_cast<size_t>(n));
    for(R_xlen_t i = 0; i < n; ++i) {
      idx[static_cast<size_t>(i)] = n - i - 1;
    }
    std::vector<int> len(static_cast<size_t>(n));
    if(n > 0) {
      r.lengths(idx.data(), n, len.data());
    }
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(INTSXP, n));
      for(R_xlen_t i = 0; i < n; ++i) {
        const int x = len[static_cast<size_t>(i)];
        INTEGER(out)[i] = x < 0 ? NA_INTEGER : x;
      }
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_reader_byte_lengths(SEXP x) {
  return test_entrypoint_boundary("reader_byte_lengths", [&]() -> SEXP {
    charport::Reader r(x);
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(INTSXP, r.size()));
      for(R_xlen_t i = 0; i < r.size(); ++i) {
        const charport::ByteView view = r.byteview(i);
        INTEGER(out)[i] = view.is_na() ? NA_INTEGER : static_cast<int>(view.len);
      }
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_reader_encodings(SEXP x) {
  return test_entrypoint_boundary("reader_encodings", [&]() -> SEXP {
    charport::Reader r(x);
    return charport_consumer::unwind_protect([&]() -> SEXP {
      SEXP out = PROTECT(Rf_allocVector(INTSXP, r.size()));
      for(R_xlen_t i = 0; i < r.size(); ++i) {
        INTEGER(out)[i] = static_cast<int>(r.encoding(i));
      }
      UNPROTECT(1);
      return out;
    });
  });
}

SEXP C_consumer_reader_touch_all_access_paths(SEXP x) {
  return test_entrypoint_boundary("reader_touch_all_access_paths",
                         [&]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
    std::vector<R_xlen_t> idx(static_cast<size_t>(n));
    for(R_xlen_t i = 0; i < n; ++i) {
      idx[static_cast<size_t>(i)] = n - i - 1;
    }
    charport::StrViews strviews(n);
    charport::ByteViews byteviews(n);
    std::vector<int> lens(static_cast<size_t>(n));
    std::vector<cetype_ext_t> encs(static_cast<size_t>(n));
    r.views(0, n, strviews);
    r.views(idx.data(), n, strviews);
    r.byteviews(0, n, byteviews);
    r.byteviews(idx.data(), n, byteviews);
    r.lengths(0, n, lens.data());
    r.lengths(idx.data(), n, lens.data());
    r.encodings(0, n, encs.data());
    r.encodings(idx.data(), n, encs.data());
    return R_NilValue;
  });
}

SEXP C_consumer_reader_access_kind(SEXP x, SEXP which_) {
  const int which = Rf_asInteger(which_);
  return test_entrypoint_boundary("reader_access_kind", [&, which]() -> SEXP {
    charport::Reader reader(x);
    int kind = 0;
    try {
      const R_xlen_t index = 0;
      const char * ptr = nullptr;
      int len = NA_INTEGER;
      cetype_ext_t encoding = cetype_ext_t::CE_NA;
      switch(which) {
      case 0:
        reader.views(static_cast<R_xlen_t>(0), 1, &ptr, &len, &encoding);
        break;
      case 1:
        reader.views(&index, 1, &ptr, &len, &encoding);
        break;
      case 2:
        reader.byteviews(static_cast<R_xlen_t>(0), 1, &ptr, &len);
        break;
      case 3:
        reader.byteviews(&index, 1, &ptr, &len);
        break;
      case 4:
        reader.lengths(static_cast<R_xlen_t>(0), 1, &len);
        break;
      case 5:
        reader.lengths(&index, 1, &len);
        break;
      case 6:
        reader.encodings(static_cast<R_xlen_t>(0), 1, &encoding);
        break;
      case 7:
        reader.encodings(&index, 1, &encoding);
        break;
      case 8:
        (void)*reader.begin();
        break;
      default:
        throw std::runtime_error("unknown access path");
      }
    } catch(const std::bad_alloc &) {
      kind = 2;
    } catch(const std::out_of_range &) {
      kind = 3;
    } catch(const std::runtime_error &) {
      kind = 1;
    }
    return charport_consumer::unwind_protect(
      [kind]() -> SEXP { return Rf_ScalarInteger(kind); }
    );
  });
}

SEXP C_consumer_reader_access_throw(SEXP x) {
  return test_entrypoint_boundary("reader_access_throw", [&]() -> SEXP {
    charport::Reader reader(x);
    (void)reader.view(0);
    return R_NilValue;
  });
}

SEXP C_consumer_reader_access_recovers(SEXP x) {
  return test_entrypoint_boundary("reader_access_recovers", [&]() -> SEXP {
    charport::Reader reader(x);
    release_test_access_status = CHARPORT_STATUS_ERROR;
    bool failed = false;
    try {
      (void)reader.length(0);
    } catch(const std::runtime_error &) {
      failed = true;
    } catch(...) {
      release_test_access_status = CHARPORT_STATUS_OK;
      throw;
    }
    release_test_access_status = CHARPORT_STATUS_OK;
    const bool recovered = failed && reader.length(0) == 5;
    return charport_consumer::unwind_protect(
      [recovered]() -> SEXP {
        return Rf_ScalarLogical(recovered ? TRUE : FALSE);
      }
    );
  });
}

SEXP C_consumer_convert_current_exception_to_status(SEXP which_) {
  const int which = Rf_asInteger(which_);
  int status = CHARPORT_STATUS_OK;
  try {
    switch(which) {
    case 0:
      break;
    case 1:
      throw std::runtime_error("runtime failure");
    case 2:
      throw std::bad_alloc();
    case 3:
      throw std::out_of_range("range failure");
    default:
      throw 1;
    }
  } catch(...) {
    status = charport::convert_current_exception_to_status();
  }
  return Rf_ScalarInteger(static_cast<int>(status));
}

SEXP C_consumer_sexp_info(SEXP x) {
  return test_sexp_guard("sexp_info", [&]() -> SEXP {
    charport::SexpInfo info = charport::sexp_info(x);
    const char * names[] = {
      "is_strsxp", "length", "is_altrep", "is_materialized", "is_registered",
      "persistent_views", "concurrent_access", "stateful_reader", "reentrant",
      "has_class_name", "has_class_package", ""
    };
    SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
    SET_VECTOR_ELT(out, 0, Rf_ScalarLogical(info.is_strsxp ? TRUE : FALSE));
    SET_VECTOR_ELT(out, 1, Rf_ScalarReal(static_cast<double>(info.length)));
    SET_VECTOR_ELT(out, 2, Rf_ScalarLogical(info.is_altrep ? TRUE : FALSE));
    SET_VECTOR_ELT(out, 3, Rf_ScalarLogical(info.is_materialized ? TRUE : FALSE));
    SET_VECTOR_ELT(out, 4, Rf_ScalarLogical(info.is_registered ? TRUE : FALSE));
    SET_VECTOR_ELT(out, 5, Rf_ScalarLogical(info.persistent_views ? TRUE : FALSE));
    SET_VECTOR_ELT(out, 6, Rf_ScalarLogical(info.concurrent_access ? TRUE : FALSE));
    SET_VECTOR_ELT(out, 7, Rf_ScalarLogical(info.stateful_reader ? TRUE : FALSE));
    SET_VECTOR_ELT(out, 8, Rf_ScalarLogical(info.reentrant() ? TRUE : FALSE));
    SET_VECTOR_ELT(out, 9, Rf_ScalarLogical(info.altrep_class_name == nullptr ? FALSE : TRUE));
    SET_VECTOR_ELT(out, 10, Rf_ScalarLogical(info.altrep_class_package == nullptr ? FALSE : TRUE));
    UNPROTECT(1);
    return out;
  });
}

SEXP C_consumer_read_scalar(SEXP x) {
  return test_entrypoint_boundary("read_scalar", [&]() -> SEXP {
    charport::Reader r(x);
    if(r.size() < 1) {
      throw std::runtime_error("x must have length at least 1");
    }
    return charport_consumer::unwind_protect([&]() -> SEXP {
      return Rf_ScalarString(make_charsxp(r.view(0)));
    });
  });
}

SEXP C_consumer_build_scalar(SEXP x) {
  return test_entrypoint_boundary("build_scalar", [&]() -> SEXP {
    charport::Reader r(x);
    if(r.size() < 1) {
      throw std::runtime_error("x must have length at least 1");
    }
    const charport::StrView value = r.view(0);
    const size_t len = value.is_na() ? 0 : static_cast<size_t>(value.len);
    charport::charvec::Store store =
      charport::charvec::Store::scalar(value.ptr, len, value.enc);
    return charport::charvec::wrap(std::move(store));
  });
}

SEXP C_consumer_builder_from_reader(SEXP x, SEXP n_shards_) {
  const int k = Rf_asInteger(n_shards_);
  return test_entrypoint_boundary("builder_from_reader", [&, k]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
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
  const int k = Rf_asInteger(n_shards_);
  return test_entrypoint_boundary("builder_reserve", [&, k]() -> SEXP {
    charport::Reader r(x);
    const R_xlen_t n = r.size();
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
  return test_entrypoint_boundary("builder_direct", []() -> SEXP {
    charport::charvec::Store store(6, 9);
    char * first = store.slices.front_data();
    std::memcpy(first, "alphabeta", 9);

    char * second = store.slices.push_front(5);
    std::memcpy(second, "gamma", 5);

    if(store.slices.data_slice(0) != second || store.slices.data_slice(1) != first) {
      throw std::runtime_error("unexpected direct slice order");
    }
    if(store.slices.data_slice(2) != nullptr) {
      throw std::runtime_error("out-of-range direct slice was not null");
    }

    store.records.set(0, first, 5, cetype_ext_t::CE_ASCII);
    store.records.set(1, first + 5, 4, cetype_ext_t::CE_ASCII);
    store.records.set_na(2);
    store.records.set(3, first, 0, cetype_ext_t::CE_ASCII);
    store.records.set(4, second, 5, cetype_ext_t::CE_ASCII);
    store.records.set(5, second, 5, cetype_ext_t::CE_ASCII);

    return charport::charvec::wrap(std::move(store));
  });
}

SEXP C_consumer_growable_from_reader(SEXP x) {
  return test_entrypoint_boundary("growable_from_reader", [&]() -> SEXP {
    charport::Reader reader(x);
    charport::charvec::GrowableBuilder builder;
    if(builder.size() != 0) {
      throw std::runtime_error("new growable builder is not empty");
    }

    for(R_xlen_t i = 0; i < reader.size(); ++i) {
      const charport::StrView value = reader.view(i);
      switch(i % 3) {
      case 0:
        builder.append(value);
        break;
      case 1:
        builder.append(
          value.ptr, value.is_na() ? 0 : static_cast<size_t>(value.len), value.enc);
        break;
      default: {
        const size_t len = value.is_na() ? 0 : static_cast<size_t>(value.len);
        char * dest = builder.append_reserve(len, value.enc);
        if(value.is_na()) {
          if(dest != nullptr) {
            throw std::runtime_error("NA append_reserve returned storage");
          }
        } else if(len > 0) {
          std::memcpy(dest, value.ptr, len);
        } else if(dest == nullptr) {
          throw std::runtime_error("empty append_reserve returned null");
        }
        break;
      }
      }

      if(builder.size() != static_cast<size_t>(i + 1)) {
        throw std::runtime_error("unexpected growable builder size");
      }
    }

    charport::charvec::Store store = builder.release_store();
    return charport::charvec::wrap(std::move(store));
  });
}

SEXP C_consumer_growable_state(void) {
  return test_entrypoint_boundary("growable_state", []() -> SEXP {
    typedef charport::charvec::components::RecordTable RecordTable;
    bool ok = true;

    RecordTable records;
    ok = ok && records.size() == 0 && records.capacity() == 0;
    for(size_t i = 0; i < 257; ++i) {
      if(i % 3 == 0) {
        records.push_back(charport::charvec::components::na_record());
      } else if(i % 3 == 1) {
        records.push_back(
          charport::charvec::components::empty_record(cetype_ext_t::CE_ASCII));
      } else {
        records.push_back(make_strview("x", 1, cetype_ext_t::CE_ASCII));
      }
    }
    ok = ok && records.size() == 257 && records.capacity() > records.size();

    const size_t size_before = records.size();
    const size_t capacity_before = records.capacity();
    ok = ok && throws_exception([&]() {
      records.reserve(std::numeric_limits<size_t>::max());
    });
    ok = ok && records.size() == size_before;
    ok = ok && records.capacity() == capacity_before;

    RecordTable moved_records(std::move(records));
    ok = ok && records.size() == 0 && records.capacity() == 0;
    ok = ok && moved_records.size() == 257;
    ok = ok && moved_records.capacity() == capacity_before;
    for(size_t i = 0; i < moved_records.size(); ++i) {
      const charport_strview value = moved_records.view(i);
      if(i % 3 == 0) {
        ok = ok && value.is_na();
      } else if(i % 3 == 1) {
        ok = ok && !value.is_na() && value.len == 0;
      } else {
        ok = ok && value.len == 1 && value.ptr[0] == 'x';
      }
    }
    records.push_back(make_strview("r", 1, cetype_ext_t::CE_ASCII));
    ok = ok && records.size() == 1 && records.view(0).ptr[0] == 'r';

    charport::charvec::GrowableBuilder source;
    for(size_t i = 0; i < 257; ++i) {
      source.append("x", 1, cetype_ext_t::CE_ASCII);
    }
    charport::charvec::GrowableBuilder builder(std::move(source));
    ok = ok && source.size() == 0 && builder.size() == 257;

    charport::charvec::Store first = builder.release_store();
    ok = ok && builder.size() == 0 && first.size() == 257;
    ok = ok && first.records.capacity() > first.records.size();
    ok = ok && first.slices.count() > 1;
    for(size_t i = 0; i < first.size(); ++i) {
      const charport_strview value = first.view(i);
      ok = ok && value.len == 1 && value.ptr[0] == 'x';
    }

    builder.append("z", 1, cetype_ext_t::CE_ASCII);
    charport::charvec::Store second = builder.release_store();
    ok = ok && builder.size() == 0 && second.size() == 1;
    ok = ok && second.view(0).len == 1 && second.view(0).ptr[0] == 'z';
    ok = ok && first.size() == 257 && first.view(0).ptr[0] == 'x';

    charport::charvec::GrowableBuilder no_payload;
    no_payload.append(nullptr, 0, cetype_ext_t::CE_NA);
    no_payload.append("", 0, cetype_ext_t::CE_ASCII);
    charport::charvec::Store no_payload_store = no_payload.release_store();
    ok = ok && no_payload_store.size() == 2;
    ok = ok && no_payload_store.slices.empty();

    charport::charvec::Builder fixed(257);
    charport::charvec::Store fixed_store = fixed.release_store();
    ok = ok && fixed_store.records.capacity() == fixed_store.records.size();

    return charport_consumer::unwind_protect([ok]() -> SEXP {
      return Rf_ScalarLogical(ok ? TRUE : FALSE);
    });
  });
}

SEXP C_consumer_builder_errors(void) {
  return test_entrypoint_boundary("builder_errors", [&]() -> SEXP {
    charport::charvec::Builder b(3);

    bool ok = !throws_exception([&]() { b.set(0, "x", 1, cetype_ext_t::CE_LATIN1); });
    ok = ok && !throws_exception([&]() { b.set(0, "x", 1, cetype_ext_t::CE_NATIVE); });
    ok = ok && !throws_exception([&]() { b.set(0, nullptr, 2, cetype_ext_t::CE_UTF8); });
    ok = ok && !throws_exception([&]() { b.set(0, "x", 1, cetype_ext_t::CE_NA); });
    ok = ok && !throws_exception([&]() { b.set(1, nullptr, 0, cetype_ext_t::CE_UTF8); });
    ok = ok && !throws_exception([&]() { b.set_na(2); });
    ok = ok && !throws_exception([&]() { b.set(0, "ok", 2, cetype_ext_t::CE_ASCII); });

    ok = ok && throws_exception([&]() { b.set(3, "x", 1, cetype_ext_t::CE_UTF8); });
    ok = ok && throws_exception([&]() { b.set(-1, "x", 1, cetype_ext_t::CE_UTF8); });

    ok = ok && throws_exception([&]() { (void)b.reserve(3, 1, cetype_ext_t::CE_UTF8); });
    ok = ok && throws_exception([&]() { (void)b.reserve(-1, 1, cetype_ext_t::CE_UTF8); });
    if(ok) {
      char * dst = b.reserve(1, 3, cetype_ext_t::CE_UTF8);
      std::memcpy(dst, "abc", 3);
      char * empty = b.reserve(2, 0, cetype_ext_t::CE_ASCII);  // len 0: valid pointer, no write
      ok = empty != nullptr;
    }

    SEXP out = b.to_sexp();
    ok = ok && TYPEOF(out) == STRSXP && Rf_xlength(out) == 3;

    ok = ok && throws_exception([&]() { charport::charvec::ParallelBuilder bad(4, 0); });
    {
      charport::charvec::ParallelBuilder mt(4, 2);
      ok = ok && throws_exception([&]() { mt.set(2, 0, "x", 1, cetype_ext_t::CE_UTF8); });
      ok = ok && throws_exception([&]() { mt.set(0, 4, "x", 1, cetype_ext_t::CE_UTF8); });
      ok = ok && !throws_exception([&]() { mt.set(0, 0, "x", 1, cetype_ext_t::CE_LATIN1); });
      ok = ok && !throws_exception([&]() { mt.set(0, 0, "a", 1, cetype_ext_t::CE_ASCII); });
    }

    {
      charport::charvec::Builder abandoned(2);
      abandoned.set(0, "z", 1, cetype_ext_t::CE_ASCII);
    }
    {
      charport::charvec::ParallelBuilder abandoned(2, 2);
      abandoned.set(0, 0, "z", 1, cetype_ext_t::CE_ASCII);
    }

    return charport_consumer::unwind_protect([&]() -> SEXP {
      return Rf_ScalarLogical(ok ? TRUE : FALSE);
    });
  });
}

SEXP C_consumer_reader_eval(SEXP x, SEXP expression, SEXP environment) {
  return test_entrypoint_boundary("reader_eval", [&]() -> SEXP {
    charport::Reader reader(x);
    unwind_probe probe;
    charport::charvec::Builder builder(reader.size());
    for(R_xlen_t i = 0; i < reader.size(); ++i) {
      builder.set(i, reader[i]);
    }

    return charport_consumer::unwind_protect([&]() -> SEXP {
      return Rf_eval(expression, environment);
    });
  });
}

SEXP C_consumer_reader_open(SEXP x) {
  charport::Reader reader(x);
  return R_NilValue;
}

SEXP C_consumer_reader_open_protected(SEXP first, SEXP second) {
  return test_entrypoint_boundary("reader_open_protected", [&]() -> SEXP {
    charport::Reader reader;
    unwind_probe probe;
    charport_consumer::unwind_protect([&]() -> SEXP {
      reader.reset(first);
      reader.reset(second);
      return R_NilValue;
    });
    return R_NilValue;
  });
}

SEXP C_consumer_context_cpp_error(void) {
  return test_entrypoint_boundary("context_cpp_error", []() -> SEXP {
    unwind_probe probe;
    return charport_consumer::unwind_protect([]() -> SEXP {
      throw std::runtime_error("injected context C++ error");
    });
  });
}

// C consumers routinely gather STRING_ELT results into C containers (which
// R's GC cannot see) before storing them, relying on the source vector to
// keep every element alive — true of any ordinary STRSXP. string_Elt must
// therefore never hand out an unrooted CHARSXP. Run under gctorture to make
// a violation deterministic.
SEXP C_consumer_elt_hold_across_alloc(SEXP x) {
  const R_xlen_t n = Rf_xlength(x);
  std::vector<SEXP> held(static_cast<size_t>(n));
  for (R_xlen_t i = 0; i < n; ++i) {
    held[static_cast<size_t>(i)] = STRING_ELT(x, i);
  }
  SEXP out = PROTECT(Rf_allocVector(STRSXP, n)); // GC may run here
  for (R_xlen_t i = 0; i < n; ++i) {
    SET_STRING_ELT(out, i, held[static_cast<size_t>(i)]);
  }
  UNPROTECT(1);
  return out;
}

} // extern "C"
