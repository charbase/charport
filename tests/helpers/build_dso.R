compile_test_dso <- function(src, extra_makevars = character(), label = src) {
  if (!file.exists(src)) src <- file.path("tests", src)
  stopifnot(file.exists(src))

  include_dir <- system.file("include", package = "charport")
  stopifnot(nzchar(include_dir))

  stem <- tools::file_path_sans_ext(basename(src))
  build_dir <- tempfile(paste0(stem, "_build_"))
  dir.create(build_dir)
  file.copy(src, file.path(build_dir, basename(src)), overwrite = TRUE)
  boundary_header <- "consumer-boundary.h"
  if (!file.exists(boundary_header)) {
    boundary_header <- file.path("tests", "consumer-boundary.h")
  }
  stopifnot(file.exists(boundary_header))
  file.copy(
    boundary_header,
    file.path(build_dir, "consumer-boundary.h"),
    overwrite = TRUE
  )
  writeLines(c(
    sprintf('PKG_CPPFLAGS = -I"%s"', include_dir),
    extra_makevars
  ), file.path(build_dir, "Makevars"))

  # R CMD check can leave a relative startup-file path in the environment even
  # after changing away from the directory that contains it. Preserve files we
  # can resolve; otherwise use a known empty file for the nested R process.
  empty_startup <- file.path(build_dir, "empty-startup")
  file.create(empty_startup)
  startup_vars <- c("R_ENVIRON_USER", "R_PROFILE_USER", "R_TESTS")
  child_env <- list()
  for (name in startup_vars) {
    path <- Sys.getenv(name, unset = NA_character_)
    if (!is.na(path) && nzchar(path) && file.exists(path)) {
      path <- normalizePath(path, winslash = "/", mustWork = TRUE)
    } else {
      path <- normalizePath(empty_startup, winslash = "/", mustWork = TRUE)
    }
    child_env[[name]] <- path
  }

  # Set them on this process rather than passing system2(env=). The two
  # platforms assemble that argument differently: the Unix method prepends
  # name=value to the command, where the shell reads it as an environment
  # assignment, but the Windows method appends it after the command, where
  # cmd.exe has no such syntax and R receives the strings as arguments. On
  # Windows the child therefore kept R CMD check's own relative R_TESTS and
  # failed to open 'startup.Rs' from build_dir.
  saved <- Sys.getenv(startup_vars, unset = NA_character_, names = TRUE)
  do.call(Sys.setenv, child_env)
  on.exit({
    restore <- saved[!is.na(saved)]
    if (length(restore)) do.call(Sys.setenv, as.list(restore))
    drop <- names(saved)[is.na(saved)]
    if (length(drop)) Sys.unsetenv(drop)
  }, add = TRUE)

  old_wd <- setwd(build_dir)
  status <- tryCatch(
    system2(file.path(R.home("bin"), "R"), c("CMD", "SHLIB", basename(src)),
            stdout = "shlib_out.txt", stderr = "shlib_out.txt"),
    finally = setwd(old_wd)
  )

  so_path <- file.path(build_dir, paste0(stem, .Platform$dynlib.ext))
  if (status != 0L || !file.exists(so_path)) {
    out <- file.path(build_dir, "shlib_out.txt")
    compiler_output <- if (file.exists(out)) {
      readLines(out, warn = FALSE)
    } else {
      "<compiler output file was not created>"
    }
    stop(paste(c(
      paste0(label, " build failed"),
      paste0("R CMD SHLIB status: ", status),
      paste0("build directory: ", build_dir),
      "compiler output follows:",
      compiler_output
    ), collapse = "\n"), call. = FALSE)
  }

  dyn.load(so_path, local = FALSE)
}
