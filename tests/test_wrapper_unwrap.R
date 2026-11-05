# Base R wraps a shared vector of length >= 64 in a wrap_string ALTREP when an
# attribute is set on it. On R >= 4.6 the broker reads through the wrapper to a
# registered inner vector instead of letting STRING_PTR_RO duplicate and
# materialize it. That relies on undocumented base internals: data1 of a
# wrap_string is the whole wrapped vector, and the wrapper's strings are that
# vector's strings. These tests pin those assumptions and run against R-devel
# in CI, so a change in base fails here first.

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

helper <- file.path("helpers", "build_dso.R")
if (!file.exists(helper)) helper <- file.path("tests", helper)
source(helper)
helper <- file.path("helpers", "internal_calls.R")
if (!file.exists(helper)) helper <- file.path("tests", helper)
source(helper)

bytes_identical <- function(x, y) {
  x <- as.character(x)
  y <- as.character(y)
  identical(x, y) && identical(Encoding(x), Encoding(y)) &&
    identical(lapply(x, function(s) if (is.na(s)) NA else charToRaw(s)),
              lapply(y, function(s) if (is.na(s)) NA else charToRaw(s)))
}

words_file <- "words_utf8.txt"
if (!file.exists(words_file)) words_file <- file.path("tests", "words_utf8.txt")
w_utf8 <- readLines(words_file, encoding = "UTF-8", warn = FALSE)
w_latin1 <- iconv(w_utf8, "UTF-8", "latin1")
values <- c("plain", NA, "", w_utf8[1:100], w_latin1[101:150], NA, "")
n <- length(values)
nm <- paste0("n", seq_len(n))

catn("compiling consumers (R CMD SHLIB)")
dll <- compile_test_dso("charport_consumer.cpp", label = "charport wrapper consumer")
cpc_dll <- compile_test_dso("charpack_consumer.cpp", label = "charpack consumer")
consumer <- function(name, ...) {
  .Call(getNativeSymbolInfo(name, PACKAGE = dll[["name"]]), ...)
}
cpc <- function(name, ...) {
  .Call(getNativeSymbolInfo(name, PACKAGE = cpc_dll[["name"]]), ...)
}
wrapped <- function(y) cpc("C_cpc_wrapped", y)
same <- function(a, b) cpc("C_cpc_same", a, b)

read_all <- function(y) {
  out <- consumer("C_consumer_reader_roundtrip", y)
  stopifnot(identical(consumer("C_consumer_reader_range_roundtrip", y), out),
            identical(consumer("C_consumer_reader_index_roundtrip", y), rev(out)))
  out
}

# Is the vector still compact (never materialized)?
compact <- function(x) {
  if (is_charpack(x)) charpack_state(x) == "filled" else !charport_info(x)$is_materialized
}

unwrap_supported <- getRversion() >= "4.6.0"

# Ways of reaching a wrapper; each sets an attribute on a shared vector.
wrap_ways <- list(
  setNames = function(x) setNames(x, nm),
  names_on_copy = function(x) { y <- x; names(y) <- nm; y },
  attr_on_copy = function(x) { y <- x; attr(y, "label") <- "l"; y },
  attributes_on_copy = function(x) { y <- x; attributes(y) <- list(names = nm); y },
  structure = function(x) structure(x, class = "tagged"),
  class_on_copy = function(x) { y <- x; class(y) <- "tagged"; y },
  dim_on_copy = function(x) { y <- x; dim(y) <- c(1L, n); y },
  dimnames_on_copy = function(x) {
    y <- x
    dim(y) <- c(1L, n)
    z <- y
    dimnames(z) <- list("r", nm)
    z
  },
  levels_on_copy = function(x) { y <- x; levels(y) <- "a"; y }
)

# Inner vectors: registered and unmaterialized ones are read through the
# wrapper; the others must behave as before.
inners <- list(
  charvec = function() as_charvec(values),
  charpack = function() charpack_from_slices(values),
  plain = function() values,
  materialized_charvec = function() { x <- as_charvec(values); charport_materialize(x); x }
)

wrapped_ways <- character(0)
for (way in names(wrap_ways)) {
  for (inner in names(inners)) {
    x <- inners[[inner]]()
    y <- wrap_ways[[way]](x)
    w <- wrapped(y)
    if (is.null(w)) next
    wrapped_ways <- union(wrapped_ways, way)
    catn(sprintf("%s around %s", way, inner))
    stopifnot(same(w, x))  # data1 is the whole wrapped vector
    registered <- inner %in% c("charvec", "charpack")
    stopifnot(bytes_identical(read_all(y), values))
    stopifnot(same(wrapped(y), x))  # not duplicated by the read
    if (registered) {
      stopifnot(compact(x), identical(cpc("C_cpc_reader_caps", y), c(TRUE, TRUE)))
    }
  }
}
if (unwrap_supported) {
  stopifnot(all(c("setNames", "names_on_copy", "attr_on_copy") %in% wrapped_ways))
}
catn("wrapped by:", paste(wrapped_ways, collapse = ", "))

# What happens to the inner vector on the fallback path is up to base R, so
# only the values are checked there.
catn("unregistered inner vector: falls back")
x <- as.character(seq_len(n) + 0.5)  # deferred string
stopifnot(identical(charport_info(x)$altrep_class_name, "deferred_string"))
y <- setNames(x, nm)
stopifnot(identical(read_all(y), as.character(seq_len(n) + 0.5)))

catn("a write through the wrapper replaces data1; reads see the new strings")
x <- as_charvec(values)
y <- setNames(x, nm)
invisible(read_all(y))
y[1] <- "changed"
expected <- values
expected[1] <- "changed"
stopifnot(bytes_identical(read_all(y), expected), bytes_identical(read_all(x), values))
if (unwrap_supported) stopifnot(!same(wrapped(y), x))

catn("short vectors are not wrapped")
x <- as_charvec(values[1:10])
stopifnot(is.null(wrapped(setNames(x, nm[1:10]))))

catn("charpack inside a wrapper, unregistered: falls back")
unregister_charpack()
x <- charpack_from_slices(values)
y <- setNames(x, nm)
stopifnot(bytes_identical(read_all(y), values))
register_charpack()

catn("OK")
