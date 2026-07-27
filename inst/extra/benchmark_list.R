# Per-vector benchmark for a large list of small character vectors, as
# produced by split-like operations. Run from the package root after
# installing the current source:
#
#   Rscript inst/extra/benchmark_list.R [reps] [chunk] [max_lines]
#
# `chunk = 1` isolates singleton vectors. `max_lines = 0` uses all of enwik8;
# the Makefile target uses 100,000 lines so this remains a practical smoke
# benchmark. Construction and RSS measurements use fresh R processes; native
# construction is reported both cold and after one same-shape warm-up.

args <- commandArgs(trailingOnly = TRUE)
reps <- if (length(args) >= 1L) as.integer(args[[1L]]) else 5L
chunk <- if (length(args) >= 2L) as.integer(args[[2L]]) else 1L
max_lines <- if (length(args) >= 3L) as.integer(args[[3L]]) else 0L
stopifnot(!is.na(reps), reps >= 1L, !is.na(chunk), chunk >= 1L,
          !is.na(max_lines), max_lines >= 0L)

suppressMessages(library(charport))

# Corpus ----------------------------------------------------------------------

dir.create("local", showWarnings = FALSE)
enwik8 <- file.path("local", "enwik8")
if (!file.exists(enwik8)) {
  zip_path <- file.path("local", "enwik8.zip")
  download.file("https://mattmahoney.net/dc/enwik8.zip", zip_path, mode = "wb")
  unzip(zip_path, exdir = "local")
  unlink(zip_path)
}

corpus_file <- enwik8
if (max_lines > 0L) {
  corpus_file <- file.path("local", sprintf("enwik8_first_%d", max_lines))
  if (!file.exists(corpus_file)) {
    input <- file(enwik8, "rb")
    lines <- tryCatch(
      readLines(input, n = max_lines, warn = FALSE, encoding = "UTF-8"),
      finally = close(input)
    )
    writeLines(lines, corpus_file, useBytes = TRUE)
  }
}

# Compile against the installed package headers --------------------------------

include_dir <- system.file("include", package = "charport")
build_dir <- file.path(tempdir(), "charport-bench-list")
dir.create(build_dir, showWarnings = FALSE)
sources <- file.path("inst", "extra", c("benchmark.cpp", "benchmark_list.cpp"))
if (!all(file.copy(sources, build_dir, overwrite = TRUE))) {
  stop("could not copy benchmark sources")
}
writeLines(c(
  sprintf("PKG_CPPFLAGS = -I\"%s\"", include_dir),
  "PKG_CXXFLAGS = -pthread",
  "PKG_LIBS = -pthread"
), file.path(build_dir, "Makevars"))

old_dir <- setwd(build_dir)
compile_log <- system2(
  file.path(R.home("bin"), "R"),
  c("CMD", "SHLIB", "-o", paste0("benchmark_list", .Platform$dynlib.ext),
    "benchmark.cpp", "benchmark_list.cpp"),
  stdout = TRUE, stderr = TRUE
)
setwd(old_dir)
shared_object <- file.path(
  build_dir, paste0("benchmark_list", .Platform$dynlib.ext)
)
if (!file.exists(shared_object)) {
  stop(paste(compile_log, collapse = "\n"))
}
dyn.load(shared_object)

# Source vector ----------------------------------------------------------------

info <- .Call("C_prepare_data_for_benchmark", corpus_file)
source_charvec <- .Call("C_bench_charvec_Builder")
n_strings <- length(source_charvec)
n_vectors <- ceiling(n_strings / chunk)
total_bytes <- info$total_bytes

cat(sprintf(
  "corpus: enwik8%s\n%s strings, %.1f MB; %s vectors of <= %d string(s); %d reps\n\n",
  if (max_lines > 0L) sprintf(" (first %d lines)", max_lines) else "",
  format(n_strings, big.mark = ",", scientific = FALSE, trim = TRUE),
  total_bytes / 2^20,
  format(n_vectors, big.mark = ",", scientific = FALSE, trim = TRUE),
  chunk, reps
))

# Timing helpers ----------------------------------------------------------------

elapsed_ms <- function(thunk) {
  vapply(seq_len(reps), function(i) {
    gc(FALSE)
    unname(system.time(thunk())[["elapsed"]]) * 1000
  }, numeric(1))
}

show_timing <- function(name, times) {
  med <- median(times)
  cat(sprintf(
    "  %-43s %8.1f ms  %8.1f ns/vec  reps: %s\n",
    name, med, med * 1e6 / n_vectors,
    paste(format(round(times, 1), nsmall = 1), collapse = ", ")
  ))
  invisible(times)
}

# A fresh process keeps R's global CHARSXP cache cold for the plain arm and lets
# the native arms report allocator-cold and steady-state costs separately.
worker_build <- file.path(build_dir, "worker_build_list.R")
writeLines(c(
  "args <- commandArgs(trailingOnly = TRUE)",
  "suppressMessages(library(charport))",
  "dyn.load(args[[1L]])",
  "invisible(.Call('C_prepare_data_for_benchmark', args[[2L]]))",
  "source <- .Call('C_bench_charvec_Builder')",
  "chunk <- as.integer(args[[3L]])",
  "kind <- args[[4L]]",
  "warmup <- as.logical(as.integer(args[[5L]]))",
  "kernel <- switch(kind, strsxp = 'C_benchl_build_strsxp_list', builder = 'C_benchl_build_builder_list', scalar = 'C_benchl_build_scalar_list')",
  "call <- if (kind == 'scalar') quote(.Call(kernel, source)) else quote(.Call(kernel, source, chunk))",
  "if (warmup) { warm_result <- eval(call, envir = environment()); rm(warm_result); gc(FALSE) }",
  "gc(FALSE)",
  "time <- system.time(result <- eval(call, envir = environment()))[['elapsed']]",
  "stopifnot(length(result) == ceiling(length(source) / chunk))",
  "cat(time, '\\n')"
), worker_build)

fresh_times <- function(kind, warmup = FALSE) {
  vapply(seq_len(reps), function(i) {
    output <- system2(
      file.path(R.home("bin"), "Rscript"),
      c(worker_build, shared_object, corpus_file, chunk, kind,
        as.integer(warmup)),
      stdout = TRUE
    )
    as.numeric(tail(output, 1L)) * 1000
  }, numeric(1))
}

# Construction -----------------------------------------------------------------

cat("construction, cold process:\n")
build_plain <- show_timing(
  "mkCharLenCE STRSXP list (baseline)", fresh_times("strsxp")
)
build_builder <- show_timing(
  "charvec Builder list", fresh_times("builder")
)
build_scalar <- NULL
if (chunk == 1L) {
  build_scalar <- show_timing(
    "charvec Store::scalar list", fresh_times("scalar")
  )
}

cat("\nconstruction, after one same-shape native warm-up:\n")
build_builder_warm <- show_timing(
  "charvec Builder list", fresh_times("builder", warmup = TRUE)
)
build_scalar_warm <- NULL
if (chunk == 1L) {
  build_scalar_warm <- show_timing(
    "charvec Store::scalar list", fresh_times("scalar", warmup = TRUE)
  )
}

# Read and access ---------------------------------------------------------------

list_plain <- .Call("C_benchl_build_strsxp_list", source_charvec, chunk)
list_builder <- .Call("C_benchl_build_builder_list", source_charvec, chunk)
list_scalar <- if (chunk == 1L) {
  .Call("C_benchl_build_scalar_list", source_charvec)
} else {
  NULL
}

hash_plain <- .Call("C_benchl_hash_string_elt", list_plain)
stopifnot(
  identical(.Call("C_benchl_hash_reader", list_plain), hash_plain),
  identical(.Call("C_benchl_hash_reader", list_builder), hash_plain),
  identical(.Call("C_benchl_hash_resolved_reader", list_plain), hash_plain),
  identical(.Call("C_benchl_hash_resolved_reader", list_builder), hash_plain),
  is.null(list_scalar) ||
    identical(.Call("C_benchl_hash_reader", list_scalar), hash_plain),
  is.null(list_scalar) ||
    identical(.Call("C_benchl_hash_resolved_reader", list_scalar), hash_plain)
)

cat("\nread path (FNV-1a over every byte):\n")
read_plain <- show_timing(
  "STRING_ELT, STRSXP list (baseline)",
  elapsed_ms(function() .Call("C_benchl_hash_string_elt", list_plain))
)
read_plain_reader <- show_timing(
  "Reader per vector, STRSXP list",
  elapsed_ms(function() .Call("C_benchl_hash_reader", list_plain))
)
read_plain_resolved <- show_timing(
  "raw resolved Reader, STRSXP list",
  elapsed_ms(function() .Call("C_benchl_hash_resolved_reader", list_plain))
)
read_builder <- show_timing(
  "Reader per vector, Builder list",
  elapsed_ms(function() .Call("C_benchl_hash_reader", list_builder))
)
read_builder_resolved <- show_timing(
  "raw resolved Reader, Builder list",
  elapsed_ms(function() .Call("C_benchl_hash_resolved_reader", list_builder))
)
read_scalar <- NULL
read_scalar_resolved <- NULL
if (!is.null(list_scalar)) {
  read_scalar <- show_timing(
    "Reader per vector, Store::scalar list",
    elapsed_ms(function() .Call("C_benchl_hash_reader", list_scalar))
  )
  read_scalar_resolved <- show_timing(
    "raw resolved Reader, Store::scalar list",
    elapsed_ms(function() {
      .Call("C_benchl_hash_resolved_reader", list_scalar)
    })
  )
}

sum_plain <- .Call("C_benchl_sumlen_elt", list_plain)
stopifnot(
  identical(.Call("C_benchl_sumlen_reader", list_plain), sum_plain),
  identical(.Call("C_benchl_sumlen_reader", list_builder), sum_plain),
  identical(.Call("C_benchl_sumlen_resolved_reader", list_plain), sum_plain),
  identical(.Call("C_benchl_sumlen_resolved_reader", list_builder), sum_plain),
  is.null(list_scalar) ||
    identical(.Call("C_benchl_sumlen_reader", list_scalar), sum_plain),
  is.null(list_scalar) ||
    identical(.Call("C_benchl_sumlen_resolved_reader", list_scalar), sum_plain)
)

cat("\naccess path only (sum lengths):\n")
access_plain <- show_timing(
  "STRING_ELT, STRSXP list (baseline)",
  elapsed_ms(function() .Call("C_benchl_sumlen_elt", list_plain))
)
access_plain_reader <- show_timing(
  "Reader per vector, STRSXP list",
  elapsed_ms(function() .Call("C_benchl_sumlen_reader", list_plain))
)
access_plain_resolved <- show_timing(
  "raw resolved Reader, STRSXP list",
  elapsed_ms(function() .Call("C_benchl_sumlen_resolved_reader", list_plain))
)
access_builder <- show_timing(
  "Reader per vector, Builder list",
  elapsed_ms(function() .Call("C_benchl_sumlen_reader", list_builder))
)
access_builder_resolved <- show_timing(
  "raw resolved Reader, Builder list",
  elapsed_ms(function() .Call("C_benchl_sumlen_resolved_reader", list_builder))
)
access_scalar <- NULL
access_scalar_resolved <- NULL
if (!is.null(list_scalar)) {
  access_scalar <- show_timing(
    "Reader per vector, Store::scalar list",
    elapsed_ms(function() .Call("C_benchl_sumlen_reader", list_scalar))
  )
  access_scalar_resolved <- show_timing(
    "raw resolved Reader, Store::scalar list",
    elapsed_ms(function() {
      .Call("C_benchl_sumlen_resolved_reader", list_scalar)
    })
  )
}

cat("\nall hashes and length sums verified equal\n")

# RSS --------------------------------------------------------------------------

if (file.exists("/proc/self/status")) {
  worker_memory <- file.path(build_dir, "worker_memory_list.R")
  writeLines(c(
    "args <- commandArgs(trailingOnly = TRUE)",
    "suppressMessages(library(charport))",
    "dyn.load(args[[1L]])",
    "rss <- function() as.numeric(sub('VmRSS:\\\\s*(\\\\d+) kB', '\\\\1', grep('VmRSS', readLines('/proc/self/status'), value = TRUE)))",
    "invisible(.Call('C_prepare_data_for_benchmark', args[[2L]]))",
    "source <- .Call('C_bench_charvec_Builder')",
    "chunk <- as.integer(args[[3L]])",
    "kind <- args[[4L]]",
    "kernel <- switch(kind, strsxp = 'C_benchl_build_strsxp_list', builder = 'C_benchl_build_builder_list', scalar = 'C_benchl_build_scalar_list')",
    "gc(FALSE); before <- rss()",
    "result <- if (kind == 'scalar') .Call(kernel, source) else .Call(kernel, source, chunk)",
    "gc(FALSE); after <- rss()",
    "stopifnot(length(result) == ceiling(length(source) / chunk))",
    "cat(after - before, '\\n')"
  ), worker_memory)

  memory_kb <- function(kind) {
    output <- system2(
      file.path(R.home("bin"), "Rscript"),
      c(worker_memory, shared_object, corpus_file, chunk, kind),
      stdout = TRUE
    )
    as.numeric(tail(output, 1L))
  }

  cat("\nmemory (fresh-process RSS delta):\n")
  kinds <- c(strsxp = "STRSXP list (baseline)", builder = "charvec Builder list")
  if (chunk == 1L) {
    kinds <- c(kinds, scalar = "charvec Store::scalar list")
  }
  for (kind in names(kinds)) {
    kb <- memory_kb(kind)
    cat(sprintf(
      "  %-43s %8.1f MB  %8.1f B/vec\n",
      kinds[[kind]], kb / 1024, kb * 1024 / n_vectors
    ))
  }
}
