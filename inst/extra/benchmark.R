# Run from the package root with the package installed (`make bench`).
# Optional argument: number of repetitions. The enwik8 corpus is cached in
# local/ and loaded in C++ into a static std::vector<std::string>. Fresh R
# workers are used for paths that create CHARSXPs so R's string cache starts
# cold for each repetition.

args <- commandArgs(trailingOnly = TRUE)
reps <- if (length(args) >= 1) as.integer(args[1]) else 5L
n_threads <- 4L

suppressMessages(library(charport))

# Corpus ---------------------------------------------------------------

dir.create("local", showWarnings = FALSE)
enwik8_txt <- file.path("local", "enwik8")
if (!file.exists(enwik8_txt)) {
  zip_path <- file.path("local", "enwik8.zip")
  download.file("https://mattmahoney.net/dc/enwik8.zip", zip_path, mode = "wb")
  unzip(zip_path, exdir = "local")
  file.remove(zip_path)
}

# Compile kernels ------------------------------------------------------

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
system2(file.path(R.home("bin"), "R"), c("CMD", "SHLIB", "benchmark.cpp"))
setwd(owd)
so <- file.path(build_dir, paste0("benchmark", .Platform$dynlib.ext))
dyn.load(so)

# Load corpus ----------------------------------------------------------

corpus_name <- "enwik8 lines"
corpus_file <- enwik8_txt
info <- .Call("C_prepare_data_for_benchmark", corpus_file)

ref <- info$hash
total_bytes <- info$total_bytes
cvec <- .Call("C_bench_charvec_Builder")
cat(sprintf("corpus: %s\n%s strings, %.1f MB, %d reps\n\n",
            corpus_name, format(info$n, big.mark = ","),
            total_bytes / 2^20, reps))

# Timing helpers -------------------------------------------------------

ms <- function(thunk) {
  t <- vapply(seq_len(reps), function(i) {
    gc(FALSE)
    start <- Sys.time()
    thunk()
    as.numeric(difftime(Sys.time(), start, units = "secs"))
  }, numeric(1))
  median(t) * 1000
}

# SET_STRING_ELT (baseline) -------------------------------------------

worker_SET_STRING_ELT_baseline <- file.path(build_dir, "worker_SET_STRING_ELT_baseline.R")
writeLines(c(
  'args <- commandArgs(trailingOnly = TRUE)',
  'suppressMessages(library(charport))',
  'dyn.load(args[1])',
  'invisible(.Call("C_prepare_data_for_benchmark", args[2]))',
  'start <- Sys.time()',
  'invisible(.Call("C_bench_SET_STRING_ELT"))',
  't <- as.numeric(difftime(Sys.time(), start, units = "secs"))',
  'cat(t, "\n")'
), worker_SET_STRING_ELT_baseline)
ms_SET_STRING_ELT_baseline <- function() {
  t <- vapply(seq_len(reps), function(i) {
    out <- system2(file.path(R.home("bin"), "Rscript"),
                   c(worker_SET_STRING_ELT_baseline, so, corpus_file),
                   stdout = TRUE)
    as.numeric(tail(out, 1))
  }, numeric(1))
  median(t) * 1000
}

# STRING_PTR_RO materialize (baseline) -------------------------------

worker_STRING_PTR_RO_materialize <- file.path(build_dir, "worker_STRING_PTR_RO_materialize.R")
writeLines(c(
  'args <- commandArgs(trailingOnly = TRUE)',
  'suppressMessages(library(charport))',
  'dyn.load(args[1])',
  'invisible(.Call("C_prepare_data_for_benchmark", args[2]))',
  'cvec <- .Call("C_bench_charvec_Builder")',
  'start <- Sys.time()',
  'invisible(.Call("C_bench_STRING_PTR_RO_hash", cvec))',
  't <- as.numeric(difftime(Sys.time(), start, units = "secs"))',
  'cat(t, "\n")'
), worker_STRING_PTR_RO_materialize)
ms_STRING_PTR_RO_materialize <- function() {
  t <- vapply(seq_len(reps), function(i) {
    out <- system2(file.path(R.home("bin"), "Rscript"),
                   c(worker_STRING_PTR_RO_materialize, so, corpus_file),
                   stdout = TRUE)
    as.numeric(tail(out, 1))
  }, numeric(1))
  median(t) * 1000
}

row <- function(name, t_ms, baseline = FALSE, label = name, color = NULL) {
  cat(sprintf("  %-46s %9.1f ms   %6.2f GB/s\n", name, t_ms,
              total_bytes / (t_ms / 1000) / 2^30))
  list(name = name, ms = t_ms,
       gbps = total_bytes / (t_ms / 1000) / 2^30, baseline = baseline,
       label = label, color = color)
}

# Construction ---------------------------------------------------------

cat("construction (input read from static C++ corpus):\n")
build_rows <- list(
  row("SET_STRING_ELT (baseline)", ms_SET_STRING_ELT_baseline(), baseline = TRUE,
      label = "SET_STRING_ELT\n(baseline)"),
  row("charport::charvec::Builder, serial",
      ms(function() .Call("C_bench_charvec_Builder")),
      label = "charvec::Builder\n(1 thread)"),
  row(sprintf("charport::charvec::ParallelBuilder, %d threads", n_threads),
      ms(function() .Call("C_bench_charvec_ParallelBuilder", n_threads)),
      label = sprintf("charvec::Builder\n(%d threads)", n_threads))
)

out <- .Call("C_bench_charvec_ParallelBuilder", n_threads)
stopifnot(identical(.Call("C_bench_charport_Reader_hash", out), ref))
rm(out)

plain <- .Call("C_bench_SET_STRING_ELT")

# Read path ------------------------------------------------------------

cat("\nread path (FNV-1a over every element):\n")
h1 <- .Call("C_bench_STRING_PTR_RO_hash", plain)
read_rows <- list(
  row("STRING_PTR_RO materialize, unmaterialized charvec (baseline)",
      ms_STRING_PTR_RO_materialize(), baseline = TRUE,
      label = "STRING_PTR_RO\nmaterialize\n(baseline)"),
  row("charport::Reader range byteviews, charvec, 1 thread",
      ms(function() .Call("C_bench_charport_Reader_hash", cvec)),
      label = "charport::Reader\ncharvec, 1 thread"),
  row(sprintf("charport::Reader range byteviews, charvec, %d threads", n_threads),
      ms(function() .Call("C_bench_charport_Reader_hash_threads", cvec, n_threads)),
      label = sprintf("charport::Reader\ncharvec, %d threads", n_threads))
)
stopifnot(identical(h1, ref),
          identical(.Call("C_bench_charport_Reader_hash_scalar", cvec), ref),
          identical(.Call("C_bench_charport_Reader_hash_block1", cvec), ref),
          identical(.Call("C_bench_charport_Reader_hash", plain), ref),
          identical(.Call("C_bench_charport_Reader_hash_scalar", plain), ref),
          identical(.Call("C_bench_charport_Reader_hash_block1", plain), ref),
          identical(.Call("C_bench_charport_Reader_hash_threads", cvec, n_threads), ref),
          identical(.Call("C_bench_STRING_PTR_RO_hash",
                          .Call("C_bench_charvec_Builder")), ref))

# Additional measurements ---------------------------------------------

cat("\nadditional measurements (not plotted):\n")
extra_rows <- list(
  row("STRING_PTR_RO hash, base R vector",
      ms(function() .Call("C_bench_STRING_PTR_RO_hash", plain)), baseline = TRUE),
  row("charport::Reader scalar byteview hash, base R vector",
      ms(function() .Call("C_bench_charport_Reader_hash_scalar", plain))),
  row("charport::Reader block size 1 byteview hash, base R vector",
      ms(function() .Call("C_bench_charport_Reader_hash_block1", plain))),
  row("charport::Reader range byteviews hash, base R vector",
      ms(function() .Call("C_bench_charport_Reader_hash", plain))),
  row("charport::Reader scalar byteview hash, charvec",
      ms(function() .Call("C_bench_charport_Reader_hash_scalar", cvec))),
  row("charport::Reader block size 1 byteview hash, charvec",
      ms(function() .Call("C_bench_charport_Reader_hash_block1", cvec))),
  row("charport::Reader range byteviews hash, charvec",
      ms(function() .Call("C_bench_charport_Reader_hash", cvec)))
)

# Access overhead ------------------------------------------------------

cat("\naccess path only (sum of lengths, no byte work):\n")
s1 <- .Call("C_probe_STRING_PTR_RO_length_sum", plain)
access_rows <- list(
  row("STRING_PTR_RO length, base R vector (baseline)",
      ms(function() .Call("C_probe_STRING_PTR_RO_length_sum", plain)),
      baseline = TRUE),
  row("charport::Reader scalar length, base R vector",
      ms(function() .Call("C_probe_charport_Reader_length_sum", plain))),
  row("charport::Reader block size 1 length, base R vector",
      ms(function() .Call("C_probe_charport_Reader_length_sum_block1", plain))),
  row("charport::Reader range lengths, base R vector",
      ms(function() .Call("C_probe_charport_Reader_length_sum_range", plain))),
  row("charport::Reader scalar length, charvec",
      ms(function() .Call("C_probe_charport_Reader_length_sum", cvec))),
  row("charport::Reader block size 1 length, charvec",
      ms(function() .Call("C_probe_charport_Reader_length_sum_block1", cvec))),
  row("charport::Reader range lengths, charvec",
      ms(function() .Call("C_probe_charport_Reader_length_sum_range", cvec)))
)
stopifnot(identical(s1, .Call("C_probe_charport_Reader_length_sum", plain)),
          identical(s1, .Call("C_probe_charport_Reader_length_sum_block1", plain)),
          identical(s1, .Call("C_probe_charport_Reader_length_sum_range", plain)),
          identical(s1, .Call("C_probe_charport_Reader_length_sum", cvec)),
          identical(s1, .Call("C_probe_charport_Reader_length_sum_block1", cvec)),
          identical(s1, .Call("C_probe_charport_Reader_length_sum_range", cvec)))

rows_to_df <- function(section, rows, plotted) {
  do.call(rbind, lapply(rows, function(x) {
    data.frame(section = section, name = x$name, ms = x$ms, gbps = x$gbps,
               baseline = isTRUE(x$baseline), plotted = plotted)
  }))
}

benchmark_table <- do.call(rbind, list(
  rows_to_df("write path", build_rows, TRUE),
  rows_to_df("read path", read_rows, TRUE),
  rows_to_df("read path", extra_rows, FALSE),
  rows_to_df("access path", access_rows, FALSE)
))
dir.create("scratch", showWarnings = FALSE)
table_path <- file.path("scratch", "benchmark-table-current.csv")
utils::write.csv(benchmark_table, table_path, row.names = FALSE)
cat(sprintf("benchmark table written to %s\n", table_path))

cat("\nall hashes verified equal across paths\n")

# Plot -----------------------------------------------------------------

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
  name_cex <- if (length(rows) > 3) 1.04 else 1.18
  bp <- barplot(gbps, horiz = TRUE, names.arg = labels, col = cols,
                border = NA, xlab = "GB/s", main = title,
                xlim = c(0, max(gbps) * 1.18), cex.names = name_cex,
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
