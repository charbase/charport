# charport:: consumer-wrapper semantics, exercised by charport consuming its own
# public header through R_GetCCallable: charport::Reader must agree with the raw
# resolve loop, and charport::charvec::Builder / BuilderMT must reproduce any
# reader's content as a fresh charvec (the end-to-end interop scenario:
# read via reader, build via builder, no CHARSXPs in between).

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

helper <- file.path("helpers", "build_dso.R")
if (!file.exists(helper)) helper <- file.path("tests", helper)
source(helper)
helper <- file.path("helpers", "internal_calls.R")
if (!file.exists(helper)) helper <- file.path("tests", helper)
source(helper)

catn("compiling the charport wrapper consumer (R CMD SHLIB)")
dll <- compile_test_dso("charport_consumer.cpp", skip_label = "charport wrapper consumer")

stats     <- charvec_stats
consumer_symbol <- function(name) getNativeSymbolInfo(name, PACKAGE = dll[["name"]])
roundtrip <- function(x) .Call(consumer_symbol("C_consumer_reader_roundtrip"), x)
rebuild <- function(x, n_shards = 1L) {
  .Call(consumer_symbol("C_consumer_builder_from_reader"), x, as.integer(n_shards))
}
reserve_rebuild <- function(x, n_shards = 1L) {
  .Call(consumer_symbol("C_consumer_builder_reserve"), x, as.integer(n_shards))
}
builder_errors <- function() .Call(consumer_symbol("C_consumer_builder_errors"))

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

catn("charport::Reader agrees with R's character view")
inputs <- list(
  character(0),
  c("plain", NA, "", w_utf8[1:5], w_latin1[6:10], b),
  as_charvec(c(w_utf8[1:20], NA, "", b)),
  as.character(1:500)
)
x <- as_charvec(c("m", NA)); charport_materialize(x)
inputs <- c(inputs, list(x))
for (input in inputs) {
  stopifnot(marks_identical(roundtrip(input), as.character(input)))
}
expect_error_matching(roundtrip(1:3), "character")

catn("builder rebuilds a charvec input across shard counts")
input <- c(w_utf8[1:40], NA, "", w_latin1[41:80], b, NA)
x <- as_charvec(input)
ref <- input
for (k in c(0L, 1L, 2L, 3L, 8L)) {
  out <- rebuild(x, k)
  stopifnot(is_charvec(out))
  stopifnot(marks_identical(out, ref))
  stopifnot(!is.na(charport_class_of(out)))    # output is a real charvec class
}
stopifnot(!stats(x)$materialized)               # building never materialized the input

catn("builder stores every reader view verbatim (ascii/bytes/NA)")
plain <- c("abc", NA, "", b, "zz")
out <- rebuild(plain, 2L)
stopifnot(is_charvec(out), marks_identical(out, plain))

catn("latin1 views pass through the builder unchanged (no policy, no error)")
latin1_word <- w_latin1[which(Encoding(w_latin1) == "latin1")[1L]]
for (k in c(0L, 1L)) {
  out <- rebuild(c("a", latin1_word), k)
  stopifnot(is_charvec(out), marks_identical(out, c("a", latin1_word)))
}

catn("builder reserve() path rebuilds identically (serial and sharded)")
# the reserve path keeps whatever encoding the view carried, latin1 included
x <- as_charvec(c(w_utf8[1:40], NA, "", w_latin1[41:80], b, NA))
ref <- c(w_utf8[1:40], NA, "", w_latin1[41:80], b, NA)
for (k in c(0L, 1L, 3L, 8L)) {
  out <- reserve_rebuild(x, k)
  stopifnot(is_charvec(out), marks_identical(out, ref))
  stopifnot(!is.na(charport_class_of(out)))
}
stopifnot(identical(as.character(reserve_rebuild(as_charvec(character(0)), 2L)), character(0)))

catn("builder error contract (throwing set/reserve, single-shot finish, safe abandon)")
stopifnot(isTRUE(builder_errors()))

catn("builder edge cases")
out <- rebuild(charvec(), 1L)
stopifnot(is_charvec(out), length(out) == 0L)
out <- rebuild(character(0), 3L)                # more shards than elements
stopifnot(length(out) == 0L)
out <- rebuild(c(NA_character_, NA_character_), 2L)
out_stats <- stats(out)
stopifnot(out_stats$n_slices == 0, all(is.na(out)))
big <- strrep("q", 300000)                      # > 256 KiB slice cap, via a shard
out <- rebuild(as_charvec(c(big, "tail")), 1L)
stopifnot(identical(as.character(out), c(big, "tail")))

catn("built charvec serializes like any other")
x <- rebuild(as_charvec(c(w_utf8[1:10], NA, "", b)), 3L)
y <- unserialize(serialize(x, NULL))
stopifnot(is_charvec(y), marks_identical(x, y))

catn("property test: rebuild across random shard counts")
for (trial in 1:15) {
  k <- sample(0:8, 1)
  n <- sample(0:400, 1)
  input <- sample(c(w_utf8, w_latin1, NA, ""), n, replace = TRUE)
  x <- as_charvec(input)
  out <- rebuild(x, k)
  stopifnot(marks_identical(out, as.character(x)))
}

dyn.unload(dll[["path"]])
catn("charport wrapper tests passed")
