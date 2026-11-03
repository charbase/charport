# charpack: compact ALTREP character vectors in one raw vector. Values and
# encoding marks must match the input exactly, the three states (unfilled,
# filled, materialized) must only move forward, the broker must read a filled
# charpack without materializing it, and the public C++ API must behave the
# same as the internal entry points.

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

helper <- file.path("helpers", "build_dso.R")
if (!file.exists(helper)) helper <- file.path("tests", helper)
source(helper)
helper <- file.path("helpers", "internal_calls.R")
if (!file.exists(helper)) helper <- file.path("tests", helper)
source(helper)

marks_identical <- function(x, y) {
  x <- as.character(x)
  y <- as.character(y)
  identical(x, y) && identical(Encoding(x), Encoding(y))
}

# identical bytes as well: Encoding() does not see the bytes of "bytes" strings
bytes_identical <- function(x, y) {
  marks_identical(x, y) &&
    identical(lapply(as.character(x), function(s) if (is.na(s)) NA else charToRaw(s)),
              lapply(as.character(y), function(s) if (is.na(s)) NA else charToRaw(s)))
}

expect_error_matching <- function(expr, pattern) {
  err <- tryCatch({expr; NULL}, error = identity)
  stopifnot(inherits(err, "error"), grepl(pattern, conditionMessage(err)))
}

words_file <- "words_utf8.txt"
if (!file.exists(words_file)) words_file <- file.path("tests", "words_utf8.txt")
w_utf8 <- readLines(words_file, encoding = "UTF-8", warn = FALSE)
w_latin1 <- iconv(w_utf8, "UTF-8", "latin1")
w_bytes <- w_utf8[1:5]
Encoding(w_bytes) <- "bytes"
mixed <- c("plain", NA, "", w_utf8[1:20], w_latin1[21:40], w_bytes, NA, "",
           strrep("long", 1000))

catn("compiling consumers (R CMD SHLIB)")
dll <- compile_test_dso("charport_consumer.cpp", label = "charport wrapper consumer")
cpc_dll <- compile_test_dso("charpack_consumer.cpp", label = "charpack consumer")
consumer <- function(name, ...) {
  .Call(getNativeSymbolInfo(name, PACKAGE = dll[["name"]]), ...)
}
cpc <- function(name, ...) {
  .Call(getNativeSymbolInfo(name, PACKAGE = cpc_dll[["name"]]), ...)
}

catn("placeholder and fill")
p <- charpack_placeholder()
stopifnot(is_charpack(p), charpack_state(p) == "unfilled", length(p) == 0L)
x <- charpack_fill_from(p, mixed)
stopifnot(identical(x, p), charpack_state(x) == "filled", length(x) == length(mixed))
stopifnot(bytes_identical(x, mixed), charpack_state(x) == "materialized")

for (input in list(character(0), NA_character_, "", "a", c(NA_character_, NA_character_), rep("", 5),
                   mixed, w_utf8, w_latin1)) {
  for (n_slices in c(0L, 1L, 2L, 7L)) {
    y <- as_charpack(input, n_slices)
    stopifnot(charpack_state(y) == "filled", length(y) == length(input))
    stopifnot(bytes_identical(y, input))
  }
}

catn("data1 layout: [u64 n][u32 size x n][u8 encoding x n][bytes], native byte order")
x <- as_charpack(c("ab", NA, "", "é"))
raw <- charpack_data1(x)
con <- rawConnection(raw)
n <- readBin(con, "raw", n = 8L)
expected_n <- as.raw(c(4L, rep(0L, 7L)))
if (.Platform$endian == "big") expected_n <- rev(expected_n)
sizes <- readBin(con, "integer", n = 4L, size = 4L)
encs <- readBin(con, "integer", n = 4L, size = 1L, signed = FALSE)
payload <- readBin(con, "raw", n = 100L)
close(con)
stopifnot(identical(n, expected_n), identical(sizes, c(2L, 0L, 0L, 2L)))
stopifnot(identical(encs, c(254L, 255L, 254L, 1L)))
stopifnot(identical(payload, c(charToRaw("ab"), charToRaw("é"))))

catn("fills only from an unfilled placeholder")
x <- as_charpack(c("a", "b"))
expect_error_matching(charpack_fill_from(x, "c"), "not an unfilled placeholder")
stopifnot(identical(as.character(x), c("a", "b")))
expect_error_matching(charpack_fill_from(x, "c"), "not an unfilled placeholder")
x <- as_charpack(character(0))
stopifnot(charpack_state(x) == "filled")
expect_error_matching(charpack_fill_from(x, "c"), "not an unfilled placeholder")
expect_error_matching(charpack_fill_from(letters, "c"), "must be a charpack")
expect_error_matching(charpack_fill_from(charvec("a"), "c"), "must be a charpack")

catn("a read before the fill makes the fill fail")
p <- charpack_placeholder()
# A length-0 vector is rarely read at all; force one through Dataptr.
charport_materialize(p)
stopifnot(identical(as.character(p), character(0)), charpack_state(p) == "materialized")
expect_error_matching(charpack_fill_from(p, "c"), "not an unfilled placeholder")
p <- charpack_placeholder()
q <- unserialize(serialize(p, NULL))
stopifnot(identical(q, character(0)), !is_charpack(q))

catn("fill_materialized_na")
p <- charpack_placeholder()
expect_error_matching(charpack_fill_materialized_na(p, -1), "negative length")
stopifnot(charpack_state(p) == "unfilled")
s <- charpack_fill_materialized_na(p, 3)
stopifnot(charpack_state(p) == "materialized", identical(s, rep(NA_character_, 3)),
          identical(as.character(p), rep(NA_character_, 3)))
expect_error_matching(charpack_fill_materialized_na(p, 1), "not an unfilled placeholder")
expect_error_matching(charpack_fill_from(p, "c"), "not an unfilled placeholder")
expect_error_matching(charpack_fill_materialized_na(letters, 1), "must be a charpack")
p <- charpack_placeholder()
invisible(charpack_fill_materialized_na(p, 0))
stopifnot(charpack_state(p) == "materialized", identical(as.character(p), character(0)))
x <- cpc("C_cpc_fill_materialized_na", mixed)
stopifnot(is_charpack(x), charpack_state(x) == "materialized", bytes_identical(x, mixed))
stopifnot(identical(consumer("C_consumer_reader_roundtrip", x), as.character(x)))
expect_error_matching(charpack_fill_from(x, "c"), "not an unfilled placeholder")

catn("behaves like a character vector")
x <- as_charpack(mixed)
stopifnot(identical(x[2:3], mixed[2:3]), identical(x[[1]], "plain"))
stopifnot(identical(nchar(x, "bytes"), nchar(mixed, "bytes")))
stopifnot(identical(rev(x), rev(mixed)), identical(unique(x), unique(mixed)))
stopifnot(identical(paste0(x, "!"), paste0(mixed, "!")), identical(x == "plain", mixed == "plain"))
x <- as_charpack(mixed)
y <- x
y[1] <- "changed"
stopifnot(identical(y[[1]], "changed"), identical(x[[1]], "plain"))
x <- as_charpack(mixed)
y <- x
names(y) <- seq_along(y)
stopifnot(identical(unname(y), mixed), charpack_state(x) == "filled")

catn("serialize keeps a filled charpack compact")
for (input in list(character(0), NA_character_, "", mixed, w_utf8)) {
  x <- as_charpack(input)
  for (xdr in c(TRUE, FALSE)) {
    z <- unserialize(serialize(x, NULL, xdr = xdr))
    stopifnot(is_charpack(z), charpack_state(z) == "filled", bytes_identical(z, input))
  }
  # the source is not materialized and keeps concurrent access
  stopifnot(charpack_state(x) == "filled", charport_info(x)$concurrent_access)
}
x <- as_charpack(mixed)
names(x) <- seq_along(x)
attr(x, "meta") <- list(a = 1L)
z <- unserialize(serialize(x, NULL))
stopifnot(is_charpack(z), identical(attributes(z), attributes(x)), identical(z, x))
f <- tempfile(fileext = ".rds")
saveRDS(as_charpack(mixed), f)
z <- readRDS(f)
stopifnot(is_charpack(z), bytes_identical(z, mixed))
unlink(f)

catn("a materialized charpack, or serialization version 2, writes an ordinary vector")
x <- as_charpack(mixed)
charport_materialize(x)
z <- unserialize(serialize(x, NULL))
stopifnot(!is_charpack(z), bytes_identical(z, mixed))
z <- unserialize(serialize(as_charpack(mixed), NULL, version = 2))
stopifnot(!is_charpack(z), bytes_identical(z, mixed))

catn("the serialized state is data1 in little-endian order")
le_u <- function(x, size) {
  out <- raw(size)
  for (i in seq_len(size)) {
    out[[i]] <- as.raw(x %% 256)
    x <- x %/% 256
  }
  out
}
# charpack(c("ab", NA)): u64 n=2 | u32 2, 0 | u8 254, 255 | "ab"
state_le <- c(le_u(2, 8L), le_u(2, 4L), le_u(0, 4L), as.raw(c(254L, 255L)), charToRaw("ab"))
x <- as_charpack(c("ab", NA))
stopifnot(identical(if (.Platform$endian == "little") charpack_data1(x)
                    else charpack_bswap_packed(charpack_data1(x)), state_le))
find_raw <- function(haystack, needle) {
  for (i in seq_len(length(haystack) - length(needle) + 1L)) {
    if (identical(haystack[i:(i + length(needle) - 1L)], needle)) return(i)
  }
  stop("raw subsequence not found")
}
mutate_state <- function(mutate) {
  s <- serialize(as_charpack(c("ab", NA)), NULL)
  idx <- find_raw(s, state_le) + seq_along(state_le) - 1L
  s[idx] <- mutate(s[idx])
  s
}
expect_unserialize_error <- function(s) {
  err <- tryCatch({unserialize(s); NULL}, error = identity)
  stopifnot(inherits(err, "error"), grepl("charport", conditionMessage(err), fixed = TRUE))
}
z <- unserialize(mutate_state(identity))
stopifnot(is_charpack(z), identical(as.character(z), c("ab", NA)))
expect_unserialize_error(mutate_state(function(s) {s[1:8] <- le_u(3, 8L); s}))    # header truncated
expect_unserialize_error(mutate_state(function(s) {s[[17]] <- as.raw(4L); s}))    # not a cetype_ext_t
expect_unserialize_error(mutate_state(function(s) {s[9:12] <- le_u(3, 4L); s}))   # runs past the payload
expect_unserialize_error(mutate_state(function(s) {s[9:12] <- le_u(1, 4L); s}))   # trailing byte
expect_unserialize_error(mutate_state(function(s) {s[13:16] <- le_u(1, 4L); s}))  # NA with a size
expect_unserialize_error(mutate_state(function(s) {s[9:12] <- as.raw(c(0L, 0L, 0L, 128L)); s}))  # too long
# the big-endian swap: n and sizes reversed, encodings and bytes unchanged
state_be <- c(rev(le_u(2, 8L)), rev(le_u(2, 4L)), rev(le_u(0, 4L)), as.raw(c(254L, 255L)),
              charToRaw("ab"))
if (.Platform$endian == "little") {
  stopifnot(identical(charpack_bswap_packed(state_le), state_be))
} else {
  stopifnot(identical(charpack_bswap_packed(state_be), state_le))
}

catn("No_NA and charport_materialize")
x <- as_charpack(c("a", "b"))
stopifnot(!anyNA(x))
x <- as_charpack(c("a", NA))
stopifnot(anyNA(x))
x <- as_charpack(mixed)
charport_materialize(x)
stopifnot(charpack_state(x) == "materialized", bytes_identical(x, mixed))

catn("broker reads a filled charpack without materializing it")
info <- charport_info(as_charpack(mixed))
stopifnot(info$is_registered, info$persistent_views, info$concurrent_access,
          identical(info$altrep_class, "charport::charpack"))
x <- as_charpack(mixed)
stopifnot(bytes_identical(consumer("C_consumer_reader_roundtrip", x), mixed))
stopifnot(bytes_identical(consumer("C_consumer_reader_range_roundtrip", x), mixed))
stopifnot(bytes_identical(consumer("C_consumer_reader_index_roundtrip", x), rev(mixed)))
stopifnot(identical(cpc("C_cpc_reader_caps", x), c(TRUE, TRUE)))
stopifnot(charpack_state(x) == "filled")
# Up to 64 elements the reader sums offsets per call; longer vectors get a
# pointer table. Cover both, and the boundary.
for (k in c(63L, 64L, 65L, 500L)) {
  input <- c(mixed, w_utf8, w_latin1)[seq_len(k)]
  x <- as_charpack(input)
  stopifnot(bytes_identical(consumer("C_consumer_reader_roundtrip", x), input))
  stopifnot(bytes_identical(consumer("C_consumer_reader_range_roundtrip", x), input))
  stopifnot(bytes_identical(consumer("C_consumer_reader_index_roundtrip", x), rev(input)))
  stopifnot(identical(consumer("C_consumer_reader_lengths", x),
                      ifelse(is.na(input), NA_integer_, nchar(input, "bytes"))))
  stopifnot(charpack_state(x) == "filled")
}
stopifnot(identical(consumer("C_consumer_reader_roundtrip", as_charpack(character(0))), character(0)))

catn("broker falls back for materialized, unfilled and unregistered")
x <- as_charpack(mixed)
charport_materialize(x)
stopifnot(bytes_identical(consumer("C_consumer_reader_roundtrip", x), mixed))
stopifnot(identical(cpc("C_cpc_reader_caps", x), c(TRUE, FALSE)))
p <- charpack_placeholder()
stopifnot(identical(consumer("C_consumer_reader_roundtrip", p), character(0)),
          charpack_state(p) == "materialized")
unregister_charpack()
x <- as_charpack(mixed)
stopifnot(!charport_info(x)$is_registered)
stopifnot(bytes_identical(consumer("C_consumer_reader_roundtrip", x), mixed))
register_charpack()
stopifnot(charport_info(as_charpack("a"))$is_registered)

catn("public header: charpack")
stopifnot(all(cpc("C_cpc_callables")))
x <- cpc("C_cpc_from_views", mixed)
stopifnot(cpc("C_cpc_is_charpack", x), charpack_state(x) == "filled", bytes_identical(x, mixed))
stopifnot(!cpc("C_cpc_is_charpack", letters), !cpc("C_cpc_is_charpack", charvec("a")))
for (n_slices in c(1L, 2L, 5L, 64L)) {
  x <- cpc("C_cpc_from_slices", mixed, n_slices)
  stopifnot(charpack_state(x) == "filled", bytes_identical(x, mixed))
}
x <- cpc("C_cpc_from_slices", c("", NA, ""), 1L)
stopifnot(identical(as.character(x), c("", NA, "")))
expect_error_matching(cpc("C_cpc_fill_twice", mixed), "not an unfilled placeholder")

catn("public header: charvec alloc_na, get_store and Writer")
x <- cpc("C_cpc_charvec_fill", mixed, FALSE)
stopifnot(is_charvec(x), bytes_identical(x, mixed))
stopifnot(cpc("C_cpc_get_store_is_null", x))  # read above: has an Elt cache or data2
x <- cpc("C_cpc_charvec_fill", mixed, TRUE)
half <- length(mixed) %/% 2L
expected <- mixed
expected[seq_len(half + 1L)] <- NA
stopifnot(is_charvec(x), bytes_identical(x, expected))
x <- charvec("a", "b")
stopifnot(!cpc("C_cpc_get_store_is_null", x))
invisible(x[[1]])
stopifnot(cpc("C_cpc_get_store_is_null", x))
stopifnot(cpc("C_cpc_get_store_is_null", letters))
stopifnot(cpc("C_cpc_get_store_is_null", as_charpack("a")))

catn("gctorture")
gctorture(TRUE)
x <- as_charpack(c("a", NA, "", "é", "long string here"), 2L)
y <- x
y[2] <- "b"
z <- consumer("C_consumer_reader_roundtrip", x)
w <- cpc("C_cpc_from_views", c("q", NA))
v <- cpc("C_cpc_fill_materialized_na", c("q", NA, "é"))
gctorture(FALSE)
stopifnot(identical(as.character(x), c("a", NA, "", "é", "long string here")))
stopifnot(identical(y[[2]], "b"), identical(z, as.character(x)), identical(as.character(w), c("q", NA)),
          identical(as.character(v), c("q", NA, "é")))

catn("OK")
