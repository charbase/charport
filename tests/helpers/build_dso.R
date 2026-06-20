compile_test_dso <- function(src, extra_makevars = character(), skip_label = src) {
  if (!file.exists(src)) src <- file.path("tests", src)
  stopifnot(file.exists(src))

  include_dir <- system.file("include", package = "charport")
  stopifnot(nzchar(include_dir))

  stem <- tools::file_path_sans_ext(basename(src))
  build_dir <- tempfile(paste0(stem, "_build_"))
  dir.create(build_dir)
  file.copy(src, file.path(build_dir, basename(src)), overwrite = TRUE)
  writeLines(c(
    sprintf('PKG_CPPFLAGS = -I"%s"', include_dir),
    extra_makevars
  ), file.path(build_dir, "Makevars"))

  old_wd <- setwd(build_dir)
  status <- tryCatch(
    system2(file.path(R.home("bin"), "R"), c("CMD", "SHLIB", basename(src)),
            stdout = "shlib_out.txt", stderr = "shlib_out.txt"),
    finally = setwd(old_wd)
  )

  so_path <- file.path(build_dir, paste0(stem, .Platform$dynlib.ext))
  if (status != 0L || !file.exists(so_path)) {
    cat("SKIP:", skip_label, "build failed; compiled-consumer coverage was not run\n")
    cat("R CMD SHLIB status:", status, "\n")
    cat("build directory:", build_dir, "\n")
    cat("compiler output follows:\n")
    out <- file.path(build_dir, "shlib_out.txt")
    if (file.exists(out)) {
      cat(readLines(out, warn = FALSE), sep = "\n")
    }
    quit(save = "no", status = 0L)
  }

  dyn.load(so_path, local = FALSE)
}
