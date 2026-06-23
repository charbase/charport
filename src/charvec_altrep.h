#ifndef CHARPORT_CHARVEC_ALTREP_H
#define CHARPORT_CHARVEC_ALTREP_H

#define R_NO_REMAP
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <R_ext/Rdynload.h>
#include <Rversion.h>

#include "../inst/include/charport.h"

#include <cstdio>
#include <exception>

namespace cpi = charport::internal;

namespace charport {
namespace internal {

inline bool charsxp_is_ascii(SEXP x) {
#if (R_VERSION >= R_Version(4, 5, 0))
  return Rf_charIsASCII(x) == TRUE;
#else
  return check_ascii(CHAR(x), static_cast<size_t>(Rf_xlength(x)));
#endif
}

inline cetype_t to_base_encoding(charport_enc enc) noexcept {
  switch(enc) {
  case charport_enc::CE_ASCII:
    return CE_NATIVE;
  case charport_enc::CE_UTF8:
  case charport_enc::CE_ASCII_OR_UTF8:
    return CE_UTF8;
  case charport_enc::CE_LATIN1:
    return CE_LATIN1;
  case charport_enc::CE_BYTES:
    return CE_BYTES;
  default:
    return CE_NATIVE;
  }
}

inline SEXP make_charsxp(const charport_strview & view) {
  if(view.is_na()) {
    return NA_STRING;
  }
  if(!check_r_string_len(view.len)) {
    throw std::runtime_error("string size exceeds R string size");
  }
  return Rf_mkCharLenCE(view.ptr, static_cast<int>(view.len), to_base_encoding(view.enc));
}

inline charport_enc classify_charsxp(SEXP x) {
  switch(Rf_getCharCE(x)) {
  case CE_UTF8:
    return charport_enc::CE_UTF8;
  case CE_LATIN1:
    return charport_enc::CE_LATIN1;
  case CE_BYTES:
    return charport_enc::CE_BYTES;
  default:
    return charsxp_is_ascii(x) ? charport_enc::CE_ASCII
                               : charport_enc::CE_NATIVE;
  }
}

inline charport_strview charsxp_to_view(SEXP x) {
  if(x == NA_STRING) {
    return make_strview(nullptr, 0, charport_enc::CE_NA);
  }
  return make_strview(CHAR(x), static_cast<uint32_t>(Rf_xlength(x)), classify_charsxp(x));
}

} // namespace internal
} // namespace charport

template <typename Fun>
inline SEXP charport_sexp_guard(const char * op, Fun fun) {
  char msg[512];
  try {
    return fun();
  } catch(const std::exception & e) {
    std::snprintf(msg, sizeof(msg), "%s", e.what());
  } catch(...) {
    std::snprintf(msg, sizeof(msg), "unknown C++ exception");
  }
  Rf_error("charport %s: %s", op, msg);
  return R_NilValue;
}

// Native-endian serialized_state with a magic/version prefix and endian flag.
struct charvec_serialized_layout {
  size_t n = 0;
  bool swap = false;
  const unsigned char * size_offset = nullptr;
  const unsigned char * enc_offset = nullptr;
  const unsigned char * data_offset = nullptr;
  const unsigned char * data_end = nullptr;
};

inline bool charvec_host_is_little_endian() noexcept {
  const uint16_t one = 1;
  return *reinterpret_cast<const unsigned char *>(&one) == 1;
}

inline unsigned char charvec_native_endian_flag() noexcept {
  return charvec_host_is_little_endian() ? static_cast<unsigned char>(1)
                                         : static_cast<unsigned char>(2);
}

inline uint32_t charvec_bswap32(uint32_t x) noexcept {
  return ((x & 0x000000ffU) << 24) |
         ((x & 0x0000ff00U) << 8) |
         ((x & 0x00ff0000U) >> 8) |
         ((x & 0xff000000U) >> 24);
}

inline uint64_t charvec_bswap64(uint64_t x) noexcept {
  return ((x & 0x00000000000000ffULL) << 56) |
         ((x & 0x000000000000ff00ULL) << 40) |
         ((x & 0x0000000000ff0000ULL) << 24) |
         ((x & 0x00000000ff000000ULL) << 8) |
         ((x & 0x000000ff00000000ULL) >> 8) |
         ((x & 0x0000ff0000000000ULL) >> 24) |
         ((x & 0x00ff000000000000ULL) >> 40) |
         ((x & 0xff00000000000000ULL) >> 56);
}

inline uint64_t charvec_read_u64(const unsigned char * ptr, bool swap) noexcept {
  uint64_t out = 0;
  std::memcpy(&out, ptr, sizeof(uint64_t));
  return swap ? charvec_bswap64(out) : out;
}

inline uint32_t charvec_read_u32_advance(const unsigned char *& ptr, bool swap) noexcept {
  uint32_t out = 0;
  std::memcpy(&out, ptr, sizeof(uint32_t));
  ptr += sizeof(uint32_t);
  return swap ? charvec_bswap32(out) : out;
}

inline size_t charvec_serialized_prefix_bytes() noexcept {
  return 5;  // "CPV1" + endian flag
}

inline size_t charvec_serialized_length(size_t n, size_t payload_bytes) {
  const size_t sizes_bytes = cpi::checked_mul_size(n, sizeof(uint32_t), "serialized_state");
  const size_t enc_bytes = cpi::checked_mul_size(n, sizeof(uint8_t), "serialized_state");
  const size_t header_bytes = cpi::checked_add_size(
    cpi::checked_add_size(
      cpi::checked_add_size(charvec_serialized_prefix_bytes(), sizeof(uint64_t), "serialized_state"),
      sizes_bytes,
      "serialized_state"),
    enc_bytes,
    "serialized_state"
  );
  const size_t total_bytes = cpi::checked_add_size(header_bytes, payload_bytes, "serialized_state");
  if(total_bytes > static_cast<size_t>(std::numeric_limits<R_xlen_t>::max())) {
    throw std::runtime_error("serialized_state length exceeds R_xlen_t");
  }
  return total_bytes;
}

inline charvec_serialized_layout charvec_parse_serialized(SEXP serialized_state) {
  if(TYPEOF(serialized_state) != RAWSXP) {
    throw std::runtime_error("invalid serialized_state type");
  }

  const size_t raw_len = static_cast<size_t>(Rf_xlength(serialized_state));
  const size_t prefix_bytes = charvec_serialized_prefix_bytes();
  if(raw_len < prefix_bytes + sizeof(uint64_t)) {
    throw std::runtime_error("serialized_state is truncated");
  }

  const unsigned char * serialized_ptr = RAW(serialized_state);
  if(serialized_ptr[0] != 'C' || serialized_ptr[1] != 'P' ||
     serialized_ptr[2] != 'V' || serialized_ptr[3] != '1') {
    throw std::runtime_error("invalid serialized_state format");
  }
  const unsigned char source_endian = serialized_ptr[4];
  if(source_endian != 1 && source_endian != 2) {
    throw std::runtime_error("invalid serialized_state endian flag");
  }
  const bool swap = source_endian != charvec_native_endian_flag();

  const uint64_t n64 = charvec_read_u64(serialized_ptr + prefix_bytes, swap);
  if(n64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
     n64 > static_cast<uint64_t>(std::numeric_limits<R_xlen_t>::max())) {
    throw std::runtime_error("serialized_state length exceeds R_xlen_t");
  }

  const size_t n = static_cast<size_t>(n64);
  const size_t sizes_bytes = cpi::checked_mul_size(n, sizeof(uint32_t), "serialized_state header");
  const size_t enc_bytes = cpi::checked_mul_size(n, sizeof(uint8_t), "serialized_state header");
  const size_t header_bytes = cpi::checked_add_size(
    cpi::checked_add_size(
      cpi::checked_add_size(prefix_bytes, sizeof(uint64_t), "serialized_state header"),
      sizes_bytes,
      "serialized_state header"),
    enc_bytes,
    "serialized_state header"
  );

  if(raw_len < header_bytes) {
    throw std::runtime_error("serialized_state header is truncated");
  }

  charvec_serialized_layout layout;
  layout.n = n;
  layout.swap = swap;
  layout.size_offset = serialized_ptr + prefix_bytes + sizeof(uint64_t);
  layout.enc_offset = layout.size_offset + sizes_bytes;
  layout.data_offset = layout.enc_offset + enc_bytes;
  layout.data_end = serialized_ptr + raw_len;
  return layout;
}

inline const char * charvec_serialized_payload(const unsigned char *& data_offset,
                                               const unsigned char * data_end,
                                               size_t len) {
  const size_t remaining = static_cast<size_t>(data_end - data_offset);
  if(len > remaining) {
    throw std::runtime_error("serialized_state payload is truncated");
  }
  const char * ptr = reinterpret_cast<const char *>(data_offset);
  data_offset += len;
  return ptr;
}

struct charvec_altrep {
  static R_altrep_class_t class_t;

  static SEXP Make(cpi::charvec_data * data, bool owner) {
    SEXP xp = PROTECT(R_MakeExternalPtr(data, R_NilValue, R_NilValue));
    if(owner) {
      R_RegisterCFinalizerEx(xp, charvec_altrep::Finalize, TRUE);
    }
    SEXP res = R_new_altrep(class_t, xp, R_NilValue);
    UNPROTECT(1);
    return res;
  }

  static void Finalize(SEXP xp) {
    auto * ptr = static_cast<cpi::charvec_data*>(R_ExternalPtrAddr(xp));
    if(ptr == nullptr) {
      return;
    }
    delete ptr;
    R_ClearExternalPtr(xp);
  }

  static cpi::charvec_data * Ptr(SEXP vec) {
    return static_cast<cpi::charvec_data*>(R_ExternalPtrAddr(R_altrep_data1(vec)));
  }

  static cpi::charvec_data & Get(SEXP vec) {
    return *Ptr(vec);
  }

  static R_xlen_t Length(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      return Rf_xlength(data2);
    }
    return static_cast<R_xlen_t>(Get(vec).records.size());
  }

  static Rboolean Inspect(SEXP x, int /* pre */, int /* deep */, int /* pvec */,
                          void (*/* inspect_subtree */)(SEXP, int, int, int)) {
    bool materialized = R_altrep_data2(x) != R_NilValue;
    Rprintf("charport charvec (len=%llu, ptr=%p, %s)\n",
            static_cast<unsigned long long int>(Length(x)),
            static_cast<void*>(Ptr(x)),
            materialized ? "materialized" : "not materialized");
    return TRUE;
  }

  // Full materialization, cached in data2. The store is freed afterwards
  // (records would be a redundant copy of the payload), so record pointers
  // handed out before materialization are invalidated. This is the concrete
  // reason the reader contract is a borrow: while a reader/view is live, the
  // consumer must ensure the vector is not touched through R or aliases.
  static SEXP Materialize(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      return data2;
    }
    auto & data1 = Get(vec);
    const R_xlen_t n = static_cast<R_xlen_t>(data1.records.size());
    data2 = PROTECT(Rf_allocVector(STRSXP, n));
    for(R_xlen_t i = 0; i < n; ++i) {
      SET_STRING_ELT(data2, i, cpi::make_charsxp(data1.records[static_cast<size_t>(i)]));
    }
    R_set_altrep_data2(vec, data2);
    Finalize(R_altrep_data1(vec));
    UNPROTECT(1);
    return data2;
  }

  static SEXP DuplicateEX(SEXP vec, Rboolean /* deep */) {
    return charport_sexp_guard("charvec Duplicate", [&]() -> SEXP {
      SEXP data2 = R_altrep_data2(vec);
      if(data2 != R_NilValue) {
        const R_xlen_t n = Rf_xlength(data2);
        auto out = charport::charvec::Builder::build_store(static_cast<size_t>(n),
          [&](cpi::charvec_shard & sh, charport_strview * rec, size_t nn) {
            for(R_xlen_t i = 0; i < n; ++i) {
              cpi::copy_record(sh, rec, nn, static_cast<size_t>(i),
                               cpi::charsxp_to_view(STRING_ELT(data2, i)));
            }
          });
        return Make(out.release(), true);
      }
      auto * out = new cpi::charvec_data(Get(vec));
      return Make(out, true);
    });
  }

  static SEXP Duplicate(SEXP vec, Rboolean deep) {
    return DuplicateEX(vec, deep);
  }

  static const void * Dataptr_or_null(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 == R_NilValue) {
      return nullptr;
    }
    return STRING_PTR_RO(data2);
  }

  static SEXP MaterializeGuarded(SEXP vec) {
    return charport_sexp_guard("charvec materialize", [&]() -> SEXP {
      return Materialize(vec);
    });
  }

  static void * Dataptr(SEXP vec, Rboolean /* writeable */) {
    return const_cast<void*>(static_cast<const void*>(STRING_PTR_RO(MaterializeGuarded(vec))));
  }

  static SEXP string_Elt(SEXP vec, R_xlen_t i) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      return STRING_ELT(data2, i);
    }
    return charport_sexp_guard("charvec Elt", [&]() -> SEXP {
      return cpi::make_charsxp(Get(vec).records[static_cast<size_t>(i)]);
    });
  }

  static void string_Set_elt(SEXP vec, R_xlen_t i, SEXP new_val) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      SET_STRING_ELT(data2, i, new_val);
      return;
    }
    charport_sexp_guard("charvec Set_elt", [&]() -> SEXP {
      Get(vec).assign(static_cast<size_t>(i), cpi::charsxp_to_view(new_val));
      return R_NilValue;
    });
  }

  static int no_NA(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if(data2 != R_NilValue) {
      const R_xlen_t len = Rf_xlength(data2);
      for(R_xlen_t i = 0; i < len; ++i) {
        if(STRING_ELT(data2, i) == NA_STRING) {
          return 0;
        }
      }
      return 1;
    }
    auto & data1 = Get(vec);
    for(size_t i = 0; i < data1.records.size(); ++i) {
      if(data1.records[i].is_na()) {
        return 0;
      }
    }
    return 1;
  }

  // Subset copies records store-to-store: one memcpy per element, no
  // intermediate owned-string temporary.
  static SEXP Extract_subset(SEXP x, SEXP indx, SEXP call) {
    return charport_sexp_guard("charvec Extract_subset", [&]() -> SEXP {
      (void)call;
      SEXP data2 = R_altrep_data2(x);
      const R_xlen_t xlen = data2 != R_NilValue
        ? Rf_xlength(data2)
        : static_cast<R_xlen_t>(Get(x).records.size());

      // out-of-range and NA subscripts produce NA elements; out records start
      // NA, so only valid subscripts need a write. Builds go through a transient
      // shard context (one bump-packed chain) rather than the store directly.
      auto copy_element = [&](cpi::charvec_shard & sh, charport_strview * recs, size_t n,
                              size_t out_i, R_xlen_t zero_based) {
        if(zero_based < 0 || zero_based >= xlen) {
          return;
        }
        if(data2 != R_NilValue) {
          cpi::copy_record(sh, recs, n, out_i, cpi::charsxp_to_view(STRING_ELT(data2, zero_based)));
          return;
        }
        const charport_strview & rec = Get(x).view(static_cast<size_t>(zero_based));
        if(!rec.is_na()) {
          cpi::copy_record(sh, recs, n, out_i, rec);
        }
      };

      if(TYPEOF(indx) == LGLSXP) {
        const int * idx = LOGICAL(indx);
        const R_xlen_t idx_len = Rf_xlength(indx);
        if(idx_len == 0) {
          return Make(new cpi::charvec_data(), true);
        }

        R_xlen_t out_len = 0;
        for(R_xlen_t i = 0; i < xlen; ++i) {
          const int idx_i = idx[i % idx_len];
          if(idx_i == TRUE || idx_i == NA_LOGICAL) {
            ++out_len;
          }
        }

        auto out = charport::charvec::Builder::build_store(static_cast<size_t>(out_len),
          [&](cpi::charvec_shard & sh, charport_strview * rec, size_t nn) {
            R_xlen_t out_i = 0;
            for(R_xlen_t i = 0; i < xlen; ++i) {
              const int idx_i = idx[i % idx_len];
              if(idx_i == TRUE) {
                copy_element(sh, rec, nn, static_cast<size_t>(out_i++), i);
              } else if(idx_i == NA_LOGICAL) {
                ++out_i;  // stays NA
              }
            }
          });
        return Make(out.release(), true);
      }

      if(TYPEOF(indx) != INTSXP && TYPEOF(indx) != REALSXP) {
        throw std::runtime_error("invalid indx type in Extract_subset method");
      }
      const R_xlen_t len = Rf_xlength(indx);
      auto out = charport::charvec::Builder::build_store(static_cast<size_t>(len),
        [&](cpi::charvec_shard & sh, charport_strview * rec, size_t nn) {
          if(TYPEOF(indx) == INTSXP) {
            const int * idx = INTEGER(indx);
            for(R_xlen_t i = 0; i < len; ++i) {
              if(idx[i] != NA_INTEGER) {
                copy_element(sh, rec, nn, static_cast<size_t>(i), static_cast<R_xlen_t>(idx[i]) - 1);
              }
            }
          } else {
            const double * idx = REAL(indx);
            for(R_xlen_t i = 0; i < len; ++i) {
              if(!ISNAN(idx[i])) {
                copy_element(sh, rec, nn, static_cast<size_t>(i), static_cast<R_xlen_t>(idx[i]) - 1);
              }
            }
          }
        });
      return Make(out.release(), true);
    });
  }

  static SEXP Serialized_state(SEXP vec) {
    return charport_sexp_guard("charvec Serialized_state", [&]() -> SEXP {
      SEXP data2 = R_altrep_data2(vec);
      if(data2 != R_NilValue) {
        // materialized: serialize the plain STRSXP; unserializes to one too
        return data2;
      }
      auto & data1 = Get(vec);
      const size_t n = data1.records.size();
      size_t total_size = 0;
      for(size_t i = 0; i < n; ++i) {
        total_size = cpi::checked_add_size(
          total_size, static_cast<size_t>(data1.records[i].len), "serialized_state payload");
      }
      const size_t total_bytes = charvec_serialized_length(n, total_size);
      SEXP serialized_state = Rf_allocVector(RAWSXP, static_cast<R_xlen_t>(total_bytes));
      unsigned char * serialized_ptr = RAW(serialized_state);
      serialized_ptr[0] = 'C';
      serialized_ptr[1] = 'P';
      serialized_ptr[2] = 'V';
      serialized_ptr[3] = '1';
      serialized_ptr[4] = charvec_native_endian_flag();
      const uint64_t n64 = static_cast<uint64_t>(n);
      std::memcpy(serialized_ptr + charvec_serialized_prefix_bytes(), &n64, sizeof(uint64_t));
      unsigned char * current_offset =
        serialized_ptr + charvec_serialized_prefix_bytes() + sizeof(uint64_t);

      for(size_t i = 0; i < n; ++i) {
        const uint32_t size = data1.records[i].len;
        std::memcpy(current_offset, &size, sizeof(uint32_t));
        current_offset += sizeof(uint32_t);
      }

      for(size_t i = 0; i < n; ++i) {
        const uint8_t encoding = static_cast<uint8_t>(data1.records[i].enc);
        std::memcpy(current_offset, &encoding, sizeof(uint8_t));
        current_offset += sizeof(uint8_t);
      }

      for(size_t i = 0; i < n; ++i) {
        const charport_strview & rec = data1.records[i];
        if(rec.len > 0) {
          std::memcpy(current_offset, rec.ptr, static_cast<size_t>(rec.len));
          current_offset += static_cast<size_t>(rec.len);
        }
      }
      return serialized_state;
    });
  }

  static SEXP Unserialize(SEXP /* class */, SEXP serialized_state) {
    return charport_sexp_guard("charvec Unserialize", [&]() -> SEXP {
      if(TYPEOF(serialized_state) == STRSXP) {
        // state from a materialized charvec: stays a plain character vector
        return serialized_state;
      }
      charvec_serialized_layout layout = charvec_parse_serialized(serialized_state);

      const unsigned char * size_offset = layout.size_offset;
      const unsigned char * enc_offset = layout.enc_offset;
      const unsigned char * data_offset = layout.data_offset;

      auto ret = charport::charvec::Builder::build_store(static_cast<R_xlen_t>(layout.n),
        [&](cpi::charvec_shard & sh, charport_strview * rec, size_t nn) {
          for(size_t i = 0; i < layout.n; ++i) {
            const uint32_t size = charvec_read_u32_advance(size_offset, layout.swap);
            if(!cpi::check_r_string_len(size)) {
              throw std::runtime_error("serialized string size exceeds R string size");
            }
            const charport_enc encoding = static_cast<charport_enc>(*enc_offset++);
            const size_t stored_len = static_cast<size_t>(size);
            const char * payload = charvec_serialized_payload(data_offset, layout.data_end, stored_len);
            // Untrusted input: accept every encoding a record can legitimately
            // hold (the store keeps encodings verbatim, so any of these can have
            // been serialized) and reject only an out-of-range encoding byte.
            switch(encoding) {
            case charport_enc::CE_ASCII:
            case charport_enc::CE_UTF8:
            case charport_enc::CE_ASCII_OR_UTF8:
            case charport_enc::CE_LATIN1:
            case charport_enc::CE_NATIVE:
            case charport_enc::CE_BYTES:
              cpi::copy_record(sh, rec, nn, i, payload, stored_len, encoding);
              break;
            case charport_enc::CE_NA:
              if(stored_len != 0) {
                throw std::runtime_error("serialized NA string must have zero length");
              }
              cpi::copy_record(sh, rec, nn, i, nullptr, 0, charport_enc::CE_NA);
              break;
            default:
              throw std::runtime_error("invalid string encoding in serialized_state");
            }
          }
        });

      if(data_offset != layout.data_end) {
        throw std::runtime_error("serialized_state has trailing bytes");
      }
      return Make(ret.release(), true);
    });
  }

  // Broker hooks used when the reference class is registered.
  // init borrows the store pointer that already hangs off data1; a materialized
  // charvec has released its store, so init returns NULL and the broker serves
  // it via the direct path over the cached data2.
  static void * reader_init(SEXP x) {
    if(R_altrep_data2(x) != R_NilValue) {
      return nullptr;
    }
    return Ptr(x);
  }

  // Pure state read: no R, no allocation, no mutation, persistent records.
  static charport_strview reader_get(void * state, R_xlen_t i) {
    return static_cast<cpi::charvec_data *>(state)->records[static_cast<size_t>(i)];
  }

  static void Init(DllInfo * dll) {
    // class + package names are baked into serialized objects, so they are
    // final from the first CRAN release on
    class_t = R_make_altstring_class("charvec", "charport", dll);

    R_set_altrep_Serialized_state_method(class_t, Serialized_state);
    R_set_altrep_Unserialize_method(class_t, Unserialize);
    R_set_altrep_DuplicateEX_method(class_t, DuplicateEX);
    R_set_altrep_Duplicate_method(class_t, Duplicate);

    R_set_altrep_Length_method(class_t, Length);
    R_set_altrep_Inspect_method(class_t, Inspect);

    R_set_altvec_Dataptr_method(class_t, Dataptr);
    R_set_altvec_Dataptr_or_null_method(class_t, Dataptr_or_null);

    R_set_altstring_Elt_method(class_t, string_Elt);
    R_set_altstring_Set_elt_method(class_t, string_Set_elt);
    R_set_altstring_No_NA_method(class_t, no_NA);

    R_set_altvec_Extract_subset_method(class_t, Extract_subset);
  }
};

#endif
