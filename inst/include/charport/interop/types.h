#ifndef CHARPORT_INTEROP_TYPES_H
#define CHARPORT_INTEROP_TYPES_H

// R-free element-view ABI types. Include charport.h from packages; this
// nested layout is for humans reading the installed headers.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <cstring>
#include <type_traits>
#endif

#ifdef __cplusplus
enum class charport_enc : uint8_t {
    CE_NATIVE        = 0,
    CE_UTF8          = 1,
    CE_LATIN1        = 2,
    CE_BYTES         = 3,
    CE_ASCII_OR_UTF8 = 100,
    CE_ASCII         = 254,
    CE_NA            = 255
};
#else
typedef uint8_t charport_enc;
enum {
    CHARPORT_CE_NATIVE        = 0,
    CHARPORT_CE_UTF8          = 1,
    CHARPORT_CE_LATIN1        = 2,
    CHARPORT_CE_BYTES         = 3,
    CHARPORT_CE_ASCII_OR_UTF8 = 100,
    CHARPORT_CE_ASCII         = 254,
    CHARPORT_CE_NA            = 255
};
#endif

typedef struct charport_strview {
    const char * ptr;
    uint32_t     len;
    charport_enc enc;

#ifdef __cplusplus
    inline bool is_na() const noexcept {
        return ptr == nullptr;
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
inline charport_strview make_strview(const char * ptr, uint32_t len, charport_enc enc) noexcept {
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
#endif
#else
static inline charport_strview make_strview(const char * ptr, uint32_t len, charport_enc enc) {
    charport_strview out;
    out.ptr = ptr;
    out.len = len;
    out.enc = enc;
    return out;
}
#endif

#endif
