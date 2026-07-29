#ifndef CHARPORT_INTEROP_TYPES_H
#define CHARPORT_INTEROP_TYPES_H

// Element-view ABI types. Include charport.h from packages; this nested
// layout is for humans reading the installed headers.

#include <stddef.h>
#include <stdint.h>

#ifndef R_NO_REMAP
#define R_NO_REMAP
#endif
#include <Rinternals.h>

#ifdef __cplusplus
#include <cstring>
#include <type_traits>
#endif

/* Only zero means success. Unknown nonzero values are generic failures. */
#define CHARPORT_STATUS_OK 0
#define CHARPORT_STATUS_ERROR 1
#define CHARPORT_STATUS_NO_MEMORY 2
#define CHARPORT_STATUS_OUT_OF_RANGE 3

#ifdef __cplusplus
enum class cetype_ext_t : uint8_t {
    CE_NATIVE        = 0,
    CE_UTF8          = 1,
    CE_LATIN1        = 2,
    CE_BYTES         = 3,
    CE_ASCII_OR_UTF8 = 100,
    CE_ASCII         = 254,
    CE_NA            = 255
};
#else
typedef uint8_t cetype_ext_t;
enum {
    CETYPE_EXT_NATIVE        = 0,
    CETYPE_EXT_UTF8          = 1,
    CETYPE_EXT_LATIN1        = 2,
    CETYPE_EXT_BYTES         = 3,
    CETYPE_EXT_ASCII_OR_UTF8 = 100,
    CETYPE_EXT_ASCII         = 254,
    CETYPE_EXT_NA            = 255
};
#endif

typedef struct charport_byteview {
    const char * ptr;
    int len;

#ifdef __cplusplus
    inline bool is_na() const noexcept {
        return ptr == nullptr || len == NA_INTEGER;
    }

    inline bool operator==(const charport_byteview & other) const noexcept {
        if(is_na() && other.is_na()) return true;
        if(is_na() || other.is_na()) return false;
        if(len != other.len) return false;
        if(ptr == other.ptr) return true;
        return std::memcmp(ptr, other.ptr, static_cast<size_t>(len)) == 0;
    }

    inline bool operator!=(const charport_byteview & other) const noexcept {
        return !(*this == other);
    }
#endif
} charport_byteview;

typedef struct charport_strview {
    const char * ptr;
    int len;
    cetype_ext_t enc;

#ifdef __cplusplus
    inline bool is_na() const noexcept {
        return ptr == nullptr || len == NA_INTEGER || enc == cetype_ext_t::CE_NA;
    }

    inline bool operator==(const charport_strview & other) const noexcept {
        if(is_na() && other.is_na()) return true;
        if(is_na() || other.is_na()) return false;
        if(len != other.len || enc != other.enc) return false;
        if(ptr == other.ptr) return true;
        return std::memcmp(ptr, other.ptr, static_cast<size_t>(len)) == 0;
    }

    inline bool operator!=(const charport_strview & other) const noexcept {
        return !(*this == other);
    }
#endif
} charport_strview;

#ifdef __cplusplus
inline charport_byteview make_byteview(const char * ptr, int len) noexcept {
    charport_byteview out;
    out.ptr = ptr;
    out.len = len;
    return out;
}

inline charport_strview make_strview(const char * ptr, int len, cetype_ext_t enc) noexcept {
    charport_strview out;
    out.ptr = ptr;
    out.len = len;
    out.enc = enc;
    return out;
}

#if defined(__cpp_lib_is_trivially_copyable) || \
    (defined(__GLIBCXX__) && (__GLIBCXX__ >= 20150422)) || \
    (defined(_LIBCPP_VERSION)) || \
    (defined(_MSC_VER))
static_assert(std::is_trivially_copyable<charport_strview>::value,
              "charport_strview must remain trivially copyable (ABI POD)");
static_assert(std::is_trivially_copyable<charport_byteview>::value,
              "charport_byteview must remain trivially copyable (ABI POD)");
#endif
static_assert(sizeof(cetype_ext_t) == sizeof(uint8_t),
              "cetype_ext_t must remain one byte");
#else
static inline charport_byteview make_byteview(const char * ptr, int len) {
    charport_byteview out;
    out.ptr = ptr;
    out.len = len;
    return out;
}

static inline charport_strview make_strview(const char * ptr, int len, cetype_ext_t enc) {
    charport_strview out;
    out.ptr = ptr;
    out.len = len;
    out.enc = enc;
    return out;
}
#endif

#endif
