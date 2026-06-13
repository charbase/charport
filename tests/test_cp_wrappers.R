# cp:: consumer-wrapper semantics, exercised by charport consuming its own
# public header through R_GetCCallable: cp::Reader must agree with the raw
# resolve loop, and cp::charvec::Builder / BuilderMT must reproduce any
# reader's content as a fresh charvec (the end-to-end interop scenario:
# read via reader, build via builder, no CHARSXPs in between).

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

read_all  <- charport:::charport_read_all
roundtrip <- charport:::cp_reader_roundtrip
rebuild   <- charport:::cp_builder_from_reader
reserve_rebuild <- charport:::cp_builder_reserve
encs      <- charport:::charvec_encodings
stats     <- charport:::charvec_stats
rinfo     <- charport:::charport_reader_info

marks_identical <- function(x, y) {
  x <- as.character(x)
  y <- as.character(y)
  identical(x, y) && identical(Encoding(x), Encoding(y))
}

expect_error_matching <- function(expr, pattern) {
  err <- tryCatch({expr; NULL}, error = identity)
  stopifnot(inherits(err, "error"))
  stopifnot(grepl(pattern, conditionMessage(err)))
}

words_file <- "words_utf8.txt"
if (!file.exists(words_file)) words_file <- file.path("tests", "words_utf8.txt")
w_utf8 <- readLines(words_file, encoding = "UTF-8", warn = FALSE)
w_latin1 <- iconv(w_utf8, "UTF-8", "latin1")
b <- rawToChar(as.raw(0xE9)); Encoding(b) <- "bytes"

set.seed(20260611)

catn("cp::Reader agrees with the raw resolve loop")
inputs <- list(
  character(0),
  c("plain", NA, "", w_utf8[1:5], w_latin1[6:10], b),
  as_charvec(c(w_utf8[1:20], NA, "", b)),
  as.character(1:500)
)
x <- as_charvec(c("m", NA)); charport_materialize(x)
inputs <- c(inputs, list(x))
for (input in inputs) {
  stopifnot(marks_identical(roundtrip(input), read_all(input)))
}
expect_error_matching(roundtrip(1:3), "character")

catn("builder rebuilds a charvec input across shard counts")
input <- c(w_utf8[1:40], NA, "", w_latin1[41:80], b, NA)
x <- as_charvec(input)
ref <- as.character(x)
for (k in c(0L, 1L, 2L, 3L, 8L)) {
  out <- rebuild(x, k)
  stopifnot(is_charvec(out))
  stopifnot(marks_identical(out, ref))
  stopifnot(identical(encs(out), encs(x)))      # views stored verbatim
  stopifnot(rinfo(out)$path == "backend")       # output is a real served charvec
}
stopifnot(!stats(x)$materialized)               # building never materialized the input

catn("builder stores every reader view verbatim (ascii/bytes/NA)")
plain <- c("abc", NA, "", b, "zz")
out <- rebuild(plain, 2L)
stopifnot(is_charvec(out), marks_identical(out, plain))
stopifnot(identical(encs(out), c("ascii", NA, "ascii", "bytes", "ascii")))

catn("latin1 views pass through the builder unchanged (no policy, no error)")
latin1_word <- w_latin1[which(Encoding(w_latin1) == "latin1")[1L]]
for (k in c(0L, 1L)) {
  out <- rebuild(c("a", latin1_word), k)
  stopifnot(is_charvec(out), marks_identical(out, c("a", latin1_word)))
  stopifnot(identical(encs(out), c("ascii", "latin1")))
}

catn("builder reserve() path rebuilds identically (serial and sharded)")
# the reserve path keeps whatever encoding the view carried, latin1 included
x <- as_charvec(c(w_utf8[1:40], NA, "", w_latin1[41:80], b, NA))
ref <- as.character(x)
for (k in c(0L, 1L, 3L, 8L)) {
  out <- reserve_rebuild(x, k)
  stopifnot(is_charvec(out), marks_identical(out, ref), identical(encs(out), encs(x)))
  stopifnot(rinfo(out)$path == "backend")
}
stopifnot(identical(as.character(reserve_rebuild(as_charvec(character(0)), 2L)), character(0)))

catn("builder error contract (throwing set/reserve, single-shot finish, safe abandon)")
stopifnot(isTRUE(charport:::cp_builder_errors()))

catn("builder edge cases")
out <- rebuild(charvec(), 1L)
stopifnot(is_charvec(out), length(out) == 0L)
out <- rebuild(character(0), 3L)                # more shards than elements
stopifnot(length(out) == 0L)
out <- rebuild(c(NA_character_, NA_character_), 2L)
stopifnot(all(is.na(out)), stats(out)$n_slices == 0)
big <- strrep("q", 300000)                      # > 256 KiB slice cap, via a shard
out <- rebuild(as_charvec(c(big, "tail")), 1L)
stopifnot(identical(as.character(out), c(big, "tail")))

catn("built charvec serializes like any other")
x <- rebuild(as_charvec(c(w_utf8[1:10], NA, "", b)), 3L)
y <- unserialize(serialize(x, NULL))
stopifnot(is_charvec(y), marks_identical(x, y), identical(encs(x), encs(y)))

catn("property test: rebuild across random shard counts")
for (trial in 1:15) {
  k <- sample(0:8, 1)
  n <- sample(0:400, 1)
  input <- sample(c(w_utf8, w_latin1, NA, ""), n, replace = TRUE)
  x <- as_charvec(input)
  out <- rebuild(x, k)
  stopifnot(marks_identical(out, as.character(x)))
  stopifnot(identical(encs(out), encs(x)))
}

catn("cp wrapper tests passed")
