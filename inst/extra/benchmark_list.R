# Per-vector overhead benchmark: a large list of small character vectors,
# conceptually as.list(corpus), as produced by split-like verbs. Run
# from the package root with the package installed:
#
#   Rscript inst/extra/benchmark_list.R [reps] [chunk] [max_lines]
#
# reps      timing repetitions (default 5)
# chunk     lines per list element (default 1 = as.list)
# max_lines cap on corpus lines, 0 = all (default 0)
#
# Per-element costs are amortized over a vector's strings; per-vector costs
# (store + ALTREP wrap + external pointer + finalizer on the build side, one
# resolve per vector on the read side) are paid once per list element, so
# chunk = 1 maximizes them. Memory is measured as the VmRSS delta of building
# the list in a fresh R session (native charvec memory is invisible to R's
# gc accounting, so RSS provides a common measure for both). Note the
# string-cache asymmetry on memory: plain CHARSXPs intern duplicates once,
# a charvec stores every occurrence.

args <- commandArgs(trailingOnly = TRUE)
reps <- if (length(args) >= 1) as.integer(args[1]) else 5L
chunk <- if (length(args) >= 2) as.integer(args[2]) else 1L
max_lines <- if (length(args) >= 3) as.integer(args[3]) else 0L

suppressMessages(library(charport))

# --- corpus file (same corpus and cache as benchmark.R) ----------------------

dir.create("local", showWarnings = FALSE)
enwik8_txt <- file.path("local", "enwik8")
if (!file.exists(enwik8_txt)) {
  zip_path <- file.path("local", "enwik8.zip")
  ok <- tryCatch({
    download.file("https://mattmahoney.net/dc/enwik8.zip", zip_path, mode = "wb")
    unzip(zip_path, exdir = "local")
    file.remove(zip_path)
    file.exists(enwik8_txt)
  }, error = function(e) FALSE, warning = function(w) FALSE)
  if (!ok) stop("enwik8 download failed; fetch it into local/ and rerun")
}

# If capped, materialize the first max_lines into a truncated corpus file so
# every path (including fresh-session workers) reads the identical corpus.
corpus_file <- enwik8_txt
if (max_lines > 0L) {
  corpus_file <- file.path("local", sprintf("enwik8_first_%d", max_lines))
  if (!file.exists(corpus_file)) {
    con_in <- file(enwik8_txt, "rb"); con_out <- file(corpus_file, "wb")
    writeLines(readLines(con_in, n = max_lines, encoding = "UTF-8"),
               con_out, useBytes = TRUE)
    close(con_in); close(con_out)
  }
}

# --- compile the kernels against the installed headers ----------------------

include_dir <- system.file("include", package = "charport")
build_dir <- file.path(tempdir(), "charport-bench-list")
dir.create(build_dir, showWarnings = FALSE)
invisible(file.copy(file.path("inst", "extra", c("benchmark.cpp", "benchmark_list.cpp")),
                    build_dir, overwrite = TRUE))
writeLines(c(
  sprintf("PKG_CPPFLAGS = -I\"%s\"", include_dir),
  "PKG_CXXFLAGS = -pthread",
  "PKG_LIBS = -pthread"
), file.path(build_dir, "Makevars"))

owd <- setwd(build_dir)
status <- system2(file.path(R.home("bin"), "R"),
                  c("CMD", "SHLIB", "-o", paste0("benchmark_list", .Platform$dynlib.ext),
                    "benchmark.cpp", "benchmark_list.cpp"),
                  stdout = TRUE, stderr = TRUE)
setwd(owd)
so <- file.path(build_dir, paste0("benchmark_list", .Platform$dynlib.ext))
if (!file.exists(so)) stop(paste(status, collapse = "\n"))
dyn.load(so)

# --- corpus ingested in C, string cache stays cold ---------------------------

cvec <- .Call("C_bench_read_lines_charvec", corpus_file, 1000L)
ref <- .Call("C_bench_reader", cvec)
total_bytes <- .Call("C_bench_sumlen_reader", cvec)
n_vectors <- ceiling(length(cvec) / chunk)
cat(sprintf("corpus: enwik8 lines%s\n%s strings, %.1f MB, %d NA\n",
            if (max_lines > 0L) sprintf(" (first %d)", max_lines) else "",
            format(length(cvec), big.mark = ","), total_bytes / 2^20,
            as.integer(ref[2])))
cat(sprintf("list shape: %s vectors of <= %d line(s) each, %d reps\n\n",
            format(n_vectors, big.mark = ","), chunk, reps))

# --- timing helpers (same discipline as benchmark.R) -------------------------

ms <- function(thunk) {
  t <- vapply(seq_len(reps), function(i) {
    gc(FALSE)
    unname(system.time(thunk())["elapsed"])
  }, numeric(1))
  median(t) * 1000
}

row <- function(name, t_ms, baseline = FALSE) {
  cat(sprintf("  %-46s %9.1f ms   %8.1f ns/vec   %6.2f GB/s\n", name, t_ms,
              t_ms * 1e6 / n_vectors, total_bytes / (t_ms / 1000) / 2^30))
  invisible(t_ms)
}

# mkCharLenCE timings need a fresh session per repetition: even after gc(),
# R's string cache keeps its grown table, so a warm session understates the
# fresh-string cost a real user pays.
worker_build <- file.path(build_dir, "worker_build_strsxp_list.R")
writeLines(c(
  'args <- commandArgs(trailingOnly = TRUE)',
  'suppressMessages(library(charport))',
  'dyn.load(args[1])',
  'cvec <- .Call("C_bench_read_lines_charvec", args[2], 1000L)',
  't <- system.time(.Call("C_benchl_build_strsxp_list", cvec, as.integer(args[3])))[["elapsed"]]',
  'cat(t, "\n")'
), worker_build)
ms_fresh <- function() {
  t <- vapply(seq_len(reps), function(i) {
    out <- system2(file.path(R.home("bin"), "Rscript"),
                   c(worker_build, so, corpus_file, chunk), stdout = TRUE)
    as.numeric(tail(out, 1))
  }, numeric(1))
  median(t) * 1000
}

# --- construction -------------------------------------------------------------

cat("construction (source read through cp::Reader on charvec):\n")
row("mkCharLenCE STRSXP list (baseline)", ms_fresh(), baseline = TRUE)
row("cp::charvec::Builder list",
    ms(function() .Call("C_benchl_build_charvec_list", cvec, chunk)))

# --- read ---------------------------------------------------------------------

lst_cv <- .Call("C_benchl_build_charvec_list", cvec, chunk)
lst_pl <- .Call("C_benchl_build_strsxp_list", cvec, chunk)

cat("\nread path (FNV-1a over every element of every vector):\n")
h_elt <- .Call("C_benchl_hash_string_elt", lst_pl)
row("STRING_ELT loops, STRSXP list (baseline)",
    ms(function() .Call("C_benchl_hash_string_elt", lst_pl)), baseline = TRUE)
row("cp::Reader per vector, STRSXP list",
    ms(function() .Call("C_benchl_hash_reader", lst_pl)))
row("cp::Reader per vector, charvec list",
    ms(function() .Call("C_benchl_hash_reader", lst_cv)))
stopifnot(identical(h_elt, ref),
          identical(.Call("C_benchl_hash_reader", lst_pl), ref),
          identical(.Call("C_benchl_hash_reader", lst_cv), ref))

cat("\naccess path only (sum of lengths, mostly per-vector overhead):\n")
s1 <- .Call("C_benchl_sumlen_elt", lst_pl)
row("STRING_ELT loops, STRSXP list (baseline)",
    ms(function() .Call("C_benchl_sumlen_elt", lst_pl)), baseline = TRUE)
row("cp::Reader per vector, STRSXP list",
    ms(function() .Call("C_benchl_sumlen_reader", lst_pl)))
row("cp::Reader per vector, charvec list",
    ms(function() .Call("C_benchl_sumlen_reader", lst_cv)))
stopifnot(identical(s1, .Call("C_benchl_sumlen_reader", lst_pl)),
          identical(s1, .Call("C_benchl_sumlen_reader", lst_cv)))
cat("\nall hashes verified equal across paths\n")

# --- memory (fresh session per measurement, Linux VmRSS) ----------------------

if (file.exists("/proc/self/status")) {
  worker_mem <- file.path(build_dir, "worker_mem_list.R")
  writeLines(c(
    'args <- commandArgs(trailingOnly = TRUE)',
    'suppressMessages(library(charport))',
    'dyn.load(args[1])',
    'vmrss_kb <- function() {',
    '  as.numeric(sub("VmRSS:\\\\s*(\\\\d+) kB", "\\\\1",',
    '    grep("VmRSS", readLines("/proc/self/status"), value = TRUE)))',
    '}',
    'cvec <- .Call("C_bench_read_lines_charvec", args[2], 1000L)',
    'kernel <- if (args[4] == "charvec") "C_benchl_build_charvec_list" else "C_benchl_build_strsxp_list"',
    'gc(FALSE); r0 <- vmrss_kb()',
    'lst <- .Call(kernel, cvec, as.integer(args[3]))',
    'gc(FALSE); r1 <- vmrss_kb()',
    'stopifnot(length(lst) > 0)',
    'cat(r1 - r0, "\n")'
  ), worker_mem)
  mem_kb <- function(kind) {
    out <- system2(file.path(R.home("bin"), "Rscript"),
                   c(worker_mem, so, corpus_file, chunk, kind), stdout = TRUE)
    as.numeric(tail(out, 1))
  }
  cat("\nmemory (RSS delta of building the list, fresh session):\n")
  for (kind in c("strsxp", "charvec")) {
    kb <- mem_kb(kind)
    cat(sprintf("  %-46s %9.1f MB   %8.1f B/vec\n",
                sprintf("%s list%s", kind, if (kind == "strsxp") " (baseline)" else ""),
                kb / 1024, kb * 1024 / n_vectors))
  }
  cat(sprintf("  (payload itself: %.1f MB, %.1f B/vec; plain list dedups repeated\n",
              total_bytes / 2^20, total_bytes / n_vectors))
  cat("   strings via R's string cache, charvec stores every occurrence)\n")
} else {
  cat("\nmemory: skipped (no /proc/self/status on this platform)\n")
}
