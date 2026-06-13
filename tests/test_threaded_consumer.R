# Real multithreading, without charport itself carrying thread flags:
# compile threaded_consumer.cpp at test time as a separate DSO (R CMD SHLIB
# with a test-local Makevars holding -pthread), the way a downstream package
# would consume the installed headers + R_GetCCallable symbols. If the
# toolchain can't build it, skip -- the package's own build is untouched.
#
# Workers: each thread reads its range through a copy of the reader POD and
# writes its own BuilderMT shard index. 2 threads (CRAN core limit).

suppressPackageStartupMessages(library(charport))

catn <- function(...) cat(..., "\n")

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
if (!file.exists(src)) src <- file.path("tests", "threaded_consumer.cpp")
words_file <- "words_utf8.txt"
if (!file.exists(words_file)) words_file <- file.path("tests", "words_utf8.txt")

catn("compiling the test-time consumer (R CMD SHLIB)")
include_dir <- system.file("include", package = "charport")
stopifnot(nzchar(include_dir))
build_dir <- tempfile("threaded_consumer_build_")
dir.create(build_dir)
file.copy(src, file.path(build_dir, "threaded_consumer.cpp"))
writeLines(c(
  sprintf('PKG_CPPFLAGS = -I"%s"', include_dir),
  "PKG_CXXFLAGS = -pthread",
  "PKG_LIBS = -pthread"
), file.path(build_dir, "Makevars"))

old_wd <- setwd(build_dir)
status <- system2(file.path(R.home("bin"), "R"),
                  c("CMD", "SHLIB", "threaded_consumer.cpp"),
                  stdout = "shlib_out.txt", stderr = "shlib_out.txt")
setwd(old_wd)

so_path <- file.path(build_dir, paste0("threaded_consumer", .Platform$dynlib.ext))
if (status != 0L || !file.exists(so_path)) {
  cat(readLines(file.path(build_dir, "shlib_out.txt"), warn = FALSE), sep = "\n")
  catn("SKIP: toolchain could not build the threaded test consumer")
  quit(save = "no", status = 0L)
}

# No on.exit here: at source()-top-level it would fire as soon as this
# expression's eval frame exits, unloading the DSO immediately. The explicit
# dyn.unload at the bottom (or process exit, on failure) is the cleanup.
dll <- dyn.load(so_path, local = FALSE)
rebuild2 <- function(x, n_threads = 2L) {
  .Call("C_consumer_threaded_rebuild", x, as.integer(n_threads))
}

catn("consumer load-time ABI check passes")
stopifnot(isTRUE(.Call("C_consumer_abi_ok")))

catn("threaded rebuild: 2 std::thread workers over reader POD copies + shards")
w_utf8 <- readLines(words_file, encoding = "UTF-8", warn = FALSE)
w_latin1 <- iconv(w_utf8, "UTF-8", "latin1")
b <- rawToChar(as.raw(0xE9)); Encoding(b) <- "bytes"
set.seed(20260612)

big_input <- sample(c(w_utf8, w_latin1, NA, "", b), 20000L, replace = TRUE)
x <- as_charvec(big_input)
out <- rebuild2(x)
stopifnot(is_charvec(out))
stopifnot(marks_identical(out, as.character(x)))
stopifnot(identical(charport:::charvec_encodings(out), charport:::charvec_encodings(x)))
stopifnot(!charport:::charvec_stats(x)$materialized)   # input never materialized

catn("threaded rebuild edge cases")
stopifnot(length(rebuild2(charvec())) == 0L)           # n < threads
out <- rebuild2(as_charvec(c("a", NA)))
stopifnot(identical(as.character(out), c("a", NA)))

catn("worker errors are caught, joined, and re-raised")
worker_throws <- function() .Call("C_consumer_worker_throws")
expect_error_matching(worker_throws(), "injected worker failure")

catn("repeated threaded builds agree (merge determinism)")
ref <- as.character(x)
for (i in 1:20) stopifnot(identical(as.character(rebuild2(x)), ref))

dyn.unload(so_path)
catn("threaded consumer tests passed")
