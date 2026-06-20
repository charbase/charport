# Registry and reader broker semantics. Reader behavior is exercised through a
# test-local downstream DSO that uses only charport.h and R_GetCCallable.

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

expect_error_matching <- function(expr, pattern) {
  err <- tryCatch({expr; NULL}, error = identity)
  stopifnot(inherits(err, "error"))
  stopifnot(grepl(pattern, conditionMessage(err)))
}

catn("compiling the charport reader consumer (R CMD SHLIB)")
dll <- compile_test_dso("charport_consumer.cpp", skip_label = "charport reader consumer")
on.exit(dyn.unload(dll[["path"]]), add = TRUE)
on.exit(unregister_charvec_backend(), add = TRUE)

roundtrip <- function(x) .Call("C_consumer_reader_roundtrip", x)

words_file <- "words_utf8.txt"
if (!file.exists(words_file)) words_file <- file.path("tests", "words_utf8.txt")
w_utf8 <- readLines(words_file, encoding = "UTF-8", warn = FALSE)
w_latin1 <- iconv(w_utf8, "UTF-8", "latin1")
b <- rawToChar(as.raw(0xE9)); Encoding(b) <- "bytes"

set.seed(20260610)

catn("C ABI symbols resolve from a downstream-style consumer")
stopifnot(isTRUE(.Call("C_consumer_abi_ok")))

catn("registry state at load: no backends are registered by default")
unregister_charvec_backend()
reg <- charport_backends()
stopifnot(reg$n == 0L, is.logical(reg$reentrant), length(reg$reentrant) == 0L)
x <- charvec("a", "b")
stopifnot(is.na(charport_backend_of(x)))
stopifnot(!charvec_stats(x)$materialized)   # backend_of didn't touch data

catn("register: charvec backend is explicit and reentrant")
register_charvec_backend()
reg <- charport_backends()
stopifnot(reg$n == 1L, identical(reg$reentrant, TRUE))

catn("backend_of: class membership, no materialization")
x <- charvec("a", "b")
if (getRversion() >= "4.6.0") {
  stopifnot(identical(charport_backend_of(x), "charport::charvec"))
} else {
  stopifnot(!is.na(charport_backend_of(x)))
}
stopifnot(is.na(charport_backend_of(letters)))
stopifnot(!charvec_stats(x)$materialized)   # backend_of didn't touch data
charport_materialize(x)
stopifnot(!is.na(charport_backend_of(x)))   # still claims the class
expect_error_matching(charport_backend_of(1:3), "character")

catn("reader equivalence: plain vector with mixed encodings and marks")
mixed <- c("plain", NA, "", w_utf8[1:5], w_latin1[6:10], b)
stopifnot(any(Encoding(mixed) == "latin1"))
stopifnot(marks_identical(roundtrip(mixed), mixed))

catn("reader equivalence: charvec is served by its backend")
ref <- c(w_utf8[1:20], NA, "", w_latin1[21:40], b)
x <- as_charvec(ref)
stopifnot(marks_identical(roundtrip(x), ref))
stopifnot(!charvec_stats(x)$materialized)   # reading didn't materialize

catn("reader equivalence: materialized charvec falls back cleanly")
charport_materialize(x)
stopifnot(marks_identical(roundtrip(x), ref))

catn("reader equivalence: unregistered ALTREP is materialized by fallback")
x <- as.character(1:5000)
stopifnot(grepl("deferred string conversion", capture.output(.Internal(inspect(x)))[1]))
stopifnot(is.na(charport_backend_of(x)))
stopifnot(identical(roundtrip(x), as.character(1:5000)))

catn("reader edge cases")
register_charvec_backend()
stopifnot(identical(roundtrip(character(0)), character(0)))
stopifnot(identical(roundtrip(charvec()), character(0)))
stopifnot(identical(roundtrip(c(NA_character_, NA_character_)), c(NA_character_, NA_character_)))
stopifnot(identical(roundtrip(as_charvec(c(NA_character_, NA_character_))),
                    c(NA_character_, NA_character_)))
expect_error_matching(roundtrip(1:3), "character")
expect_error_matching(roundtrip(NULL), "character")
expect_error_matching(roundtrip(list("a")), "character")

catn("unregister: charvec resolves through fallback; re-register restores")
n0 <- charport_backends()$n
x <- as_charvec(c("u", NA, "v"))
unregister_charvec_backend()
stopifnot(charport_backends()$n == n0 - 1L)
stopifnot(is.na(charport_backend_of(x)))
stopifnot(identical(roundtrip(x), c("u", NA, "v")))
stopifnot(charvec_stats(x)$materialized)    # the one-time fallback cost
register_charvec_backend()
stopifnot(charport_backends()$n == n0)
y <- as_charvec(c("w", "z"))
stopifnot(!is.na(charport_backend_of(y)))

catn("re-registration is idempotent")
register_charvec_backend()
register_charvec_backend()
stopifnot(charport_backends()$n == n0)

catn("reader equivalence under serialization round trip")
x <- as_charvec(c(w_utf8[1:10], NA, ""))
y <- unserialize(serialize(x, NULL))
stopifnot(!is.na(charport_backend_of(y)))
stopifnot(marks_identical(roundtrip(y), roundtrip(x)))

catn("property test: reader output == STRING_ELT view of the same object")
for (trial in 1:20) {
  k <- sample(0:500, 1)
  input <- sample(c(w_utf8, w_latin1, NA, "", b), k, replace = TRUE)
  stopifnot(marks_identical(roundtrip(input), input))
  x <- as_charvec(input)
  out <- roundtrip(x)
  stopifnot(!charvec_stats(x)$materialized)
  stopifnot(marks_identical(out, input))
}

catn("registry/reader tests passed")
