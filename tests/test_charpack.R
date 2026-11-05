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
    for (wide in c(FALSE, TRUE)) {
      y <- charpack_from_slices(input, n_slices, wide)
      stopifnot(charpack_state(y) == "filled", length(y) == length(input))
      stopifnot(bytes_identical(y, input))
    }
  }
}

catn("data1 layout: [u64 head][end offset x n][u8 encoding x n][bytes], native byte order")
# head is n, with bit 63 set when the end offsets are u64 rather than u32.
num_le <- function(b) sum(as.numeric(b) * 256^(seq_along(b) - 1L))
read_layout <- function(x) {
  raw <- charpack_data1(x)
  native <- function(b) if (.Platform$endian == "big") rev(b) else b
  head <- native(raw[1:8])
  wide <- as.integer(head[[8]]) >= 128L
  head[[8]] <- as.raw(as.integer(head[[8]]) %% 128L)
  n <- num_le(head)
  w <- if (wide) 8L else 4L
  ends <- vapply(seq_len(n), function(i) num_le(native(raw[8L + (i - 1L) * w + seq_len(w)])), 0)
  tags_at <- 8L + n * w
  list(wide = wide, n = n, ends = ends, tags = as.integer(raw[tags_at + seq_len(n)]),
       payload = raw[-seq_len(tags_at + n)])
}
for (wide in c(FALSE, TRUE)) {
  l <- read_layout(charpack_from_slices(c("ab", NA, "", "é"), wide = wide))
  stopifnot(identical(l$wide, wide), l$n == 4, identical(l$ends, c(2, 2, 2, 4)))
  stopifnot(identical(l$tags, c(254L, 255L, 254L, 1L)))
  stopifnot(identical(l$payload, c(charToRaw("ab"), charToRaw("é"))))
  l <- read_layout(charpack_from_slices(c(NA, "", NA), wide = wide))
  stopifnot(identical(l$ends, c(0, 0, 0)), length(l$payload) == 0L)
}
x <- charpack_from_slices(character(0))
stopifnot(length(charpack_data1(x)) == 8L, !read_layout(x)$wide)
stopifnot(!read_layout(charpack_from_slices(mixed))$wide)

catn("fills only from an unfilled placeholder")
x <- charpack_from_slices(c("a", "b"))
expect_error_matching(charpack_fill_from(x, "c"), "not an unfilled placeholder")
stopifnot(identical(as.character(x), c("a", "b")))
expect_error_matching(charpack_fill_from(x, "c"), "not an unfilled placeholder")
x <- charpack_from_slices(character(0))
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

catn("is_charpack")
stopifnot(is_charpack(charpack_from_slices("a")), !is_charpack(letters), !is_charpack(charvec("a")),
          !is_charpack(1:3), !is_charpack(NULL))
x <- charpack_from_slices(c("a", "b"))
charport_materialize(x)
stopifnot(is_charpack(x))

catn("as_charpack")
x <- as_charpack(mixed)
stopifnot(is_charpack(x), charpack_state(x) == "filled", bytes_identical(x, mixed))
stopifnot(identical(as_charpack(x), x))  # already a charpack: unchanged
named <- c(a = "x", b = NA, c = "")
attr(named, "extra") <- 1
x <- as_charpack(named)
stopifnot(is_charpack(x), identical(names(x), c("a", "b", "c")), is.null(attr(x, "extra")))
stopifnot(identical(unname(as.character(x)), c("x", NA, "")))
x <- as_charpack(c(one = 1L, two = NA))
stopifnot(is_charpack(x), identical(names(x), c("one", "two")), identical(as.character(x), c("1", NA)))
stopifnot(is_charpack(as_charpack(character(0))), length(as_charpack(character(0))) == 0L)
cv <- charvec("p", "q")
x <- as_charpack(cv)
stopifnot(is_charpack(x), identical(as.character(x), c("p", "q")))
stopifnot(identical(consumer("C_consumer_reader_roundtrip", as_charpack(mixed)), mixed))

catn("behaves like a character vector")
x <- charpack_from_slices(mixed)
stopifnot(identical(x[2:3], mixed[2:3]), identical(x[[1]], "plain"))
stopifnot(identical(nchar(x, "bytes"), nchar(mixed, "bytes")))
stopifnot(identical(rev(x), rev(mixed)), identical(unique(x), unique(mixed)))
stopifnot(identical(paste0(x, "!"), paste0(mixed, "!")), identical(x == "plain", mixed == "plain"))
x <- charpack_from_slices(mixed)
y <- x
y[1] <- "changed"
stopifnot(identical(y[[1]], "changed"), identical(x[[1]], "plain"))
x <- charpack_from_slices(mixed)
y <- x
names(y) <- seq_along(y)
stopifnot(identical(unname(y), mixed), charpack_state(x) == "filled")

catn("serialize keeps a filled charpack compact")
for (input in list(character(0), NA_character_, "", mixed, w_utf8)) {
  for (wide in c(FALSE, TRUE)) {
    x <- charpack_from_slices(input, wide = wide)
    data1 <- charpack_data1(x)
    for (xdr in c(TRUE, FALSE)) {
      z <- unserialize(serialize(x, NULL, xdr = xdr))
      # the state is data1, so the offset width comes back as written
      stopifnot(is_charpack(z), charpack_state(z) == "filled", identical(read_layout(z)$wide, wide))
      stopifnot(bytes_identical(z, input))
    }
    # the source is not materialized, keeps concurrent access, and its data1,
    # which serialization only reads, is unchanged
    stopifnot(charpack_state(x) == "filled", charport_info(x)$concurrent_access,
              identical(charpack_data1(x), data1))
  }
}
x <- charpack_from_slices(mixed)
names(x) <- seq_along(x)
attr(x, "meta") <- list(a = 1L)
z <- unserialize(serialize(x, NULL))
stopifnot(is_charpack(z), identical(attributes(z), attributes(x)), identical(z, x))
f <- tempfile(fileext = ".rds")
saveRDS(charpack_from_slices(mixed), f)
z <- readRDS(f)
stopifnot(is_charpack(z), bytes_identical(z, mixed))
unlink(f)

catn("a materialized charpack, or serialization version 2, writes an ordinary vector")
x <- charpack_from_slices(mixed)
charport_materialize(x)
z <- unserialize(serialize(x, NULL))
stopifnot(!is_charpack(z), bytes_identical(z, mixed))
z <- unserialize(serialize(charpack_from_slices(mixed), NULL, version = 2))
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
# charpack(c("ab", NA)): u64 head n=2 | u32 ends 2, 2 | u8 254, 255 | "ab"
state_le <- c(le_u(2, 8L), le_u(2, 4L), le_u(2, 4L), as.raw(c(254L, 255L)), charToRaw("ab"))
# wide: bit 63 set | u64 ends 2, 2 | the same tags and bytes
wide_le <- c(le_u(2, 7L), as.raw(128L), le_u(2, 8L), le_u(2, 8L), as.raw(c(254L, 255L)), charToRaw("ab"))
stopifnot(identical(charpack_serialized_state(charpack_from_slices(c("ab", NA))), state_le))
stopifnot(identical(charpack_serialized_state(charpack_from_slices(c("ab", NA), wide = TRUE)), wide_le))
if (.Platform$endian == "little") {
  x <- charpack_from_slices(mixed)
  stopifnot(identical(charpack_serialized_state(x), charpack_data1(x)))
}
find_raw <- function(haystack, needle) {
  for (i in seq_len(length(haystack) - length(needle) + 1L)) {
    if (identical(haystack[i:(i + length(needle) - 1L)], needle)) return(i)
  }
  stop("raw subsequence not found")
}
mutate_state <- function(mutate) {
  s <- serialize(charpack_from_slices(c("ab", NA)), NULL)
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
malformed <- list(
  function(s) {s[1:8] <- le_u(3, 8L); s},                      # header truncated
  function(s) {s[[7]] <- as.raw(16L); s},                      # a later version (bit 52)
  function(s) {s[[8]] <- as.raw(128L); s},                     # u64 ends that are not there
  function(s) {s[[17]] <- as.raw(4L); s},                      # not a cetype_ext_t
  function(s) {s[9:12] <- le_u(3, 4L); s},                     # runs past the payload
  function(s) {s[13:16] <- le_u(1, 4L); s},                    # ends out of order
  function(s) {s[9:16] <- c(le_u(1, 4L), le_u(1, 4L)); s},     # trailing byte
  function(s) {s[9:12] <- le_u(0, 4L); s}                      # NA with a size
)
for (mutate in malformed) {
  expect_unserialize_error(mutate_state(mutate))
}
# These change the state's length, so they are checked on the state alone.
malformed <- c(malformed, function(s) s[-length(s)],          # payload truncated
               function(s) s[1:7])                            # no header
for (mutate in malformed) {
  expect_error_matching(charpack_from_state(mutate(state_le)), "charport charpack Unserialize")
}
# Either offset width reads back as written, and the caller's state is left
# as it was.
for (input in list(character(0), c("ab", NA), c(NA, "", NA), mixed, w_utf8)) {
  for (wide in c(FALSE, TRUE)) {
    state <- charpack_serialized_state(charpack_from_slices(input, wide = wide))
    copy <- as.raw(as.integer(state))
    z <- charpack_from_state(state)
    l <- read_layout(z)
    stopifnot(identical(state, copy), identical(charpack_serialized_state(z), state))
    stopifnot(identical(l$wide, wide), bytes_identical(z, input),
              identical(l$ends, cumsum(c(0, ifelse(is.na(input), 0, nchar(input, "bytes"))))[-1L]))
  }
}

catn("No_NA and charport_materialize")
x <- charpack_from_slices(c("a", "b"))
stopifnot(!anyNA(x))
x <- charpack_from_slices(c("a", NA))
stopifnot(anyNA(x))
x <- charpack_from_slices(mixed)
charport_materialize(x)
stopifnot(charpack_state(x) == "materialized", bytes_identical(x, mixed))

catn("broker reads a filled charpack without materializing it")
info <- charport_info(charpack_from_slices(mixed))
stopifnot(info$is_registered, info$persistent_views, info$concurrent_access,
          identical(info$altrep_class, "charport::charpack"))
x <- charpack_from_slices(mixed)
stopifnot(bytes_identical(consumer("C_consumer_reader_roundtrip", x), mixed))
stopifnot(bytes_identical(consumer("C_consumer_reader_range_roundtrip", x), mixed))
stopifnot(bytes_identical(consumer("C_consumer_reader_index_roundtrip", x), rev(mixed)))
stopifnot(identical(cpc("C_cpc_reader_caps", x), c(TRUE, TRUE)))
stopifnot(charpack_state(x) == "filled")
# The end offsets give any record directly, with u32 or u64 offsets. Every
# access path must match the same strings in an ordinary vector.
pool <- c(mixed, w_utf8, w_latin1)
set.seed(1)
for (wide in c(FALSE, TRUE)) {
  for (k in c(0L, 1L, 2L, 63L, 64L, 65L, 256L, 500L)) {
    input <- pool[seq_len(k)]
    x <- charpack_from_slices(input, wide = wide)
    stopifnot(bytes_identical(consumer("C_consumer_reader_roundtrip", x), input))
    stopifnot(bytes_identical(consumer("C_consumer_reader_range_roundtrip", x), input))
    stopifnot(bytes_identical(consumer("C_consumer_reader_index_roundtrip", x), rev(input)))
    stopifnot(identical(consumer("C_consumer_reader_lengths", x),
                        as.integer(ifelse(is.na(input), NA_integer_, nchar(input, "bytes")))))
    windows <- list(c(0, k), c(0, min(k, 1L)), c(max(k - 1L, 0L), min(k, 1L)),
                    c(k %/% 3L, k - k %/% 3L), c(k, 0))
    idx <- if (k == 0L) numeric(0) else as.numeric(c(sample.int(k, min(k, 50L), TRUE), 1L, k) - 1L)
    for (w in windows) {
      stopifnot(identical(cpc("C_cpc_reader_access", x, w[[1]], w[[2]], idx),
                          cpc("C_cpc_reader_access", input, w[[1]], w[[2]], idx)))
    }
    stopifnot(charpack_state(x) == "filled")
  }
}
stopifnot(identical(consumer("C_consumer_reader_roundtrip", charpack_from_slices(character(0))), character(0)))

catn("broker falls back for materialized, unfilled and unregistered")
x <- charpack_from_slices(mixed)
charport_materialize(x)
stopifnot(bytes_identical(consumer("C_consumer_reader_roundtrip", x), mixed))
stopifnot(identical(cpc("C_cpc_reader_caps", x), c(TRUE, FALSE)))
p <- charpack_placeholder()
stopifnot(identical(consumer("C_consumer_reader_roundtrip", p), character(0)),
          charpack_state(p) == "materialized")
unregister_charpack()
x <- charpack_from_slices(mixed)
stopifnot(!charport_info(x)$is_registered)
stopifnot(bytes_identical(consumer("C_consumer_reader_roundtrip", x), mixed))
register_charpack()
stopifnot(charport_info(charpack_from_slices("a"))$is_registered)

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

catn("public header: charpack::from_store copies and leaves the store as it was")
for (input in list(character(0), NA_character_, c("", NA, ""), mixed, w_utf8)) {
  r <- cpc("C_cpc_from_store", input, NA_integer_)
  stopifnot(r[[3]], is_charpack(r[[1]]), is_charpack(r[[2]]), charpack_state(r[[1]]) == "filled")
  stopifnot(bytes_identical(r[[1]], input), bytes_identical(r[[2]], input))
  stopifnot(bytes_identical(unserialize(serialize(r[[2]], NULL)), input))
}
# ASCII-or-UTF-8 records resolve when materialized: ASCII unmarked, the rest UTF-8
input <- c("plain", "café", NA, "")
r <- cpc("C_cpc_from_store", input, 0L)
stopifnot(r[[3]], identical(as.character(r[[1]]), input),
          identical(Encoding(as.character(r[[1]])), c("unknown", "UTF-8", "unknown", "unknown")))

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
stopifnot(cpc("C_cpc_get_store_is_null", charpack_from_slices("a")))

catn("subsetting a filled charpack copies records into a new charpack")
# Fresh sources each time: the state checks come before any R element read.
subset_cases <- list(
  c(3L, 1L), integer(0), c(2L, NA, 2L), c(1L, 99L), -1L, c(TRUE, FALSE),
  c(2.9, 1), c(NA_real_, 4), 1:length(mixed)
)
for (wide in c(FALSE, TRUE)) {
  for (i in subset_cases) {
    x <- charpack_from_slices(mixed, wide = wide)
    y <- x[i]
    stopifnot(charpack_state(x) == "filled", is_charpack(y), charpack_state(y) == "filled",
              identical(read_layout(y)$wide, FALSE))
    stopifnot(bytes_identical(y, mixed[i]), bytes_identical(x, mixed))
  }
}
# names come along through R, and the source stays packed
x <- charpack_from_slices(c("a", NA, "é"))
names(x) <- c("p", "q", "r")
y <- x[c("r", "p", "zz")]
stopifnot(charpack_state(x) == "filled", is_charpack(y), charpack_state(y) == "filled")
stopifnot(identical(as.character(y), c("é", "a", NA)),
          identical(names(y), c("r", "p", NA)))
# a materialized charpack, or an empty one, subsets through R
x <- charpack_from_slices(mixed)
charport_materialize(x)
stopifnot(bytes_identical(x[c(2L, 1L)], mixed[c(2L, 1L)]))
stopifnot(identical(charpack_from_slices(character(0))[1:2], c(NA_character_, NA_character_)))
# the Reader reads a subset like any charpack
x <- charpack_from_slices(mixed)[c(4L, 2L)]
stopifnot(bytes_identical(consumer("C_consumer_reader_roundtrip", x), mixed[c(4L, 2L)]),
          charpack_state(x) == "filled")

catn("gctorture")
gctorture(TRUE)
x <- charpack_from_slices(c("a", NA, "", "é", "long string here"), 2L)
y <- x
y[2] <- "b"
z <- consumer("C_consumer_reader_roundtrip", x)
w <- cpc("C_cpc_from_views", c("q", NA))
v <- cpc("C_cpc_fill_materialized_na", c("q", NA, "é"))
u <- unserialize(serialize(charpack_from_slices(c("q", NA, "", "é"), wide = TRUE), NULL))
w2 <- charpack_from_state(charpack_serialized_state(charpack_from_slices(c("q", NA, "é"), wide = TRUE)))
gctorture(FALSE)
stopifnot(identical(as.character(x), c("a", NA, "", "é", "long string here")))
stopifnot(identical(y[[2]], "b"), identical(z, as.character(x)), identical(as.character(w), c("q", NA)),
          identical(as.character(v), c("q", NA, "é")), identical(as.character(u), c("q", NA, "", "é")),
          identical(as.character(w2), c("q", NA, "é")))

catn("OK")
