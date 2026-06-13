# charvec slice-store semantics: in-place vs relocating assignment, pointer
# stability, manual compaction, sharded construction merging. Uses the internal
# (non-API) hooks in R/charvec-internal.R to observe store state.
#
# Mutation model under test:
#   - a shrink or same-size write rewrites in place (pointer stable);
#   - a grow, or a write over an NA record, takes a fresh dedicated slice and
#     repoints only that record (every other record's pointer is untouched);
#   - bytes left unreferenced by grows/overwrites are NOT auto-reclaimed; a
#     manual compact() rewrites live payload exact-fit and moves pointers.
# stats() exposes only structure now: length, n_slices, materialized.

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

alloc   <- charport:::charvec_alloc
cassign <- charport:::charvec_assign
stats   <- charport:::charvec_stats
addr    <- charport:::charvec_element_addr
compact <- charport:::charvec_compact
encs    <- charport:::charvec_encodings
sharded <- charport:::charvec_build_sharded

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

catn("in-place shrink keeps the same slice and pointer")
x <- alloc(3)
cassign(x, 1, "hello")
stopifnot(stats(x)$n_slices == 1, identical(x[[1L]], "hello"))
a1 <- addr(x, 1)
cassign(x, 1, "hi")                               # shrink: rewritten in place
stopifnot(identical(addr(x, 1), a1))
stopifnot(stats(x)$n_slices == 1)                 # no new slice
stopifnot(identical(x[[1L]], "hi"))               # length honored, no stale tail

catn("a growing write relocates only that record")
cassign(x, 2, "world")
a2 <- addr(x, 2)
cassign(x, 1, "hello!")                           # grow elt 1: fresh dedicated slice
stopifnot(!identical(addr(x, 1), a1))             # relocated
stopifnot(identical(addr(x, 2), a2))              # other records untouched
stopifnot(identical(x[[1L]], "hello!"), identical(x[[2L]], "world"))

catn("NA and empty-string overwrites repoint the record")
cassign(x, 1, NA)
stopifnot(is.na(x[[1L]]))
cassign(x, 2, "")
cassign(x, 3, "")
stopifnot(identical(addr(x, 2), addr(x, 3)))      # all "" share one static byte
stopifnot(identical(as.character(x), c(NA, "", "")))
stopifnot(identical(encs(x), c(NA, "ascii", "ascii")))

catn("record pointers are stable while the store grows")
n <- 500L
words <- sample(c(w_utf8, w_latin1), n, replace = TRUE)
x <- alloc(n)
cassign(x, 1, words[[1L]])
a1 <- addr(x, 1)
for (i in 2:n) cassign(x, i, words[[i]])
stopifnot(identical(addr(x, 1), a1))              # writing other elements never moves elt 1
stopifnot(marks_identical(x, words))              # latin1/utf8 kept verbatim
stopifnot(stats(x)$n_slices >= 1)

catn("manual compaction reclaims unreferenced bytes and moves pointers")
n <- 100L
x <- alloc(n)
for (i in 1:n) cassign(x, i, strrep("a", 100))    # each first write: its own slice
slices_built <- stats(x)$n_slices
for (i in 1:n) cassign(x, i, strrep("b", 150))    # grow each: old bytes left behind
stopifnot(stats(x)$n_slices > slices_built)       # leaked slices accumulate
a_before <- addr(x, 1)
compact(x)
s <- stats(x)
stopifnot(s$n_slices == 1)                        # 100 * 150 bytes fit one block
stopifnot(!identical(addr(x, 1), a_before))       # compaction moves pointers
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
stopifnot(identical(encs(x)[1L], "latin1"))
stopifnot(identical(x[[1L]], latin1_word))
b <- rawToChar(as.raw(0xE9)); Encoding(b) <- "bytes"
cassign(x, 2, b)
stopifnot(identical(encs(x)[2L], "bytes"))

catn("internal hook error paths")
x <- alloc(3)
expect_error_matching(cassign(x, 0, "a"), "out of bounds")
expect_error_matching(cassign(x, 4, "a"), "out of bounds")
expect_error_matching(addr(x, 4), "out of bounds")
expect_error_matching(stats(letters), "must be a charvec")
charport_materialize(x)
stopifnot(stats(x)$materialized, is.na(stats(x)$n_slices))
expect_error_matching(addr(x, 1), "materialized")
expect_error_matching(compact(x), "materialized")
cassign(x, 1, "post")                             # routed to the materialized data
stopifnot(identical(x[[1L]], "post"), is_charvec(x))

catn("sharded construction merges shards correctly")
chunks <- list(c("a", "bb", NA), character(0), c("ccc", "", latin1_word))
x <- sharded(chunks)
ref <- c("a", "bb", NA, "ccc", "", latin1_word)
stopifnot(is_charvec(x), marks_identical(x, ref))
stopifnot(identical(encs(x), c("ascii", "ascii", NA, "ascii", "ascii", "latin1")))
y <- unserialize(serialize(x, NULL))
stopifnot(is_charvec(y), marks_identical(y, ref))

catn("sharded construction edge cases")
stopifnot(length(sharded(list())) == 0L)
stopifnot(length(sharded(list(character(0), character(0)))) == 0L)
x <- sharded(list(c(NA_character_, NA_character_)))
stopifnot(all(is.na(x)), stats(x)$n_slices == 0)
expect_error_matching(sharded(list(1:3)), "character")

catn("sharded construction matches single-threaded construction")
for (trial in 1:10) {
  n_chunks <- sample(1:8, 1)
  chunks <- lapply(seq_len(n_chunks), function(.) {
    k <- sample(0:300, 1)
    sample(c(w_utf8, w_latin1, NA, ""), k, replace = TRUE)
  })
  combined <- unlist(chunks, use.names = FALSE)
  if (is.null(combined)) combined <- character(0)
  x <- sharded(chunks)
  y <- as_charvec(combined)
  stopifnot(marks_identical(x, y))
  stopifnot(identical(encs(x), encs(y)))
}

catn("store tests passed")
