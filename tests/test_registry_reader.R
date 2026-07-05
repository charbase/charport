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
range_roundtrip <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_range_roundtrip"), x)
}
index_roundtrip <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_index_roundtrip"), x)
}
reader_capabilities <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_capabilities"), x)
}
consumer_info <- function(x) .Call(consumer_symbol("C_consumer_sexp_info"), x)
reader_lengths <- function(x) .Call(consumer_symbol("C_consumer_reader_lengths"), x)
reader_range_lengths <- function(x) .Call(consumer_symbol("C_consumer_reader_range_lengths"), x)
reader_index_lengths <- function(x) .Call(consumer_symbol("C_consumer_reader_index_lengths"), x)
reader_byte_lengths <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_byte_lengths"), x)
}
reader_encodings <- function(x) .Call(consumer_symbol("C_consumer_reader_encodings"), x)
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

expect_registered_class <- function(x) {
  info <- charport_info(x)
  stopifnot(isTRUE(info$is_registered))
  stopifnot(identical(info$altrep_class_name, "charvec"))
  stopifnot(identical(info$altrep_class_package, "charport"))
  stopifnot(identical(info$altrep_class, "charport::charvec"))
  stopifnot(identical(charport_class_of(x), "charport::charvec"))
}

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
stopifnot(identical(reg$persistent_views, TRUE))
stopifnot(identical(reg$concurrent_access, TRUE))
stopifnot(identical(reg$reentrant, TRUE))
x <- charvec("a", "b")
expect_registered_class(x)
stopifnot(!charvec_stats(x)$materialized)   # class_of didn't touch data

catn("duplicate register: charvec registration throws")
expect_error_matching(register_charvec(), "already registered")
reg <- charport_classes()
stopifnot(reg$n == 1L)
stopifnot(identical(reg$persistent_views, TRUE))
stopifnot(identical(reg$concurrent_access, TRUE))
stopifnot(identical(reg$reentrant, TRUE))

catn("class_of: class membership, no materialization")
x <- charvec("a", "b")
expect_registered_class(x)
stopifnot(is.na(charport_class_of(letters)))
stopifnot(!charvec_stats(x)$materialized)   # class_of didn't touch data
charport_materialize(x)
expect_registered_class(x)                 # still claims the class
expect_error_matching(charport_class_of(1:3), "character")

catn("sexp_info: plain, registered, materialized, deferred, and fallback")
info <- charport_info(c("a", "b"))
stopifnot(isTRUE(info$is_strsxp))
stopifnot(identical(info$length, 2))
stopifnot(!info$is_altrep)
stopifnot(info$is_materialized)
stopifnot(!info$is_registered)
stopifnot(!info$persistent_views)
stopifnot(!info$concurrent_access)
stopifnot(!info$reentrant)
stopifnot(identical(info$altrep_class, NA_character_))

cinfo <- consumer_info(c("a", "b"))
stopifnot(isTRUE(cinfo$is_strsxp))
stopifnot(identical(cinfo$length, 2))
stopifnot(!cinfo$is_altrep)
stopifnot(cinfo$is_materialized)
stopifnot(!cinfo$is_registered)
stopifnot(identical(reader_capabilities(c("a", "b")), c(TRUE, FALSE, FALSE)))

x <- as_charvec(c("a", "b"))
info <- charport_info(x)
stopifnot(isTRUE(info$is_strsxp))
stopifnot(identical(info$length, 2))
stopifnot(info$is_altrep)
stopifnot(!info$is_materialized)
stopifnot(info$is_registered)
stopifnot(info$persistent_views)
stopifnot(info$concurrent_access)
stopifnot(info$reentrant)
stopifnot(!info$stateful_reader)
expect_registered_class(x)
cinfo <- consumer_info(x)
stopifnot(isTRUE(cinfo$has_class_name))
stopifnot(isTRUE(cinfo$has_class_package))
stopifnot(identical(reader_capabilities(x), c(TRUE, TRUE, TRUE)))
stopifnot(!charvec_stats(x)$materialized)

charport_materialize(x)
info <- charport_info(x)
stopifnot(info$is_altrep)
stopifnot(info$is_materialized)
stopifnot(info$is_registered)
stopifnot(info$persistent_views)
stopifnot(info$concurrent_access)
stopifnot(info$reentrant)
stopifnot(identical(reader_capabilities(x), c(TRUE, FALSE, FALSE)))

x <- as.character(1:5000)
stopifnot(grepl("deferred string conversion", capture.output(.Internal(inspect(x)))[1]))
info <- charport_info(x)
stopifnot(isTRUE(info$is_strsxp))
stopifnot(identical(info$length, 5000))
stopifnot(info$is_altrep)
stopifnot(!info$is_materialized)
stopifnot(!info$is_registered)
stopifnot(!info$persistent_views)
stopifnot(!info$concurrent_access)
stopifnot(!info$reentrant)
stopifnot(!is.na(info$altrep_class_name))
stopifnot(!is.na(info$altrep_class_package))
stopifnot(grepl("deferred string conversion", capture.output(.Internal(inspect(x)))[1]))

x <- as_charvec(c("u", "v"))
unregister_charvec()
info <- charport_info(x)
stopifnot(info$is_altrep)
stopifnot(!info$is_materialized)
stopifnot(!info$is_registered)
stopifnot(!info$persistent_views)
stopifnot(!info$concurrent_access)
stopifnot(!info$reentrant)
stopifnot(!charvec_stats(x)$materialized)
stopifnot(identical(reader_capabilities(x), c(TRUE, FALSE, FALSE)))
register_charvec()

catn("reader equivalence: plain vector with mixed encodings and marks")
mixed <- c("plain", NA, "", w_utf8[1:5], w_latin1[6:10], b)
stopifnot(any(Encoding(mixed) == "latin1"))
stopifnot(marks_identical(roundtrip(mixed), mixed))
stopifnot(marks_identical(range_roundtrip(mixed), mixed))
stopifnot(marks_identical(index_roundtrip(mixed), rev(mixed)))
expected_lengths <- nchar(mixed, type = "bytes", allowNA = TRUE)
stopifnot(identical(reader_lengths(mixed), expected_lengths))
stopifnot(identical(reader_range_lengths(mixed), expected_lengths))
stopifnot(identical(reader_index_lengths(mixed), rev(expected_lengths)))
stopifnot(identical(reader_byte_lengths(mixed), expected_lengths))
stopifnot(reader_encodings(mixed)[is.na(mixed)] == 255L)

catn("reader equivalence: charvec is served by its class")
ref <- c(w_utf8[1:20], NA, "", w_latin1[21:40], b)
x <- as_charvec(ref)
stopifnot(marks_identical(roundtrip(x), ref))
stopifnot(marks_identical(range_roundtrip(x), ref))
stopifnot(marks_identical(index_roundtrip(x), rev(ref)))
expected_lengths <- nchar(ref, type = "bytes", allowNA = TRUE)
stopifnot(identical(reader_lengths(x), expected_lengths))
stopifnot(identical(reader_range_lengths(x), expected_lengths))
stopifnot(identical(reader_index_lengths(x), rev(expected_lengths)))
stopifnot(identical(reader_byte_lengths(x), expected_lengths))
stopifnot(identical(reader_encodings(x), reader_encodings(ref)))
stopifnot(!charvec_stats(x)$materialized)   # reading didn't materialize

catn("reader equivalence: materialized charvec falls back cleanly")
charport_materialize(x)
stopifnot(marks_identical(roundtrip(x), ref))

catn("reader equivalence: unregistered ALTREP is materialized by fallback")
x <- as.character(1:5000)
stopifnot(grepl("deferred string conversion", capture.output(.Internal(inspect(x)))[1]))
stopifnot(is.na(charport_class_of(x)))
stopifnot(identical(roundtrip(x), as.character(1:5000)))
stopifnot(identical(range_roundtrip(x), as.character(1:5000)))

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
expect_registered_class(y)

catn("duplicate registration errors")
expect_error_matching(register_charvec(), "already registered")
stopifnot(charport_classes()$n == n0)

catn("reader release: per-reader state is released")
register_release_test()
on.exit(try(unregister_release_test(), silent = TRUE), add = TRUE)
z <- release_test_vector()
count0 <- release_test_count()
info <- charport_info(z)
stopifnot(info$is_altrep)
stopifnot(!info$is_materialized)
stopifnot(info$is_registered)
stopifnot(!info$persistent_views)
stopifnot(!info$concurrent_access)
stopifnot(info$stateful_reader)
stopifnot(!info$reentrant)
stopifnot(identical(release_test_count(), count0))
cinfo <- consumer_info(z)
stopifnot(cinfo$is_registered)
stopifnot(cinfo$stateful_reader)
stopifnot(!cinfo$persistent_views)
stopifnot(!cinfo$concurrent_access)
stopifnot(!cinfo$reentrant)
stopifnot(identical(release_test_count(), count0))
stopifnot(identical(reader_capabilities(z), c(FALSE, FALSE, FALSE)))
stopifnot(identical(release_test_count(), count0 + 1L))
stopifnot(identical(roundtrip(z), c("alpha", "beta")))
stopifnot(identical(release_test_count(), count0 + 2L))
unregister_release_test()

catn("reader equivalence under serialization round trip")
x <- as_charvec(c(w_utf8[1:10], NA, ""))
y <- unserialize(serialize(x, NULL))
expect_registered_class(y)
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
