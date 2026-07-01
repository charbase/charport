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

consumer_symbol <- function(name) getNativeSymbolInfo(name, PACKAGE = dll[["name"]])
roundtrip <- function(x) .Call(consumer_symbol("C_consumer_reader_roundtrip"), x)
reader_kind <- function(x) .Call(consumer_symbol("C_consumer_reader_kind"), x)
reader_capabilities <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_capabilities"), x)
}
register_release_test <- function() {
  invisible(.Call(consumer_symbol("C_consumer_register_release_test")))
}
unregister_release_test <- function() {
  invisible(.Call(consumer_symbol("C_consumer_unregister_release_test")))
}
release_test_vector <- function() {
  .Call(consumer_symbol("C_consumer_release_test_vector"))
}
release_test_count <- function() {
  .Call(consumer_symbol("C_consumer_release_test_count"))
}
K_PLAIN <- 0L
K_MATERIALIZED_ALTREP <- 1L
K_REGISTERED_ALTREP <- 2L
K_FALLBACK_ALTREP <- 3L

words_file <- "words_utf8.txt"
if (!file.exists(words_file)) words_file <- file.path("tests", "words_utf8.txt")
w_utf8 <- readLines(words_file, encoding = "UTF-8", warn = FALSE)
w_latin1 <- iconv(w_utf8, "UTF-8", "latin1")
b <- rawToChar(as.raw(0xE9)); Encoding(b) <- "bytes"

set.seed(20260610)

catn("C ABI symbols resolve from a downstream-style consumer")
stopifnot(isTRUE(.Call(consumer_symbol("C_consumer_abi_ok"))))

catn("registry state at load: charvec is registered by default")
reg <- charport_classes()
stopifnot(reg$n == 1L)
stopifnot(identical(reg$view_persistence, TRUE))
stopifnot(identical(reg$thread_safe_access, TRUE))
stopifnot(identical(reg$reentrant, TRUE))
x <- charvec("a", "b")
stopifnot(!is.na(charport_class_of(x)))
stopifnot(!charvec_stats(x)$materialized)   # class_of didn't touch data

catn("duplicate register: charvec registration throws")
expect_error_matching(register_charvec(), "already registered")
reg <- charport_classes()
stopifnot(reg$n == 1L)
stopifnot(identical(reg$view_persistence, TRUE))
stopifnot(identical(reg$thread_safe_access, TRUE))
stopifnot(identical(reg$reentrant, TRUE))

catn("class_of: class membership, no materialization")
x <- charvec("a", "b")
if (getRversion() >= "4.6.0") {
  stopifnot(identical(charport_class_of(x), "charport::charvec"))
} else {
  stopifnot(!is.na(charport_class_of(x)))
}
stopifnot(is.na(charport_class_of(letters)))
stopifnot(!charvec_stats(x)$materialized)   # class_of didn't touch data
charport_materialize(x)
stopifnot(!is.na(charport_class_of(x)))   # still claims the class
expect_error_matching(charport_class_of(1:3), "character")

catn("reader kind: plain, registered, materialized, and fallback")
stopifnot(identical(reader_kind(c("a", "b")), K_PLAIN))
stopifnot(identical(reader_capabilities(c("a", "b")), c(TRUE, FALSE, FALSE)))
x <- as_charvec(c("a", "b"))
stopifnot(identical(reader_kind(x), K_REGISTERED_ALTREP))
stopifnot(identical(reader_capabilities(x), c(TRUE, TRUE, TRUE)))
stopifnot(!charvec_stats(x)$materialized)
charport_materialize(x)
stopifnot(identical(reader_kind(x), K_MATERIALIZED_ALTREP))
stopifnot(identical(reader_capabilities(x), c(TRUE, FALSE, FALSE)))
x <- as_charvec(c("u", "v"))
unregister_charvec()
stopifnot(identical(reader_kind(x), K_FALLBACK_ALTREP))
stopifnot(identical(reader_capabilities(x), c(TRUE, FALSE, FALSE)))
register_charvec()

catn("reader equivalence: plain vector with mixed encodings and marks")
mixed <- c("plain", NA, "", w_utf8[1:5], w_latin1[6:10], b)
stopifnot(any(Encoding(mixed) == "latin1"))
stopifnot(marks_identical(roundtrip(mixed), mixed))

catn("reader equivalence: charvec is served by its class")
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
stopifnot(is.na(charport_class_of(x)))
stopifnot(identical(roundtrip(x), as.character(1:5000)))

catn("reader edge cases")
stopifnot(identical(roundtrip(character(0)), character(0)))
stopifnot(identical(roundtrip(charvec()), character(0)))
stopifnot(identical(roundtrip(c(NA_character_, NA_character_)), c(NA_character_, NA_character_)))
stopifnot(identical(roundtrip(as_charvec(c(NA_character_, NA_character_))),
                    c(NA_character_, NA_character_)))
expect_error_matching(roundtrip(1:3), "character")
expect_error_matching(roundtrip(NULL), "character")
expect_error_matching(roundtrip(list("a")), "character")

catn("unregister: charvec resolves through fallback; re-register restores")
n0 <- charport_classes()$n
x <- as_charvec(c("u", NA, "v"))
unregister_charvec()
stopifnot(charport_classes()$n == n0 - 1L)
stopifnot(is.na(charport_class_of(x)))
stopifnot(identical(roundtrip(x), c("u", NA, "v")))
stopifnot(charvec_stats(x)$materialized)    # the one-time fallback cost
register_charvec()
stopifnot(charport_classes()$n == n0)
y <- as_charvec(c("w", "z"))
stopifnot(!is.na(charport_class_of(y)))

catn("duplicate registration errors")
expect_error_matching(register_charvec(), "already registered")
stopifnot(charport_classes()$n == n0)

catn("reader release: per-reader state is released")
register_release_test()
on.exit(try(unregister_release_test(), silent = TRUE), add = TRUE)
z <- release_test_vector()
count0 <- release_test_count()
stopifnot(identical(reader_kind(z), K_REGISTERED_ALTREP))
stopifnot(identical(release_test_count(), count0 + 1L))
stopifnot(identical(roundtrip(z), c("alpha", "beta")))
stopifnot(identical(release_test_count(), count0 + 2L))
unregister_release_test()

catn("reader equivalence under serialization round trip")
x <- as_charvec(c(w_utf8[1:10], NA, ""))
y <- unserialize(serialize(x, NULL))
stopifnot(!is.na(charport_class_of(y)))
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

dyn.unload(dll[["path"]])
catn("registry/reader tests passed")
