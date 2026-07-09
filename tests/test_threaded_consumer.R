# Real multithreading, without charport itself carrying thread flags:
# compile threaded_consumer.cpp at test time as a separate DSO (R CMD SHLIB
# with a test-local Makevars holding -pthread), the way a downstream package
# would consume the installed headers + R_GetCCallable symbols. If the
# toolchain can't build it, skip because the package's own build is untouched.
#
# Workers: each thread reads its range through the same reentrant Reader and
# writes its own ParallelBuilder shard index. 2 threads (CRAN core limit).

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

src <- "threaded_consumer.cpp"
words_file <- "words_utf8.txt"
if (!file.exists(words_file)) words_file <- file.path("tests", "words_utf8.txt")

catn("compiling the test-time consumer (R CMD SHLIB)")
dll <- compile_test_dso(src, c(
  "PKG_CXXFLAGS = -pthread",
  "PKG_LIBS = -pthread"
), label = "threaded consumer")
consumer_symbol <- function(name) getNativeSymbolInfo(name, PACKAGE = dll[["name"]])
rebuild2 <- function(x, n_threads = 2L) {
  .Call(consumer_symbol("C_consumer_threaded_rebuild"), x, as.integer(n_threads))
}

catn("consumer load-time ABI check passes")
stopifnot(isTRUE(.Call(consumer_symbol("C_consumer_abi_ok"))))

catn("threaded rebuild: 2 std::thread workers over shared reentrant Reader + shards")
w_utf8 <- readLines(words_file, encoding = "UTF-8", warn = FALSE)
w_latin1 <- iconv(w_utf8, "UTF-8", "latin1")
b <- rawToChar(as.raw(0xE9)); Encoding(b) <- "bytes"
set.seed(20260612)

big_input <- sample(c(w_utf8, w_latin1, NA, "", b), 20000L, replace = TRUE)
x <- as_charvec(big_input)
out <- rebuild2(x)
stopifnot(is_charvec(out))
stopifnot(!charvec_stats(x)$materialized)   # input never materialized
stopifnot(marks_identical(out, big_input))

catn("threaded rebuild edge cases")
stopifnot(length(rebuild2(charvec())) == 0L)           # n < threads
out <- rebuild2(as_charvec(c("a", NA)))
stopifnot(identical(as.character(out), c("a", NA)))

catn("worker errors are caught, joined, and re-raised")
worker_throws <- function() .Call(consumer_symbol("C_consumer_worker_throws"))
expect_error_matching(worker_throws(), "injected worker failure")

catn("repeated threaded builds agree (merge determinism)")
ref <- as.character(x)
for (i in 1:20) stopifnot(identical(as.character(rebuild2(x)), ref))

dyn.unload(dll[["path"]])
catn("threaded consumer tests passed")
