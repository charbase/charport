# Serialization across locales. A packed charpack or charvec state carries
# native-tagged records only when it was written in a UTF-8 locale; outside
# one the state is a temporary character vector and the source stays packed.
# A packed state read outside a UTF-8 locale marks native records UTF-8.
# native_is_utf8() follows Sys.setlocale(), so this switches LC_CTYPE in
# process: the C locale is never UTF-8 and exists on every platform.

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

helper <- file.path("helpers", "build_dso.R")
if (!file.exists(helper)) helper <- file.path("tests", helper)
source(helper)
helper <- file.path("helpers", "internal_calls.R")
if (!file.exists(helper)) helper <- file.path("tests", helper)
source(helper)

old_ctype <- Sys.getlocale("LC_CTYPE")
set_ctype <- function(candidates) {
  for (loc in candidates) {
    if (nzchar(suppressWarnings(Sys.setlocale("LC_CTYPE", loc)))) return(TRUE)
  }
  FALSE
}
utf8_locales <- c("C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8", "English_United States.utf8")
if (!isTRUE(l10n_info()[["UTF-8"]]) && !set_ctype(utf8_locales)) {
  catn("no UTF-8 locale available; skipping")
  quit(save = "no", status = 0)
}
utf8_ctype <- Sys.getlocale("LC_CTYPE")
to_utf8 <- function() stopifnot(set_ctype(utf8_ctype), isTRUE(l10n_info()[["UTF-8"]]))
to_c <- function() stopifnot(set_ctype("C"), !isTRUE(l10n_info()[["UTF-8"]]))

dll <- compile_test_dso("charport_consumer.cpp", label = "charport locale consumer")
c_dll <- compile_test_dso("charport_c_header.c", label = "charport C locale consumer")
cxx_native_is_utf8 <- function() .Call(getNativeSymbolInfo("C_consumer_native_is_utf8", PACKAGE = dll[["name"]]))
c_native_is_utf8 <- function() .Call(getNativeSymbolInfo("C_charport_c_native_is_utf8", PACKAGE = c_dll[["name"]]))

raw_bytes <- function(x) lapply(as.character(x), function(s) if (is.na(s)) NA else charToRaw(s))

# Unmarked "é", made in the UTF-8 locale. The others carry their own marks.
native <- rawToChar(as.raw(c(0xc3, 0xa9)))
utf8 <- "é"
latin1 <- iconv(utf8, "UTF-8", "latin1")
bytes <- rawToChar(as.raw(0xe9)); Encoding(bytes) <- "bytes"
input <- c("plain", native, utf8, latin1, bytes, NA)
stopifnot(identical(Encoding(input), c("unknown", "unknown", "UTF-8", "latin1", "bytes", "unknown")))
no_native <- c("plain", utf8, latin1, bytes, NA)

classes <- list(
  charpack = list(make = as_charpack, is = is_charpack,
                  packed = function(x) charpack_state(x) == "filled"),
  charvec = list(make = as_charvec, is = is_charvec,
                 packed = function(x) !charvec_stats(x)$materialized)
)

catn("native_is_utf8() follows Sys.setlocale() through the C and C++ headers")
to_utf8()
stopifnot(isTRUE(c_native_is_utf8()), isTRUE(cxx_native_is_utf8()))
to_c()
stopifnot(identical(c_native_is_utf8(), FALSE), identical(cxx_native_is_utf8(), FALSE))
to_utf8()
stopifnot(isTRUE(c_native_is_utf8()), isTRUE(cxx_native_is_utf8()))

for (cls in names(classes)) {
  k <- classes[[cls]]

  catn(cls, ": a UTF-8 locale writes the packed state and keeps every mark")
  to_utf8()
  x <- k$make(input)
  s_utf8 <- serialize(x, NULL)
  stopifnot(k$packed(x))
  y <- unserialize(s_utf8)
  stopifnot(k$is(y), k$packed(y))
  stopifnot(identical(Encoding(y), Encoding(input)), identical(raw_bytes(y), raw_bytes(input)))

  catn(cls, ": read outside a UTF-8 locale, native records are marked UTF-8")
  to_c()
  y <- unserialize(s_utf8)
  stopifnot(k$is(y), k$packed(y))
  expected <- Encoding(input); expected[2] <- "UTF-8"
  stopifnot(identical(Encoding(y), expected), identical(raw_bytes(y), raw_bytes(input)))

  catn(cls, ": written outside a UTF-8 locale with native records, the state is a plain vector")
  x <- k$make(input)
  s_c <- serialize(x, NULL)
  stopifnot(k$packed(x))                      # the source is not materialized
  y <- unserialize(s_c)                       # read back in the same locale
  stopifnot(!k$is(y), identical(Encoding(y), Encoding(input)), identical(raw_bytes(y), raw_bytes(input)))

  catn(cls, ": without native records, any locale writes the packed state")
  x <- k$make(no_native)
  y <- unserialize(serialize(x, NULL))
  stopifnot(k$is(y), k$packed(x), k$packed(y))
  stopifnot(identical(Encoding(y), Encoding(no_native)), identical(raw_bytes(y), raw_bytes(no_native)))
  to_utf8()
}

catn("charvec: a CPV1 state (charport 0.1) reads native records as the current locale's")
to_utf8()
s_v2 <- serialize(as_charvec(input), NULL, xdr = FALSE)
at <- which(vapply(seq_len(length(s_v2) - 3L), function(i) {
  identical(s_v2[i:(i + 3L)], charToRaw("CPV2"))
}, logical(1)))
stopifnot(length(at) == 1L)
s_v1 <- s_v2
s_v1[[at + 3L]] <- charToRaw("1")
to_c()
y1 <- unserialize(s_v1)
y2 <- unserialize(s_v2)
stopifnot(is_charvec(y1), is_charvec(y2))
stopifnot(identical(Encoding(y1), Encoding(input)))           # native kept
expected <- Encoding(input); expected[2] <- "UTF-8"
stopifnot(identical(Encoding(y2), expected))                  # native marked UTF-8
stopifnot(identical(raw_bytes(y1), raw_bytes(input)), identical(raw_bytes(y2), raw_bytes(input)))
to_utf8()

invisible(Sys.setlocale("LC_CTYPE", old_ctype))
catn("OK")
