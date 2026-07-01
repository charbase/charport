# Read/build benchmarks for charport. Run from the package root with the
# package installed (`make bench`). Optional argument: number of repetitions
# per timing (default 5). Writes the plot to man/figures/bench.png.
#
# Corpus: enwik8 (first 1e8 bytes of English Wikipedia XML; the standard
# Hutter Prize dataset), split into lines: ~1.1M strings, ~94 MB. Too big to
# ship in the package, so it is downloaded once and cached in local/ (git-
# and Rbuildignored).
#
# String-cache handling: Rf_mkCharLenCE interns every string in R's global
# string cache, and an already-interned string costs a hash lookup instead
# of an allocation. So if the corpus were read with readLines() first, the
# construction baseline would get cache hits and look much faster than real
# fresh-string ingest. gc() between repetitions is not enough to undo that:
# collected CHARSXPs leave the cache, but the cache's grown table stays, so
# the session no longer looks like a fresh user session. The benchmark avoids
# this in three ways:
# (1) the corpus is ingested in C, file -> charvec, so no CHARSXP is created
# in the parent session; (2) every repetition of the mkCharLenCE baseline
# runs in its own fresh R session (a worker script that ingests in C and
# times exactly one build); (3) every repetition of the STRING_ELT
# materialization read baseline also runs in a fresh R session. charvec does
# not use the string cache, so the builder and charport::Reader timings are
# run normally, in-session.

args <- commandArgs(trailingOnly = TRUE)
reps <- if (length(args) >= 1) as.integer(args[1]) else 5L
n_threads <- 4L

suppressMessages(library(charport))
charport_native_symbol <- function(name) {
  get(name, envir = asNamespace("charport"), inherits = FALSE)
}
invisible(.Call(charport_native_symbol("C_register_charvec")))

# --- corpus file ------------------------------------------------------------

dir.create("local", showWarnings = FALSE)
enwik8_txt <- file.path("local", "enwik8")
if (!file.exists(enwik8_txt)) {
  zip_path <- file.path("local", "enwik8.zip")
  download.file("https://mattmahoney.net/dc/enwik8.zip", zip_path, mode = "wb")
  unzip(zip_path, exdir = "local")
  file.remove(zip_path)
  if (!file.exists(enwik8_txt)) {
    stop("enwik8 download completed, but local/enwik8 was not created")
  }
}

# --- compile the kernels against the installed headers ----------------------

include_dir <- system.file("include", package = "charport")
build_dir <- file.path(tempdir(), "charport-bench")
dir.create(build_dir, showWarnings = FALSE)
invisible(file.copy(file.path("inst", "extra", "benchmark.cpp"), build_dir, overwrite = TRUE))
writeLines(c(
  sprintf("PKG_CPPFLAGS = -I\"%s\"", include_dir),
  "PKG_CXXFLAGS = -pthread",
  "PKG_LIBS = -pthread"
), file.path(build_dir, "Makevars"))

owd <- setwd(build_dir)
status <- system2(file.path(R.home("bin"), "R"), c("CMD", "SHLIB", "benchmark.cpp"),
                  stdout = TRUE, stderr = TRUE)
setwd(owd)
so <- file.path(build_dir, paste0("benchmark", .Platform$dynlib.ext))
if (!file.exists(so)) stop(paste(status, collapse = "\n"))
dyn.load(so)

# --- corpus as a file, ingested in C (no CHARSXPs in this session) ----------

corpus_name <- "enwik8 lines"
corpus_file <- enwik8_txt
cvec <- .Call("C_bench_read_lines_charvec", corpus_file, 1000L)

ref <- .Call("C_bench_reader", cvec)   # reference hash + NA count
total_bytes <- .Call("C_bench_sumlen_reader", cvec)
cat(sprintf("corpus: %s\n%s strings, %.1f MB, %d NA, %d reps\n\n",
            corpus_name, format(length(cvec), big.mark = ","),
            total_bytes / 2^20, as.integer(ref[2]), reps))

# --- timing ----------------------------------------------------------------

ms <- function(thunk) {
  t <- vapply(seq_len(reps), function(i) {
    gc(FALSE)
    unname(system.time(thunk())["elapsed"])
  }, numeric(1))
  median(t) * 1000
}

# One fresh R session per repetition: ingest in C, time exactly one build.
# A fresh session keeps the mkCharLenCE benchmark on new strings. Even after
# gc(), the string cache retains its grown table, so later repetitions in one
# session would measure a different cache state.
worker_script <- file.path(build_dir, "worker_build_strsxp.R")
writeLines(c(
  'args <- commandArgs(trailingOnly = TRUE)',
  'suppressMessages(library(charport))',
  'charport_native_symbol <- function(name) get(name, envir = asNamespace("charport"), inherits = FALSE)',
  'invisible(.Call(charport_native_symbol("C_register_charvec")))',
  'dyn.load(args[1])',
  'cvec <- .Call("C_bench_read_lines_charvec", args[2], 1000L)',
  't <- system.time(.Call("C_bench_build_strsxp", cvec))[["elapsed"]]',
  'cat(t, "\n")'
), worker_script)
ms_fresh <- function() {
  t <- vapply(seq_len(reps), function(i) {
    out <- system2(file.path(R.home("bin"), "Rscript"),
                   c(worker_script, so, corpus_file), stdout = TRUE)
    as.numeric(tail(out, 1))
  }, numeric(1))
  median(t) * 1000
}

# One fresh R session per repetition for STRING_ELT over unmaterialized charvec.
# This read baseline creates CHARSXPs through the ALTREP Elt method, so repeated
# timings in one session would mostly measure string-cache hits after the first
# run. The charport::Reader read rows below run in this benchmark process
# because they read charvec bytes without creating CHARSXPs.
worker_string_elt_charvec <- file.path(build_dir, "worker_string_elt_charvec.R")
writeLines(c(
  'args <- commandArgs(trailingOnly = TRUE)',
  'suppressMessages(library(charport))',
  'dyn.load(args[1])',
  'cvec <- .Call("C_bench_read_lines_charvec", args[2], 1000L)',
  't <- system.time(.Call("C_bench_string_elt", cvec))[["elapsed"]]',
  'cat(t, "\n")'
), worker_string_elt_charvec)
ms_fresh_string_elt_charvec <- function() {
  t <- vapply(seq_len(reps), function(i) {
    out <- system2(file.path(R.home("bin"), "Rscript"),
                   c(worker_string_elt_charvec, so, corpus_file), stdout = TRUE)
    as.numeric(tail(out, 1))
  }, numeric(1))
  median(t) * 1000
}
rows <- list()
row <- function(name, t_ms, baseline = FALSE, label = name, color = NULL) {
  cat(sprintf("  %-46s %9.1f ms   %6.2f GB/s\n", name, t_ms,
              total_bytes / (t_ms / 1000) / 2^30))
  list(name = name, ms = t_ms,
       gbps = total_bytes / (t_ms / 1000) / 2^30, baseline = baseline,
       label = label, color = color)
}

# Builder timings use freshly mapped pages and therefore include page faults
# and zeroing by the kernel. A session with warm pages available from earlier
# allocations may produce lower builder timings. Both construction paths here
# start from a cold allocation state.
cat("construction (input read through charport::Reader on charvec):\n")
build_rows <- list(
  row("SET_STRING_ELT (baseline)", ms_fresh(), baseline = TRUE,
      label = "SET_STRING_ELT\n(baseline)"),
  row("charport::charvec::Builder, serial",
      ms(function() .Call("C_bench_build_charvec", cvec, 0L)),
      label = "charvec::Builder\n(1 thread)"),
  row(sprintf("charport::charvec::ParallelBuilder, %d threads", n_threads),
      ms(function() .Call("C_bench_build_charvec", cvec, n_threads)),
      label = sprintf("charvec::Builder\n(%d threads)", n_threads))
)

# Construction correctness: the rebuilt charvec hashes identically.
out <- .Call("C_bench_build_charvec", cvec, n_threads)
stopifnot(identical(.Call("C_bench_reader", out), ref))
rm(out)

# Build the plain vector once for correctness and access-path checks.
plain <- .Call("C_bench_build_strsxp", cvec)

cat("\nread path (FNV-1a over every element):\n")
h1 <- .Call("C_bench_string_elt", plain)
read_rows <- list(
  row("STRING_ELT direct, unmaterialized charvec (baseline)",
      ms_fresh_string_elt_charvec(), baseline = TRUE,
      label = "STRING_ELT\nmaterialize\n(baseline)"),
  row("charport::Reader charvec, 1 thread",
      ms(function() .Call("C_bench_reader", cvec)),
      label = "charport::Reader\ncharvec, 1 thread"),
  row(sprintf("charport::Reader, charvec, %d threads", n_threads),
      ms(function() .Call("C_bench_reader_threaded", cvec, n_threads)),
      label = sprintf("charport::Reader\ncharvec, %d threads", n_threads))
)
stopifnot(identical(h1, ref),
          identical(.Call("C_bench_reader", plain), ref),
          identical(.Call("C_bench_string_elt", cvec), ref))

cat("\naccess path only (sum of lengths, no byte work):\n")
s1 <- .Call("C_bench_sumlen_elt", plain)
invisible(row("STRING_ELT loop, plain vector (baseline)",
              ms(function() .Call("C_bench_sumlen_elt", plain))))
invisible(row("charport::Reader, plain vector (direct path)",
              ms(function() .Call("C_bench_sumlen_reader", plain))))
invisible(row("charport::Reader, charvec (registered class)",
              ms(function() .Call("C_bench_sumlen_reader", cvec))))
stopifnot(identical(s1, .Call("C_bench_sumlen_reader", plain)),
          identical(s1, .Call("C_bench_sumlen_reader", cvec)))

cat("\nall hashes verified equal across paths\n")

# --- plot -------------------------------------------------------------------

plot_panel <- function(rows, title) {
  theme_bg <- "#26323d"
  theme_panel <- "#2f4147"
  theme_text <- "#f2ead9"
  theme_muted <- "#9aa5aa"
  theme_accent <- "#80cbc4"
  gbps <- rev(vapply(rows, `[[`, numeric(1), "gbps"))
  labels <- rev(vapply(rows, `[[`, character(1), "label"))
  cols <- rev(vapply(rows, function(r)
                       if (isTRUE(r$baseline)) theme_muted else theme_accent,
                     character(1)))
  par(bg = theme_bg)
  plot.new()
  usr <- par("usr")
  rect(usr[1], usr[3], usr[2], usr[4], col = theme_panel, border = NA)
  par(new = TRUE)
  bp <- barplot(gbps, horiz = TRUE, names.arg = labels, col = cols,
                border = NA, xlab = "GB/s", main = title,
                xlim = c(0, max(gbps) * 1.18), cex.names = 1.18,
                cex.axis = 1.05, cex.lab = 1.1, cex.main = 1.28,
                font.main = 2, col.axis = theme_text, col.lab = theme_text,
                col.main = theme_text, axes = TRUE)
  text(gbps, bp, sprintf("%.2f", gbps), pos = 4, cex = 1.02,
       xpd = NA, col = theme_text)
}

png_path <- if (dir.exists(file.path("man", "figures"))) {
  file.path("man", "figures", "bench.png")
} else {
  file.path("local", "bench.png")
}
png(png_path, width = 1700, height = 1350, res = 180)
par(mfrow = c(2, 1), mar = c(4.2, 11.2, 2.8, 1.4), mgp = c(2.4, 0.75, 0),
    las = 1, bg = "#26323d", fg = "#f2ead9")
plot_panel(read_rows, "read path (hash data)")
plot_panel(build_rows, "write path")
dev.off()
cat(sprintf("plot written to %s\n", png_path))
