# charport:: consumer-wrapper semantics, exercised by charport consuming its own
# public header through R_GetCCallable: charport::Reader must agree with the raw
# resolve loop, and charport::charvec::Builder / ParallelBuilder must reproduce any
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
dll <- compile_test_dso("charport_consumer.cpp", label = "charport wrapper consumer")

stats     <- charvec_stats
consumer_symbol <- function(name) getNativeSymbolInfo(name, PACKAGE = dll[["name"]])
roundtrip <- function(x) .Call(consumer_symbol("C_consumer_reader_roundtrip"), x)
range_roundtrip <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_range_roundtrip"), x)
}
index_roundtrip <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_index_roundtrip"), x)
}
rebuild <- function(x, n_shards = 1L) {
  .Call(consumer_symbol("C_consumer_builder_from_reader"), x, as.integer(n_shards))
}
reserve_rebuild <- function(x, n_shards = 1L) {
  .Call(consumer_symbol("C_consumer_builder_reserve"), x, as.integer(n_shards))
}
direct_build <- function() .Call(consumer_symbol("C_consumer_builder_direct"))
growable_build <- function(x) {
  .Call(consumer_symbol("C_consumer_growable_from_reader"), x)
}
builder_errors <- function() .Call(consumer_symbol("C_consumer_builder_errors"))
read_scalar <- function(x) .Call(consumer_symbol("C_consumer_read_scalar"), x)
build_scalar <- function(x) .Call(consumer_symbol("C_consumer_build_scalar"), x)
reader_lengths <- function(x) .Call(consumer_symbol("C_consumer_reader_lengths"), x)
reader_range_lengths <- function(x) .Call(consumer_symbol("C_consumer_reader_range_lengths"), x)
reader_index_lengths <- function(x) .Call(consumer_symbol("C_consumer_reader_index_lengths"), x)
reader_byte_lengths <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_byte_lengths"), x)
}
reader_encodings <- function(x) .Call(consumer_symbol("C_consumer_reader_encodings"), x)

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
  stopifnot(marks_identical(range_roundtrip(input), as.character(input)))
  stopifnot(marks_identical(index_roundtrip(input), rev(as.character(input))))
  expected_lengths <- nchar(as.character(input), type = "bytes", allowNA = TRUE)
  stopifnot(identical(reader_lengths(input), expected_lengths))
  stopifnot(identical(reader_range_lengths(input), expected_lengths))
  stopifnot(identical(reader_index_lengths(input), rev(expected_lengths)))
  stopifnot(identical(reader_byte_lengths(input), expected_lengths))
  stopifnot(length(reader_encodings(input)) == length(input))
}
expect_error_matching(roundtrip(1:3), "character")

catn("scalar reads and Store::scalar build one-element views")
x <- as_charvec(c("scalar", "tail"))
stopifnot(identical(read_scalar(x), "scalar"))
stopifnot(!stats(x)$materialized)
scalar_inputs <- c("plain", w_utf8[[1L]], w_latin1[[6L]], "", b, NA_character_)
for (value in scalar_inputs) {
  out <- build_scalar(c(value, "tail"))
  stopifnot(is_charvec(out), length(out) == 1L)
  stopifnot(marks_identical(out, value))
}
expect_error_matching(read_scalar(character(0)), "length at least 1")

catn("scalar helpers release state-owning registered ALTREP classes")
release_test_count <- function() .Call(consumer_symbol("C_consumer_release_test_count"))
release_test_vector <- function() .Call(consumer_symbol("C_consumer_release_test_vector"))
unwind_probe_count <- function() .Call(consumer_symbol("C_consumer_unwind_probe_count"))
unwind_probe_reset <- function() invisible(.Call(consumer_symbol("C_consumer_unwind_probe_reset")))
release_test_init_failure <- function(value) {
  invisible(.Call(consumer_symbol("C_consumer_release_test_init_failure"), value))
}
reader_open <- function(x) .Call(consumer_symbol("C_consumer_reader_open"), x)
context_cpp_error <- function() .Call(consumer_symbol("C_consumer_context_cpp_error"))
reader_eval <- function(x, expression, environment = parent.frame()) {
  .Call(consumer_symbol("C_consumer_reader_eval"), x, expression, environment)
}
invisible(.Call(consumer_symbol("C_consumer_register_release_test")))
before <- release_test_count()
stopifnot(identical(read_scalar(release_test_vector()), "alpha"))
stopifnot(identical(release_test_count(), before + 1L))

catn("R errors unwind Reader and Builder state")
condition <- structure(
  list(message = "injected R error", code = 42L),
  class = c("charport_test_error", "error", "condition")
)
condition_env <- list2env(list(condition = condition), parent = baseenv())
unwind_probe_reset()
before <- release_test_count()
err <- tryCatch(
  reader_eval(release_test_vector(), quote(stop(condition)), condition_env),
  charport_test_error = identity
)
stopifnot(identical(err$code, 42L), identical(conditionMessage(err), "injected R error"))
stopifnot(identical(release_test_count(), before + 1L), identical(unwind_probe_count(), 1L))

catn("Rcpp and cpp11 preserve R errors while unwinding Reader state")
framework_dlls <- list()
for (framework in c("Rcpp", "cpp11")) {
  if (!requireNamespace(framework, quietly = TRUE)) {
    catn(sprintf("framework unwind test skipped: %s is unavailable", framework))
    next
  }

  include_dir <- system.file("include", package = framework)
  framework_dlls[[framework]] <- compile_test_dso(
    paste0(tolower(framework), "_consumer.cpp"),
    sprintf('PKG_CPPFLAGS += -I"%s"', include_dir),
    paste0(framework, " unwind consumer")
  )
  framework_symbol <- getNativeSymbolInfo(
    paste0("C_", tolower(framework), "_charport_test"),
    PACKAGE = framework_dlls[[framework]][["name"]]
  )
  cleanup_symbol <- getNativeSymbolInfo(
    paste0("C_", tolower(framework), "_charport_cleanup_count"),
    PACKAGE = framework_dlls[[framework]][["name"]]
  )
  cleanup_count <- function() .Call(cleanup_symbol)

  cleanup_before <- cleanup_count()
  before <- release_test_count()
  err <- tryCatch(
    .Call(framework_symbol, release_test_vector(), quote(stop(condition)), condition_env),
    charport_test_error = identity
  )
  stopifnot(identical(err$code, 42L), identical(conditionMessage(err), "injected R error"))
  stopifnot(identical(release_test_count(), before + 1L))
  stopifnot(identical(cleanup_count(), cleanup_before + 1L))

  cleanup_before <- cleanup_count()
  before <- release_test_count()
  out <- .Call(framework_symbol, release_test_vector(), NULL, baseenv())
  stopifnot(is_charvec(out), identical(as.character(out), c("alpha", "beta")))
  stopifnot(identical(release_test_count(), before + 1L))
  stopifnot(identical(cleanup_count(), cleanup_before + 1L))

  cleanup_before <- cleanup_count()
  err <- tryCatch(.Call(framework_symbol, NULL, NULL, NULL), error = identity)
  stopifnot(grepl("injected builder C\\+\\+ error", conditionMessage(err)))
  stopifnot(identical(cleanup_count(), cleanup_before + 1L))

  cleanup_before <- cleanup_count()
  err <- tryCatch(.Call(framework_symbol, 1:3, NULL, baseenv()), error = identity)
  stopifnot(grepl("character", conditionMessage(err)))
  stopifnot(identical(cleanup_count(), cleanup_before + 1L))

  release_test_init_failure(1L)
  cleanup_before <- cleanup_count()
  err <- tryCatch(
    .Call(framework_symbol, release_test_vector(), NULL, baseenv()),
    error = identity,
    finally = release_test_init_failure(0L)
  )
  stopifnot(grepl("injected provider init R error", conditionMessage(err)))
  stopifnot(identical(cleanup_count(), cleanup_before + 1L))

  # hand-written r_boundary in the same TU: the framework backend's unwind
  # exception must continue the original R condition, not a generic error
  boundary_symbol <- getNativeSymbolInfo(
    paste0("C_", tolower(framework), "_charport_boundary"),
    PACKAGE = framework_dlls[[framework]][["name"]]
  )
  cleanup_before <- cleanup_count()
  before <- release_test_count()
  err <- tryCatch(
    .Call(boundary_symbol, release_test_vector(), quote(stop(condition)), condition_env),
    charport_test_error = identity
  )
  stopifnot(identical(err$code, 42L), identical(conditionMessage(err), "injected R error"))
  stopifnot(identical(release_test_count(), before + 1L))
  stopifnot(identical(cleanup_count(), cleanup_before + 1L))

  out <- .Call(boundary_symbol, release_test_vector(), quote("ok"), baseenv())
  stopifnot(identical(out, "ok"))
}

catn("R errors during provider init unwind consumer state")
unwind_probe_reset()
release_test_init_failure(1L)
err <- tryCatch(
  reader_open(release_test_vector()),
  error = identity,
  finally = release_test_init_failure(0L)
)
stopifnot(grepl("injected provider init R error", conditionMessage(err)))
stopifnot(identical(unwind_probe_count(), 1L))

catn("C++ errors return through R frames before conversion")
unwind_probe_reset()
expect_error_matching(context_cpp_error(), "injected context C\\+\\+ error")
stopifnot(identical(unwind_probe_count(), 1L))

catn("warnings promoted to errors unwind Reader and Builder state")
old_warn <- options(warn = 2L)
unwind_probe_reset()
before <- release_test_count()
err <- tryCatch(
  reader_eval(release_test_vector(), quote(warning("promoted warning")), baseenv()),
  error = identity,
  finally = options(old_warn)
)
stopifnot(inherits(err, "error"), grepl("promoted warning", conditionMessage(err)))
stopifnot(identical(release_test_count(), before + 1L), identical(unwind_probe_count(), 1L))

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

catn("direct Store payload and record access")
out <- direct_build()
stopifnot(is_charvec(out))
stopifnot(marks_identical(out, c("alpha", "beta", NA, "", "gamma", "gamma")))

catn("growable builder appends discovered-length output")
growable_input <- c("ascii", NA, "", w_utf8[[1L]], latin1_word, b, NA)
out <- growable_build(growable_input)
stopifnot(is_charvec(out), marks_identical(out, growable_input))
out <- growable_build(character(0))
stopifnot(is_charvec(out), length(out) == 0L, stats(out)$n_slices == 0)

catn("growable builder packs small strings into shared slices")
out <- growable_build(rep("x", 20000L))
stopifnot(length(out) == 20000L, all(out == "x"))
stopifnot(stats(out)$n_slices > 0, stats(out)$n_slices < 100)

catn("builder error contract (throwing set/reserve, safe abandon)")
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

catn("oversize strings are stored whole on every builder path")
# strings above the slice cap get an exact-fit slice; the cap only bounds
# the regular geometric slice size
mixed <- c(big, "s1", NA, strrep("w", 400000), "", "s2")
for (k in c(0L, 1L, 3L)) {
  out <- rebuild(mixed, k)                      # set() path
  stopifnot(is_charvec(out), marks_identical(out, mixed))
  out <- reserve_rebuild(mixed, k)              # reserve() path
  stopifnot(is_charvec(out), marks_identical(out, mixed))
}

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

invisible(.Call(consumer_symbol("C_consumer_unregister_release_test")))
for (framework_dll in framework_dlls) {
  dyn.unload(framework_dll[["path"]])
}
dyn.unload(dll[["path"]])
catn("charport wrapper tests passed")
