// White-box test helpers; compiled only by tests/helpers/internal_calls.R.

#include "charpack_altrep.h"

#include "consumer-boundary.h"

#include <cmath>

R_altrep_class_t charvec_altrep::class_t;

R_altrep_class_t charpack_altrep::class_t;

extern "C" int charport_native_is_utf8_impl(void) {

  return charport::native_is_utf8() ? 1 : 0;

}

bool is_charvec_sexp(SEXP x) {
  return R_altrep_inherits(x, charvec_altrep::class_t) == TRUE;
}

cpv::Store & checked_store(SEXP x) {
  if(!is_charvec_sexp(x)) {
    throw std::runtime_error("x must be a charvec");
  }
  if(R_altrep_data2(x) != R_NilValue) {
    throw std::runtime_error("charvec is materialized; its store has been released");
  }
  auto * ptr = charvec_altrep::Ptr(x);
  if(ptr == nullptr) {
    throw std::runtime_error("charvec store pointer is null");
  }
  return *ptr;
}

size_t checked_len_arg(SEXP n_) {
  const double n = Rf_asReal(n_);
  if(ISNAN(n) || n < 0 ||
     n > static_cast<double>(std::numeric_limits<R_xlen_t>::max())) {
    throw std::runtime_error("invalid length");
  }
  return static_cast<size_t>(n);
}

extern "C" SEXP C_charvec_alloc(SEXP n_) {
  return charport_sexp_guard("charvec_alloc", [&]() -> SEXP {
    const size_t n = checked_len_arg(n_);
    return charport::charvec::alloc_na(static_cast<R_xlen_t>(n));
  });
}

extern "C" SEXP C_charvec_stats(SEXP x) {
  return charport_sexp_guard("charvec_stats", [&]() -> SEXP {
    if(!is_charvec_sexp(x)) {
      throw std::runtime_error("x must be a charvec");
    }
    const bool materialized = R_altrep_data2(x) != R_NilValue;
    const char * names[] = {"length", "n_slices", "materialized", ""};
    SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
    if(materialized) {
      SET_VECTOR_ELT(out, 0, Rf_ScalarReal(static_cast<double>(Rf_xlength(R_altrep_data2(x)))));
      SET_VECTOR_ELT(out, 1, Rf_ScalarReal(NA_REAL));
    } else {
      auto & store = checked_store(x);
      SET_VECTOR_ELT(out, 0, Rf_ScalarReal(static_cast<double>(store.size())));
      SET_VECTOR_ELT(out, 1, Rf_ScalarReal(static_cast<double>(store.slices.count())));
    }
    SET_VECTOR_ELT(out, 2, Rf_ScalarLogical(materialized ? TRUE : FALSE));
    UNPROTECT(1);
    return out;
  });
}

extern "C" SEXP C_charvec_assign(SEXP x, SEXP i_, SEXP value) {
  return charport_sexp_guard("charvec_assign", [&]() -> SEXP {
    if(!is_charvec_sexp(x)) {
      throw std::runtime_error("x must be a charvec");
    }
    if(TYPEOF(value) != STRSXP || Rf_xlength(value) != 1) {
      throw std::runtime_error("value must be a character vector of length 1");
    }
    const double i = Rf_asReal(i_);
    const R_xlen_t len = charvec_altrep::Length(x);
    if(ISNAN(i) || i < 1 || i > static_cast<double>(len)) {
      throw std::runtime_error("index out of bounds");
    }
    SET_STRING_ELT(x, static_cast<R_xlen_t>(i) - 1, STRING_ELT(value, 0));
    return x;
  });
}

extern "C" SEXP C_charvec_compact(SEXP x) {
  return charport_sexp_guard("charvec_compact", [&]() -> SEXP {
    cpv::Store & store = checked_store(x);
    store = charvec_detail::rebuild_store(store.records);
    return x;
  });
}

extern "C" SEXP C_charpack_placeholder(void) {
  return charport::charpack::placeholder();
}

extern "C" SEXP C_charpack_fill_materialized_na(SEXP x, SEXP n) {
  return charport::charpack::fill_materialized_na(x, static_cast<R_xlen_t>(Rf_asReal(n)));
}

extern "C" SEXP C_charpack_state(SEXP x) {
  if(!charpack_altrep::is(x)) {
    return Rf_ScalarString(NA_STRING);
  }
  return Rf_mkString(charpack_altrep::state_name(x));
}

extern "C" SEXP C_charpack_data1(SEXP x) {
  if(!charpack_altrep::is(x)) {
    return R_NilValue;
  }
  return R_altrep_data1(x);
}

extern "C" SEXP C_charpack_serialized_state(SEXP x) {
  if(!charpack_altrep::is(x) || R_altrep_data1(x) == R_NilValue) {
    Rf_error("charport charpack serialized_state: x must be a filled charpack");
  }
  return charpack_altrep::Serialized_state(x);
}

extern "C" SEXP C_charpack_from_state(SEXP state) {
  if(TYPEOF(state) != RAWSXP) {
    Rf_error("charport charpack from_state: state must be a raw vector");
  }
  SEXP copy = PROTECT(Rf_duplicate(state));
  SEXP x = charpack_altrep::Unserialize(R_NilValue, copy);
  UNPROTECT(1);
  return x;
}

extern "C" SEXP C_unregister_charpack(void) {
  charport::unregister_altrep(charpack_altrep::class_t);
  return R_NilValue;
}

extern "C" SEXP C_register_charpack(void) {
  charport::register_altrep(
    charpack_altrep::class_t,
    charport_reader_state_fns{charpack_altrep::reader_init, nullptr},
    charport_reader_range_fns{
      charpack_altrep::reader_strviews_range,
      charpack_altrep::reader_byteviews_range,
      charpack_altrep::reader_lengths_range,
      charpack_altrep::reader_encodings_range
    },
    charport_reader_index_fns{
      charpack_altrep::reader_strviews_index,
      charpack_altrep::reader_byteviews_index,
      charpack_altrep::reader_lengths_index,
      charpack_altrep::reader_encodings_index
    },
    charport_reader_capabilities{true, true}
  );
  return R_NilValue;
}

extern "C" SEXP C_unregister_charvec(void) {
  charport::unregister_altrep(charvec_altrep::class_t);
  return R_NilValue;
}

extern "C" SEXP C_register_charvec(void) {
  charport::register_altrep(
    charvec_altrep::class_t,
    charport_reader_state_fns{charvec_altrep::reader_init, nullptr},
    charport_reader_range_fns{
      charvec_altrep::reader_strviews_range,
      charvec_altrep::reader_byteviews_range,
      charvec_altrep::reader_lengths_range,
      charvec_altrep::reader_encodings_range
    },
    charport_reader_index_fns{
      charvec_altrep::reader_strviews_index,
      charvec_altrep::reader_byteviews_index,
      charvec_altrep::reader_lengths_index,
      charvec_altrep::reader_encodings_index
    },
    charport_reader_capabilities{true, true}
  );
  return R_NilValue;
}

extern "C" void R_init_internal_calls(DllInfo *) {
  SEXP v = PROTECT(charport::charvec::alloc_na(0));
  SEXP p = PROTECT(charport::charpack::placeholder());
  R_altrep_class_t vc = R_SUBTYPE_INIT(ALTREP_CLASS(v));
  R_altrep_class_t pc = R_SUBTYPE_INIT(ALTREP_CLASS(p));
  charvec_altrep::class_t = vc;
  charpack_altrep::class_t = pc;
  UNPROTECT(2);
}

// Force the u64 representation without allocating a multi-gigabyte payload.
// This fixture changes data1; the installed class still reads and serializes it.
void force_wide_charpack(SEXP x) {
  SEXP original = R_altrep_data1(x);
  const charpack_layout::view old = charpack_layout::parse(original);
  if(old.wide) return;
  const size_t payload = static_cast<size_t>(Rf_xlength(original)) -
    charpack_layout::header_bytes - old.n * (sizeof(uint32_t) + 1);
  const size_t bytes = charpack_layout::total_bytes(old.n, payload, true);
  if(bytes == 0) Rf_error("test charpack is too large");
  SEXP data = PROTECT(Rf_allocVector(RAWSXP, static_cast<R_xlen_t>(bytes)));
  charpack_layout::write_head(RAW(data), old.n, true);
  unsigned char * ends = RAW(data) + charpack_layout::header_bytes;
  for(size_t i = 0; i < old.n; ++i) {
    charpack_layout::write_end(ends, true, i, charpack_layout::end_at(old, i));
  }
  unsigned char * tags = ends + old.n * sizeof(uint64_t);
  std::memcpy(tags, old.tags, old.n);
  std::memcpy(tags + old.n, old.bytes, payload);
  R_set_altrep_data1(x, data);
  UNPROTECT(1);
}

extern "C" SEXP C_charpack_fill_from(
    SEXP x, SEXP strings, SEXP n_slices_, SEXP wide_) {
  if(!charpack_altrep::is(x)) Rf_error("x must be a charpack");
  if(!charpack_altrep::is_unfilled(x)) Rf_error("x is not an unfilled placeholder");
  const int n_slices = Rf_asInteger(n_slices_);
  const bool wide = Rf_asLogical(wide_) == TRUE;
  const charport_reader resolved = charport::resolve(strings);
  return charport_consumer::boundary("test charpack fill", [&]() -> SEXP {
    charport::Reader reader(resolved);
    charport::StrViews views;
    if(reader.size() > 0) reader.views(0, reader.size(), views);
    cpv::Builder builder(reader.size());
    for(R_xlen_t i = 0; i < reader.size(); ++i) builder.set(i, views[i]);
    cpv::Store store = builder.release_store();
    reader = charport::Reader();
    return charport_consumer::unwind_protect([&]() -> SEXP {
      const R_xlen_t n = static_cast<R_xlen_t>(store.size());
      if(n_slices <= 0) {
        charport::charpack::fill_views(x, n, store.records.ptrs(),
                                      store.records.lengths(), store.records.encodings());
      } else {
        size_t payload = 0;
        for(size_t i = 0; i < store.size(); ++i) {
          if(!store.view(i).is_na()) payload += static_cast<size_t>(store.length(i));
        }
        char * joined = R_alloc(payload == 0 ? 1 : payload, 1);
        char * dest = joined;
        for(size_t i = 0; i < store.size(); ++i) {
          const auto value = store.view(i);
          if(!value.is_na() && value.len > 0) {
            std::memcpy(dest, value.ptr, static_cast<size_t>(value.len));
            dest += value.len;
          }
        }
        const char ** slices = reinterpret_cast<const char **>(
          R_alloc(static_cast<size_t>(n_slices), sizeof(const char *)));
        size_t * sizes = reinterpret_cast<size_t *>(
          R_alloc(static_cast<size_t>(n_slices), sizeof(size_t)));
        size_t offset = 0;
        for(int k = 0; k < n_slices; ++k) {
          sizes[k] = payload / static_cast<size_t>(n_slices) +
            (static_cast<size_t>(k) < payload % static_cast<size_t>(n_slices) ? 1 : 0);
          slices[k] = joined + offset;
          offset += sizes[k];
        }
        charport::charpack::fill_slices(x, n, store.records.lengths(),
          store.records.encodings(), n_slices, slices, sizes);
      }
      if(wide) force_wide_charpack(x);
      return x;
    });
  });
}
