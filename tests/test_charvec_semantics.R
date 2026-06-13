# charvec reference-class semantics: behave-like-character tests (subset,
# serialize round-trip, identical(), coercions), in the style of R's own
# ALTREP tests. A charvec must be indistinguishable from the equivalent plain
# character vector in value AND encoding-mark terms: construction keeps the
# input's encoding verbatim (ascii/utf8/latin1/native/bytes), never translating.

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

# value + encoding-mark equality (identical() alone ignores encoding marks)
marks_identical <- function(x, y) {
  x <- as.character(x)
  y <- as.character(y)
  identical(x, y) && identical(Encoding(x), Encoding(y))
}

words_file <- "words_utf8.txt"
if (!file.exists(words_file)) words_file <- file.path("tests", "words_utf8.txt")
stopifnot(file.exists(words_file))
w_utf8 <- readLines(words_file, encoding = "UTF-8", warn = FALSE)
w_latin1 <- iconv(w_utf8, "UTF-8", "latin1")
stopifnot(!anyNA(w_latin1), any(Encoding(w_latin1) == "latin1"))

set.seed(20260609)

catn("construction and type identity")
x <- charvec()
stopifnot(is_charvec(x), is.character(x), typeof(x) == "character", length(x) == 0L)
x <- charvec("a", c("b", "c"))
stopifnot(is_charvec(x), identical(as.character(x), c("a", "b", "c")))
stopifnot(identical(as.character(charvec(1:3)), c("1", "2", "3")))
x <- as_charvec(letters)
stopifnot(is_charvec(x), identical(as.character(x), letters))
stopifnot(identical(as_charvec(x), x))  # already a charvec: returned unchanged
stopifnot(!is_charvec(letters), !is_charvec(1:3), !is_charvec(NULL))
x <- as_charvec(c(a = "x", b = "y"))
stopifnot(identical(names(x), c("a", "b")), identical(x[["a"]], "x"))

catn("NA, empty strings, and encoding preserved verbatim")
mixed_in <- c("plain", NA, "", w_utf8[[1L]], w_latin1[[2L]])
x <- as_charvec(mixed_in)
stopifnot(marks_identical(x, mixed_in))  # encodings kept as-is, no translation
stopifnot(identical(is.na(x), is.na(mixed_in)), anyNA(x), !anyNA(as_charvec("a")))
enc <- charport:::charvec_encodings(x)
stopifnot(identical(enc, c("ascii", NA, "ascii", "UTF-8", "latin1")))

catn("latin1 corpus is kept as latin1 (not translated)")
x <- as_charvec(w_latin1)
stopifnot(marks_identical(x, w_latin1))
stopifnot(all(charport:::charvec_encodings(x) %in% c("ascii", "latin1")))

catn("bytes encoding is preserved verbatim")
b <- rawToChar(as.raw(0xE9))
Encoding(b) <- "bytes"
x <- as_charvec(c("a", b))
stopifnot(identical(charport:::charvec_encodings(x), c("ascii", "bytes")))
stopifnot(identical(Encoding(as.character(x)), c("unknown", "bytes")))
stopifnot(identical(charToRaw(as.character(x)[[2L]]), as.raw(0xE9)))

catn("element access and coercions")
ref <- enc2utf8(c("10", "20", w_utf8[[1L]], NA, ""))
x <- as_charvec(ref)
stopifnot(identical(x[[1L]], "10"), identical(x[4], NA_character_))
stopifnot(identical(as.integer(x[1:2]), c(10L, 20L)))
stopifnot(identical(paste0(x, "!"), paste0(ref, "!")))
stopifnot(identical(nchar(x[c(1, 3)]), nchar(ref[c(1, 3)])))
stopifnot(identical(nchar(x[c(1, 3)], type = "bytes"), nchar(ref[c(1, 3)], type = "bytes")))
stopifnot(identical(toupper(x), toupper(ref)))
stopifnot(identical(rev(as.character(x)), rev(ref)))
stopifnot(identical(rep(x, 2)[6:10], rep(ref, 2)[6:10]))
stopifnot(identical(sort(x, na.last = TRUE), sort(ref, na.last = TRUE)))
stopifnot(identical(match(c("20", "nope"), x), match(c("20", "nope"), ref)))
stopifnot(identical(unique(as_charvec(c("a", "a", "b"))), c("a", "b")))
stopifnot(identical(c(x, "tail")[6L], "tail"))
stopifnot(identical(as.character(factor(as_charvec(c("b", "a")))), c("b", "a")))

catn("identical() against the plain equivalent")
x <- as_charvec(w_latin1)
stopifnot(identical(x, w_latin1))
stopifnot(identical(w_latin1, x))

catn("subsetting semantics")
ref <- c(w_utf8[1:5], NA, "", w_latin1[7:8])
names(ref) <- letters[seq_along(ref)]
x <- as_charvec(ref)
stopifnot(is_charvec(x[2:4]))
stopifnot(marks_identical(x[2:4], ref[2:4]))
stopifnot(marks_identical(x[-c(1L, 3L)], ref[-c(1L, 3L)]))
stopifnot(marks_identical(x[c(3L, NA, 99L)], ref[c(3L, NA, 99L)]))
stopifnot(marks_identical(x[c(TRUE, FALSE, NA)], ref[c(TRUE, FALSE, NA)]))
stopifnot(marks_identical(x[0L], ref[0L]), length(x[0L]) == 0L)
stopifnot(marks_identical(x[c(2.0, 4.0)], ref[c(2.0, 4.0)]))
stopifnot(identical(names(x[c(2L, 5L)]), names(ref[c(2L, 5L)])))
for (i in 1:25) {
  idx <- sample(c(seq_along(ref), NA, 50L), size = sample(0:12, 1), replace = TRUE)
  stopifnot(marks_identical(x[idx], ref[idx]))
}

catn("subsetting after materialization")
x <- as_charvec(ref)
charport_materialize(x)
stopifnot(is_charvec(x))
stopifnot(is_charvec(x[2:4]))
stopifnot(marks_identical(x[c(3L, NA, 99L)], ref[c(3L, NA, 99L)]))

catn("copy-on-write under [<-")
ref <- enc2utf8(c(w_utf8[1:4], NA))
x <- as_charvec(ref)
y <- x
y[2L] <- "replaced"
ref2 <- ref
ref2[2L] <- "replaced"
stopifnot(marks_identical(x, ref))   # original untouched
stopifnot(marks_identical(y, ref2))
stopifnot(is_charvec(y))             # duplication stayed in-class
y[[7L]] <- "grown"                   # length-extending subassign
ref2[[7L]] <- "grown"
stopifnot(identical(as.character(y), ref2))

catn("copy-on-write from a materialized charvec")
x <- as_charvec(ref)
charport_materialize(x)
y <- x
y[1L] <- "q"
stopifnot(marks_identical(x, ref), identical(y[[1L]], "q"))

catn("serialization round trip (unmaterialized stays charvec)")
mixed_in <- c(w_utf8[1:50], NA, "", w_latin1[51:100], b)
x <- as_charvec(mixed_in)
y <- unserialize(serialize(x, NULL))
stopifnot(is_charvec(y))
stopifnot(marks_identical(x, y))
stopifnot(identical(charport:::charvec_encodings(x), charport:::charvec_encodings(y)))
# xdr = FALSE path too (used by the corrupted-state tests below)
y <- unserialize(serialize(x, NULL, xdr = FALSE))
stopifnot(is_charvec(y), marks_identical(x, y))

catn("serialization round trip through a file")
tmp <- tempfile(fileext = ".rds")
on.exit(unlink(tmp), add = TRUE)
saveRDS(x, tmp)
y <- readRDS(tmp)
stopifnot(is_charvec(y), marks_identical(x, y))

catn("materialized charvec round-trips to a plain character vector")
x <- as_charvec(mixed_in)
charport_materialize(x)
y <- unserialize(serialize(x, NULL))
stopifnot(marks_identical(x, y))

catn("charport_materialize is a no-op on plain character vectors")
stopifnot(identical(charport_materialize(letters), letters))
err <- tryCatch({charport_materialize(1:3); NULL}, error = identity)
stopifnot(inherits(err, "error"))

catn("corrupted serialized_state errors cleanly")
encode_native_uint <- function(x, size) {
  remaining <- as.numeric(x)
  out <- as.raw(integer(size))
  for (i in seq_len(size)) {
    byte <- as.integer(remaining %% 256)
    out[[if (.Platform$endian == "little") i else size - i + 1L]] <- as.raw(byte)
    remaining <- floor(remaining / 256)
  }
  out
}
find_raw_subsequence <- function(haystack, needle) {
  limit <- length(haystack) - length(needle) + 1L
  stopifnot(limit >= 1L)
  for (i in seq_len(limit)) {
    if (identical(haystack[i:(i + length(needle) - 1L)], needle)) {
      return(i)
    }
  }
  stop("raw subsequence not found")
}
mutate_serialized_state <- function(x, expected_state, mutate) {
  serialized <- serialize(x, NULL, xdr = FALSE)
  start <- find_raw_subsequence(serialized, expected_state)
  state_idx <- start:(start + length(expected_state) - 1L)
  mutated_state <- mutate(serialized[state_idx])
  stopifnot(is.raw(mutated_state), length(mutated_state) == length(expected_state))
  serialized[state_idx] <- mutated_state
  serialized
}
expect_unserialize_error <- function(serialized) {
  err <- tryCatch({unserialize(serialized); NULL}, error = identity)
  stopifnot(inherits(err, "error"))
  stopifnot(grepl("charport", conditionMessage(err), fixed = TRUE))
}
# state layout for charvec("abc"): u64 n=1 | u32 len=3 | u8 enc=254 (ascii) | "abc"
state_abc <- c(
  encode_native_uint(1L, 8L),
  encode_native_uint(3L, 4L),
  as.raw(254L),
  charToRaw("abc")
)
expect_unserialize_error(mutate_serialized_state(charvec("abc"), state_abc, function(state) {
  state[[1L]] <- as.raw(0x02)  # claim 2 elements: header truncated
  state
}))
expect_unserialize_error(mutate_serialized_state(charvec("abc"), state_abc, function(state) {
  state[[13L]] <- as.raw(0x04)  # not a charport_enc value
  state
}))
expect_unserialize_error(mutate_serialized_state(charvec("abc"), state_abc, function(state) {
  state[9:12] <- encode_native_uint(5L, 4L)  # length runs past the payload
  state
}))
expect_unserialize_error(mutate_serialized_state(charvec("abc"), state_abc, function(state) {
  state[9:12] <- encode_native_uint(2L, 4L)  # trailing payload byte
  state
}))

catn("whole-corpus stress")
big_in <- sample(c(w_utf8, w_latin1, NA, ""), size = 20000L, replace = TRUE)
x <- as_charvec(big_in)
stopifnot(marks_identical(x, big_in))   # latin1/utf8/ascii/NA all verbatim
y <- unserialize(serialize(x, NULL))
stopifnot(is_charvec(y), marks_identical(y, big_in))
for (i in 1:10) {
  idx <- sample(c(seq_along(big_in), NA, 1e6L), size = 500L, replace = TRUE)
  stopifnot(marks_identical(x[idx], big_in[idx]))
}

catn("semantics tests passed")
