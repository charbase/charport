# charvec slice-store semantics: in-place vs relocating assignment, dead-byte
# accounting, compaction thresholds and pointer behavior, sharded
# construction merging. Uses the internal (non-API) hooks in
# R/charvec-internal.R to observe store state.
#
# Accounting invariant under test throughout:
#   allocated_bytes - dead_bytes == sum(byte length of live records)

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

live_bytes <- function(x) {
  v <- as.character(x)
  v <- v[!is.na(v)]
  if (length(v) == 0L) 0 else sum(nchar(v, type = "bytes"))
}

check_invariant <- function(x) {
  s <- stats(x)
  stopifnot(s$allocated_bytes - s$dead_bytes == live_bytes(x))
  invisible(s)
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

set.seed(20260609)

catn("allocation basics")
x <- alloc(3)
stopifnot(is_charvec(x), length(x) == 3L, all(is.na(x)))
s <- stats(x)
stopifnot(s$length == 3, s$allocated_bytes == 0, s$dead_bytes == 0, s$n_slices == 0)
stopifnot(!s$materialized)
stopifnot(length(alloc(0)) == 0L, length(alloc(1)) == 1L)

catn("initial slice size preallocation (rounded up to 64-byte alignment)")
s <- stats(alloc(4, 256))
stopifnot(s$n_slices == 1, s$tail_capacity == 256, s$tail_used == 0)
s <- stats(alloc(4, 100))
stopifnot(s$tail_capacity == 128)
s <- stats(alloc(4, 0))
stopifnot(s$n_slices == 0)

catn("in-place shrink: pointer stable, slack counted dead")
x <- alloc(3)
cassign(x, 1, "hello")
s <- check_invariant(x)
stopifnot(s$allocated_bytes == 5, s$n_slices == 1, s$tail_capacity == 64)
a1 <- addr(x, 1)
cassign(x, 1, "hi")
stopifnot(identical(addr(x, 1), a1))              # rewrote in place
s <- check_invariant(x)
stopifnot(s$dead_bytes == 3, s$allocated_bytes == 5)
stopifnot(identical(x[[1L]], "hi"))               # length honored, no stale tail

catn("growing assignment relocates and retires the old bytes")
cassign(x, 1, "hello!")
stopifnot(!identical(addr(x, 1), a1))
s <- check_invariant(x)
stopifnot(s$dead_bytes == 5, s$allocated_bytes == 11)
stopifnot(identical(x[[1L]], "hello!"))

catn("NA and empty-string overwrites retire live bytes")
cassign(x, 1, NA)
s <- check_invariant(x)
stopifnot(s$dead_bytes == 11, s$allocated_bytes == 11, is.na(x[[1L]]))
cassign(x, 2, "")
cassign(x, 3, "")
s <- check_invariant(x)
stopifnot(s$allocated_bytes == 11, s$dead_bytes == 11)
stopifnot(identical(addr(x, 2), addr(x, 3)))      # all "" share one static byte
stopifnot(identical(as.character(x), c(NA, "", "")))
stopifnot(identical(encs(x), c(NA, "ascii", "ascii")))

catn("compacting an all-dead store clears its slices")
compact(x)
s <- check_invariant(x)
stopifnot(s$allocated_bytes == 0, s$dead_bytes == 0, s$n_slices == 0)
stopifnot(identical(as.character(x), c(NA, "", "")))
cassign(x, 1, "abc")                              # store still usable
stopifnot(identical(x[[1L]], "abc"), stats(x)$allocated_bytes == 3)

catn("record pointers are stable while the store grows")
n <- 500L
words <- sample(c(w_utf8, w_latin1), n, replace = TRUE)
x <- alloc(n)
cassign(x, 1, words[[1L]])
a1 <- addr(x, 1)
for (i in 2:n) cassign(x, i, words[[i]])
stopifnot(identical(addr(x, 1), a1))              # blocks never move on append
stopifnot(marks_identical(x, enc2utf8(words)))
s <- check_invariant(x)
stopifnot(s$n_slices >= 1, s$dead_bytes == 0)

catn("manual compaction rewrites exact-fit and moves pointers")
n <- 100L
x <- alloc(n)
s100 <- strrep("a", 100)
s150 <- strrep("b", 150)
for (i in 1:n) cassign(x, i, s100)
stopifnot(stats(x)$allocated_bytes == 100 * 100)
for (i in 1:n) cassign(x, i, s150)
s <- check_invariant(x)
stopifnot(s$dead_bytes == 100 * 100, s$allocated_bytes == 100 * 250)
a_before <- addr(x, 1)
compact(x)
s <- check_invariant(x)
stopifnot(s$dead_bytes == 0, s$allocated_bytes == 100 * 150)
stopifnot(s$n_slices == 1, s$tail_used == 100 * 150, s$tail_capacity == 100 * 150)
stopifnot(!identical(addr(x, 1), a_before))
stopifnot(all(as.character(x) == s150))

catn("threshold compaction triggers automatically on relocating writes")
# dead >= 1 MiB AND dead >= half of live; crossing happens mid-loop
n <- 700L
big1 <- strrep("x", 2000)
big2 <- strrep("y", 2100)
x <- alloc(n)
for (i in 1:n) cassign(x, i, big1)
for (i in 1:n) cassign(x, i, big2)
s <- check_invariant(x)
stopifnot(s$allocated_bytes - s$dead_bytes == n * 2100)
stopifnot(s$dead_bytes < 1048576)                 # a compaction must have run
stopifnot(all(as.character(x) == big2))

catn("shrink slack alone arms the threshold; the next grow fires it")
n <- 600L
x <- alloc(n)
for (i in 1:n) cassign(x, i, strrep("x", 2000))
for (i in 1:n) cassign(x, i, "ss")                # all in place
s <- check_invariant(x)
stopifnot(s$dead_bytes == n * 1998, s$allocated_bytes == n * 2000)
cassign(x, 1, strrep("z", 10))                    # grow: compacts, then relocates
s <- check_invariant(x)
stopifnot(s$dead_bytes == 2)                      # just the pre-grow record
stopifnot(s$allocated_bytes == (n - 1) * 2 + 2 + 10)
stopifnot(identical(x[[1L]], strrep("z", 10)), identical(x[[2L]], "ss"))

catn("a string larger than the slice cap gets an exact slice")
x <- alloc(2)
big <- strrep("q", 300000)                        # > 256 KiB max_slice_bytes
cassign(x, 1, big)
s <- check_invariant(x)
stopifnot(s$n_slices == 1, s$tail_capacity == 300032)  # rounded to 64
cassign(x, 2, "ab")
s <- check_invariant(x)
stopifnot(s$n_slices == 1, s$allocated_bytes == 300002)
stopifnot(identical(x[[1L]], big))

catn("assignment normalizes encodings like construction")
x <- alloc(2)
latin1_word <- w_latin1[which(Encoding(w_latin1) == "latin1")[1L]]
cassign(x, 1, latin1_word)
stopifnot(identical(encs(x)[1L], "UTF-8"))
stopifnot(identical(x[[1L]], enc2utf8(latin1_word)))
b <- rawToChar(as.raw(0xE9)); Encoding(b) <- "bytes"
cassign(x, 2, b)
stopifnot(identical(encs(x)[2L], "bytes"))
check_invariant(x)

catn("internal hook error paths")
x <- alloc(3)
expect_error_matching(cassign(x, 0, "a"), "out of bounds")
expect_error_matching(cassign(x, 4, "a"), "out of bounds")
expect_error_matching(addr(x, 4), "out of bounds")
expect_error_matching(stats(letters), "must be a charvec")
charport_materialize(x)
stopifnot(stats(x)$materialized, is.na(stats(x)$allocated_bytes))
expect_error_matching(addr(x, 1), "materialized")
expect_error_matching(compact(x), "materialized")
cassign(x, 1, "post")                             # routed to the materialized data
stopifnot(identical(x[[1L]], "post"), is_charvec(x))

catn("sharded construction merges shards correctly")
chunks <- list(c("a", "bb", NA), character(0), c("ccc", "", latin1_word))
x <- sharded(chunks)
ref <- enc2utf8(c("a", "bb", NA, "ccc", "", latin1_word))
stopifnot(is_charvec(x), marks_identical(x, ref))
stopifnot(identical(encs(x), c("ascii", "ascii", NA, "ascii", "ascii", "UTF-8")))
s <- check_invariant(x)
stopifnot(s$dead_bytes == 0)
y <- unserialize(serialize(x, NULL))
stopifnot(is_charvec(y), marks_identical(y, ref))

catn("sharded construction edge cases")
stopifnot(length(sharded(list())) == 0L)
stopifnot(length(sharded(list(character(0), character(0)))) == 0L)
x <- sharded(list(c(NA_character_, NA_character_)))
stopifnot(all(is.na(x)), stats(x)$allocated_bytes == 0)
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
  check_invariant(x)
}

catn("store tests passed")
