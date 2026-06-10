#ifndef CHARPORT_STRVIEW_H
#define CHARPORT_STRVIEW_H

// The ABI element types: charport_enc and charport_strview. R-free so that
// backend storage engines can compile against them without R headers; the
// rest of the public ABI (reader, registration) lives in charport.hpp.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Values 0-3 mirror base R's cetype_t so the direct path can convert
// getCharCE() results value-preserving (that mirroring is the only
// load-bearing numbering); extension values live high to dodge any future
// cetype_t additions. The exact extension numbers are free to change until
// this header freezes at the first CRAN release.
enum class charport_enc : uint8_t {
    CE_NATIVE        = 0,
    CE_UTF8          = 1,
    CE_LATIN1        = 2,
    CE_BYTES         = 3,
    CE_ASCII_OR_UTF8 = 100,   // valid UTF-8, ASCII-ness not tracked (reserved; emission policy open)
    CE_ASCII         = 254,
    CE_NA            = 255
};

// A borrowed view of one string element: pointer + byte length + encoding.
// The bytes are owned by whoever produced the view (a backend's store, or a
// CHARSXP) and stay valid only as long as that owner promises.
struct charport_strview {
    const char * ptr = nullptr;    // nullptr <=> NA_character_
    uint32_t     len = 0;          // byte length; no NUL-termination guarantee
    charport_enc enc = charport_enc::CE_NA;

    // ptr is the single source of truth for NA-ness; enc == CE_NA is set
    // defensively by producers but never tested by consumers.
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
};

#if __cplusplus >= 201103L
inline charport_strview make_strview(const char * ptr, uint32_t len, charport_enc enc) noexcept {
    charport_strview out;
    out.ptr = ptr;
    out.len = len;
    out.enc = enc;
    return out;
}
#endif

#if defined(__cpp_lib_is_trivially_copyable) || \
    (defined(__GLIBCXX__) && (__GLIBCXX__ >= 20150422)) || \
    (defined(_LIBCPP_VERSION)) || \
    (defined(_MSC_VER))
static_assert(std::is_trivially_copyable<charport_strview>::value,
              "charport_strview must remain trivially copyable (ABI POD)");
#endif

#endif
