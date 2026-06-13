# Broker semantics: charport_resolve equivalence (reader output must be
# value- and mark-identical to STRING_ELT + CHAR over the same input, across
# plain vectors, charvec, materialized charvec, and unregistered ALTREP) and
# registry behavior (register/unregister, idempotent re-registration,
# fallback when the backend declines).

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

read_all  <- charport:::charport_read_all
rinfo     <- charport:::charport_reader_info
unreg     <- charport:::charport_test_unregister_charvec
rereg     <- charport:::charport_test_register_charvec
sharded   <- charport:::charvec_build_sharded

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

set.seed(20260610)

catn("C ABI symbols round-trip through R_GetCCallable")
stopifnot(isTRUE(charport:::charport_ccallable_check()))

catn("registry state at load: charvec is registered, reentrant")
reg <- charport_backends()
stopifnot(reg$n >= 1L, is.logical(reg$reentrant), length(reg$reentrant) == reg$n)
stopifnot(all(reg$reentrant))

catn("backend_of: class membership, no materialization")
x <- charvec("a", "b")
if (getRversion() >= "4.6.0") {
  stopifnot(identical(charport_backend_of(x), "charport::charvec"))
} else {
  stopifnot(!is.na(charport_backend_of(x)))
}
stopifnot(is.na(charport_backend_of(letters)))
stopifnot(!charport:::charvec_stats(x)$materialized)   # backend_of didn't touch data
charport_materialize(x)
stopifnot(!is.na(charport_backend_of(x)))              # still claims the class
expect_error_matching(charport_backend_of(1:3), "character")

catn("reader equivalence: plain vector with mixed encodings and marks")
# the direct path may surface latin1/native marks (only registered backends
# promise UTF-8): the reader must reproduce the input exactly, marks included
mixed <- c("plain", NA, "", w_utf8[1:5], w_latin1[6:10], b)
stopifnot(any(Encoding(mixed) == "latin1"))
info <- rinfo(mixed)
stopifnot(info$n == length(mixed), isTRUE(info$reentrant), info$path == "direct")
stopifnot(marks_identical(read_all(mixed), mixed))

catn("reader equivalence: charvec is served by its backend")
ref <- c(w_utf8[1:20], NA, "", w_latin1[21:40], b)   # latin1 kept as latin1
x <- as_charvec(ref)
info <- rinfo(x)
stopifnot(info$n == length(ref), isTRUE(info$reentrant), info$path == "backend")
stopifnot(marks_identical(read_all(x), ref))
stopifnot(!charport:::charvec_stats(x)$materialized)   # reading didn't materialize

catn("reader equivalence: materialized charvec falls back to direct (init returns NULL)")
charport_materialize(x)
info <- rinfo(x)
stopifnot(info$path == "direct", isTRUE(info$reentrant))
stopifnot(marks_identical(read_all(x), ref))

catn("reader equivalence: unregistered ALTREP is materialized once at resolve")
x <- as.character(1:5000)                              # deferred_string ALTREP
stopifnot(grepl("deferred string conversion", capture.output(.Internal(inspect(x)))[1]))
stopifnot(is.na(charport_backend_of(x)))               # ALTREP, but not registered
info <- rinfo(x)
stopifnot(info$path == "direct", isTRUE(info$reentrant))
stopifnot(identical(read_all(x), as.character(1:5000)))

catn("reader edge cases")
stopifnot(identical(read_all(character(0)), character(0)))
stopifnot(identical(read_all(charvec()), character(0)))
stopifnot(identical(read_all(c(NA_character_, NA_character_)), c(NA_character_, NA_character_)))
stopifnot(identical(read_all(as_charvec(c(NA_character_, NA_character_))),
                    c(NA_character_, NA_character_)))
expect_error_matching(rinfo(1:3), "character")
expect_error_matching(read_all(NULL), "character")
expect_error_matching(read_all(list("a")), "character")

catn("unregister: charvec resolves through the fallback; re-register restores")
n0 <- charport_backends()$n
x <- as_charvec(c("u", NA, "v"))
unreg()
stopifnot(charport_backends()$n == n0 - 1L)
stopifnot(is.na(charport_backend_of(x)))
info <- rinfo(x)                                       # forces materialization
stopifnot(info$path == "direct")
stopifnot(charport:::charvec_stats(x)$materialized)    # the one-time fallback cost
stopifnot(identical(read_all(x), c("u", NA, "v")))
rereg()
stopifnot(charport_backends()$n == n0)
y <- as_charvec(c("w", "z"))
stopifnot(rinfo(y)$path == "backend")

catn("re-registration is idempotent")
rereg()
rereg()
stopifnot(charport_backends()$n == n0)

catn("reader equivalence under serialization round trip")
x <- as_charvec(c(w_utf8[1:10], NA, ""))
y <- unserialize(serialize(x, NULL))
stopifnot(rinfo(y)$path == "backend")                  # unserialized charvec has a live store
stopifnot(marks_identical(read_all(y), read_all(x)))

catn("property test: reader output == STRING_ELT view of the same object")
for (trial in 1:20) {
  k <- sample(0:500, 1)
  input <- sample(c(w_utf8, w_latin1, NA, "", b), k, replace = TRUE)
  stopifnot(marks_identical(read_all(input), input))
  x <- as_charvec(input)
  stopifnot(marks_identical(read_all(x), as.character(x)))
  x <- sharded(list(input))
  stopifnot(marks_identical(read_all(x), as.character(x)))
}

catn("registry/reader tests passed")
