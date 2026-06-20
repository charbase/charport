# charvec slice-store semantics: assignment, materialization-aware mutation,
# manual compaction, and encoding-mark preservation.
#
# Mutation model under test:
#   - a shrink or same-size write rewrites in place without adding a slice;
#   - a grow, or a write over an NA record, may take a fresh dedicated slice;
#   - bytes left unreferenced by grows/overwrites are not auto-reclaimed; a
#     manual compact() rewrites live payload exact-fit.
# stats() exposes only structure now: length, n_slices, materialized.

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

helper <- file.path("helpers", "internal_calls.R")
if (!file.exists(helper)) helper <- file.path("tests", helper)
source(helper)

alloc   <- charvec_alloc
cassign <- charvec_assign
stats   <- charvec_stats
compact <- charvec_compact

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
latin1_word <- w_latin1[which(Encoding(w_latin1) == "latin1")[1L]]

set.seed(20260609)

catn("allocation basics")
x <- alloc(3)
stopifnot(is_charvec(x), length(x) == 3L, all(is.na(x)))
s <- stats(x)
stopifnot(s$length == 3, s$n_slices == 0, !s$materialized)
stopifnot(length(alloc(0)) == 0L, length(alloc(1)) == 1L)

catn("in-place shrink keeps the same slice count")
x <- alloc(3)
cassign(x, 1, "hello")
stopifnot(stats(x)$n_slices == 1, identical(x[[1L]], "hello"))
cassign(x, 1, "hi")
stopifnot(stats(x)$n_slices == 1)                 # no new slice
stopifnot(identical(x[[1L]], "hi"))               # length honored, no stale tail

catn("a growing write preserves values")
cassign(x, 2, "world")
cassign(x, 1, "hello!")
stopifnot(identical(x[[1L]], "hello!"), identical(x[[2L]], "world"))

catn("NA and empty-string overwrites preserve values")
cassign(x, 1, NA)
stopifnot(is.na(x[[1L]]))
cassign(x, 2, "")
cassign(x, 3, "")
stopifnot(marks_identical(x, c(NA, "", "")))

catn("stored records survive while the store grows")
n <- 500L
words <- sample(c(w_utf8, w_latin1), n, replace = TRUE)
x <- alloc(n)
cassign(x, 1, words[[1L]])
for (i in 2:n) cassign(x, i, words[[i]])
stopifnot(marks_identical(x, words))              # latin1/utf8 kept verbatim
stopifnot(stats(x)$n_slices >= 1)

catn("manual compaction reclaims unreferenced bytes")
n <- 100L
x <- alloc(n)
for (i in 1:n) cassign(x, i, strrep("a", 100))    # each first write: its own slice
slices_built <- stats(x)$n_slices
for (i in 1:n) cassign(x, i, strrep("b", 150))    # grow each: old bytes left behind
stopifnot(stats(x)$n_slices > slices_built)       # leaked slices accumulate
compact(x)
s <- stats(x)
stopifnot(s$n_slices == 1)                        # 100 * 150 bytes fit one block
stopifnot(all(as.character(x) == strrep("b", 150)))
cassign(x, 1, "abc")                              # shrink over a compacted record, in place
stopifnot(identical(x[[1L]], "abc"), stats(x)$n_slices == 1)

catn("compacting a store with no live bytes drops every slice")
x <- alloc(3)
cassign(x, 1, "hello")
cassign(x, 1, NA)                                 # leaks the "hello" slice
cassign(x, 2, "")
stopifnot(stats(x)$n_slices >= 1)
compact(x)
stopifnot(stats(x)$n_slices == 0)
stopifnot(identical(as.character(x), c(NA, "", NA)))

catn("a string larger than the slice cap is stored whole")
x <- alloc(2)
big <- strrep("q", 300000)                        # > 256 KiB
cassign(x, 1, big)
stopifnot(stats(x)$n_slices == 1, identical(x[[1L]], big))
cassign(x, 2, "ab")                               # a second write is its own slice
stopifnot(stats(x)$n_slices == 2, identical(x[[2L]], "ab"))

catn("assignment keeps encodings verbatim like construction")
x <- alloc(2)
cassign(x, 1, latin1_word)
b <- rawToChar(as.raw(0xE9)); Encoding(b) <- "bytes"
cassign(x, 2, b)
stopifnot(marks_identical(x, c(latin1_word, b)))

catn("helper error paths")
x <- alloc(3)
expect_error_matching(cassign(x, 0, "a"), "out of bounds")
expect_error_matching(cassign(x, 4, "a"), "out of bounds")
expect_error_matching(stats(letters), "must be a charvec")
charport_materialize(x)
stopifnot(stats(x)$materialized, is.na(stats(x)$n_slices))
expect_error_matching(compact(x), "materialized")
cassign(x, 1, "post")                             # routed to the materialized data
stopifnot(identical(x[[1L]], "post"), is_charvec(x))

catn("store tests passed")
