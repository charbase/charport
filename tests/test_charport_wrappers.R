# charport:: consumer-wrapper semantics, exercised by charport consuming its
# own public header through R_GetCCallable. Ordinary Reader acquisition and
# acquisition protected by this test consumer must agree. The charvec builder
# variants must reproduce any reader's content as a fresh charvec, with no
# CHARSXPs between the read and build paths.

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
catn("compiling the charport C consumer (R CMD SHLIB)")
c_dll <- compile_test_dso("charport_c_header.c", label = "charport C consumer")

stats     <- charvec_stats
consumer_symbol <- function(name) getNativeSymbolInfo(name, PACKAGE = dll[["name"]])
c_consumer_symbol <- function(name) {
  getNativeSymbolInfo(name, PACKAGE = c_dll[["name"]])
}
roundtrip <- function(x) .Call(consumer_symbol("C_consumer_reader_roundtrip"), x)
resolved_roundtrip <- function(x) {
  .Call(consumer_symbol("C_consumer_resolved_reader_roundtrip"), x)
}
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
growable_state <- function() .Call(consumer_symbol("C_consumer_growable_state"))
builder_errors <- function() .Call(consumer_symbol("C_consumer_builder_errors"))
c_from_views <- function(x) {
  .Call(c_consumer_symbol("C_charport_c_from_views"), x)
}
c_header_probe <- function() {
  .Call(c_consumer_symbol("C_charport_c_header_probe"))
}
c_abi_ok <- function() {
  .Call(c_consumer_symbol("C_charport_c_abi_ok"))
}
c_reader_roundtrip <- function(x) {
  .Call(c_consumer_symbol("C_charport_c_reader_roundtrip"), x)
}
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
stopifnot(isTRUE(c_abi_ok()))
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
  stopifnot(marks_identical(c_reader_roundtrip(input), as.character(input)))
  stopifnot(marks_identical(range_roundtrip(input), as.character(input)))
  stopifnot(marks_identical(index_roundtrip(input), rev(as.character(input))))
  expected_lengths <- nchar(as.character(input), type = "bytes", allowNA = TRUE)
  stopifnot(identical(reader_lengths(input), expected_lengths))
  stopifnot(identical(reader_range_lengths(input), expected_lengths))
  stopifnot(identical(reader_index_lengths(input), rev(expected_lengths)))
  stopifnot(identical(reader_byte_lengths(input), expected_lengths))
  stopifnot(length(reader_encodings(input)) == length(input))
}

# Explicitly protected resolution has the same values for every input class.
for (input in inputs) {
  stopifnot(marks_identical(resolved_roundtrip(input), as.character(input)))
}
expect_error_matching(roundtrip(1:3), "character")
expect_error_matching(c_reader_roundtrip(1:3), "character")

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
release_test_init_condition <- function(value) {
  invisible(.Call(
    consumer_symbol("C_consumer_release_test_init_condition"),
    value
  ))
}
release_test_access_status <- function(value) {
  invisible(.Call(
    consumer_symbol("C_consumer_release_test_access_status"),
    as.integer(value)
  ))
}
reader_open <- function(x) .Call(consumer_symbol("C_consumer_reader_open"), x)
reader_open_protected <- function(first, second) {
  .Call(
    consumer_symbol("C_consumer_reader_open_protected"),
    first,
    second
  )
}
reader_access_kind <- function(x, path) {
  .Call(
    consumer_symbol("C_consumer_reader_access_kind"),
    x,
    as.integer(path)
  )
}
reader_access_throw <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_access_throw"), x)
}
reader_access_recovers <- function(x) {
  .Call(consumer_symbol("C_consumer_reader_access_recovers"), x)
}
convert_current_exception_to_status <- function(kind) {
  .Call(
    consumer_symbol("C_consumer_convert_current_exception_to_status"),
    as.integer(kind)
  )
}
context_cpp_error <- function() .Call(consumer_symbol("C_consumer_context_cpp_error"))
reader_eval <- function(x, expression, environment = parent.frame()) {
  .Call(consumer_symbol("C_consumer_reader_eval"), x, expression, environment)
}
invisible(.Call(consumer_symbol("C_consumer_register_release_test")))
before <- release_test_count()
stopifnot(identical(
  as.character(c_reader_roundtrip(release_test_vector())),
  c("alpha", "beta")
))
stopifnot(identical(release_test_count(), before + 1L))
release_test_access_status(1L)
before <- release_test_count()
expect_error_matching(
  c_reader_roundtrip(release_test_vector()),
  "status 1"
)
release_test_access_status(0L)
stopifnot(identical(release_test_count(), before + 1L))
before <- release_test_count()
stopifnot(identical(read_scalar(release_test_vector()), "alpha"))
stopifnot(identical(release_test_count(), before + 1L))
before <- release_test_count()
stopifnot(identical(
  resolved_roundtrip(release_test_vector()),
  c("alpha", "beta")
))
stopifnot(identical(release_test_count(), before + 1L))

catn("consumer-owned R boundaries unwind Reader and Builder state")
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

catn("Rcpp and cpp11 adapt Reader construction and charvec conversion errors")
framework_dlls <- list()
framework_available <- c(
  Rcpp = requireNamespace("Rcpp", quietly = TRUE),
  cpp11 = requireNamespace("cpp11", quietly = TRUE)
)
for (framework in names(framework_available)) {
  if (!framework_available[[framework]]) {
    catn(sprintf("framework factory test skipped: %s is unavailable", framework))
    next
  }

  include_dir <- system.file("include", package = framework)
  framework_dlls[[framework]] <- compile_test_dso(
    paste0(tolower(framework), "_consumer.cpp"),
    sprintf('PKG_CPPFLAGS += -I"%s"', include_dir),
    paste0(framework, " Reader factory consumer")
  )
  framework_symbol <- getNativeSymbolInfo(
    paste0("C_", tolower(framework), "_charport_test"),
    PACKAGE = framework_dlls[[framework]][["name"]]
  )
  wrap_symbol <- getNativeSymbolInfo(
    paste0("C_", tolower(framework), "_charport_wrap_test"),
    PACKAGE = framework_dlls[[framework]][["name"]]
  )
  cleanup_symbol <- getNativeSymbolInfo(
    paste0("C_", tolower(framework), "_charport_cleanup_count"),
    PACKAGE = framework_dlls[[framework]][["name"]]
  )
  cleanup_count <- function() .Call(cleanup_symbol)

  cleanup_before <- cleanup_count()
  before <- release_test_count()
  out <- .Call(framework_symbol, release_test_vector())
  stopifnot(is_charvec(out), identical(as.character(out), c("alpha", "beta")))
  stopifnot(identical(release_test_count(), before + 1L))
  stopifnot(identical(cleanup_count(), cleanup_before + 1L))

  wrapped <- .Call(wrap_symbol)
  stopifnot(is_charvec(wrapped), identical(as.character(wrapped), "wrapped"))

  cleanup_before <- cleanup_count()
  err <- tryCatch(.Call(framework_symbol, 1:3), error = identity)
  stopifnot(grepl("character", conditionMessage(err)))
  stopifnot(identical(cleanup_count(), cleanup_before + 1L))

  release_test_init_condition(condition)
  cleanup_before <- cleanup_count()
  err <- tryCatch({
    .Call(framework_symbol, release_test_vector())
    NULL
  }, charport_test_error = identity, finally = release_test_init_condition(NULL))
  stopifnot(
    identical(err$code, 42L),
    identical(conditionMessage(err), "injected R error")
  )
  stopifnot(identical(cleanup_count(), cleanup_before + 1L))
}

catn("ordinary Reader construction follows the provider's R error")
release_test_init_condition(condition)
err <- tryCatch({
  reader_open(release_test_vector())
  NULL
}, charport_test_error = identity, finally = release_test_init_condition(NULL))
stopifnot(
  identical(err$code, 42L),
  identical(conditionMessage(err), "injected R error")
)

catn("a consumer boundary can protect acquisition while borrows are live")
unwind_probe_reset()
before <- release_test_count()
stopifnot(is.null(reader_open_protected(
  release_test_vector(),
  release_test_vector()
)))
stopifnot(
  identical(release_test_count(), before + 2L),
  identical(unwind_probe_count(), 1L)
)

unwind_probe_reset()
before <- release_test_count()
expect_error_matching(
  reader_open_protected(release_test_vector(), 1:3),
  "character"
)
stopifnot(
  identical(release_test_count(), before + 1L),
  identical(unwind_probe_count(), 1L)
)

catn("access statuses become C++ exceptions and release the Reader")
release_vector <- release_test_vector()
expected_kind <- c(`1` = 1L, `2` = 2L, `3` = 3L, `99` = 1L)
for (status in as.integer(names(expected_kind))) {
  release_test_access_status(status)
  before <- release_test_count()
  kind <- reader_access_kind(release_vector, 0L)
  release_test_access_status(0L)
  stopifnot(
    identical(kind, unname(expected_kind[[as.character(status)]])),
    identical(release_test_count(), before + 1L)
  )
}

release_test_access_status(3L)
expect_error_matching(
  reader_access_throw(release_vector),
  "charport Reader access is out of range"
)
release_test_access_status(0L)

release_test_access_status(1L)
for (path in 0:8) {
  before <- release_test_count()
  stopifnot(identical(reader_access_kind(release_vector, path), 1L))
  stopifnot(identical(release_test_count(), before + 1L))
}
expect_error_matching(reader_access_throw(release_vector), "charport Reader access failed")
release_test_access_status(0L)

before <- release_test_count()
stopifnot(isTRUE(reader_access_recovers(release_vector)))
stopifnot(identical(release_test_count(), before + 1L))
stopifnot(identical(
  vapply(0:4, convert_current_exception_to_status, integer(1)),
  c(0L, 1L, 2L, 3L, 1L)
))

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

catn("C bulk construction copies pointer, length and encoding arrays")
stopifnot(isTRUE(c_header_probe()))
c_input <- c("abc", NA, "", w_utf8[[1L]], w_latin1[[6L]], b)
out <- c_from_views(c_input)
stopifnot(is_charvec(out), marks_identical(out, c_input))
out <- c_from_views(character())
stopifnot(is_charvec(out), identical(as.character(out), character()))
expect_error_matching(
  .Call(c_consumer_symbol("C_charport_c_from_views_bad_length")),
  "negative length"
)
expect_error_matching(
  .Call(c_consumer_symbol("C_charport_c_from_views_bad_encoding")),
  "invalid encoding"
)
expect_error_matching(
  .Call(c_consumer_symbol("C_charport_c_from_views_null_arrays")),
  "must not be NULL"
)

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
stopifnot(isTRUE(growable_state()))
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

catn("string_Elt results stay rooted when held across allocations")
# The consumer stashes every STRING_ELT result in a C container (invisible to
# the GC) before storing them, as base R and stringi do with ordinary
# STRSXPs. paste0 at runtime + gc() keeps the strings out of any live STRSXP,
# so a fresh unrooted CHARSXP from Elt would be collected by the allocation
# in between (deterministic under gctorture).
x <- as_charvec(paste0("elt-root-", seq_len(20000), "-", sample(1e9, 20000)))
gc()
stopifnot(is_charvec(x), !charport_info(x)$is_materialized)
gctorture(TRUE)
held <- .Call(consumer_symbol("C_consumer_elt_hold_across_alloc"), x)
gctorture(FALSE)
stopifnot(identical(held, as.character(x)))

invisible(.Call(consumer_symbol("C_consumer_unregister_release_test")))
for (framework_dll in framework_dlls) {
  dyn.unload(framework_dll[["path"]])
}
dyn.unload(dll[["path"]])
catn("charport wrapper tests passed")
