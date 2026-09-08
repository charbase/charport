# Developer-only fault injection for package-owned Store construction.  The
# probe compiles the native source into a temporary DSO and is never installed.

suppressPackageStartupMessages(library(charport))

if (!identical(unname(Sys.info()[["sysname"]]), "Linux") ||
    !identical(.Machine$sizeof.pointer, 8L) ||
    !identical(.Machine$sizeof.long, 8L)) {
  cat("native cleanup probe skipped: requires Linux with 64-bit pointers and size_t\n")
  quit(save = "no", status = 0L)
}

package_root <- normalizePath(getwd(), winslash = "/", mustWork = TRUE)
helper <- file.path(package_root, "tests", "helpers", "build_dso.R")
source(helper)

src_dir <- normalizePath(file.path(package_root, "src"), winslash = "/", mustWork = TRUE)
wrap_flags <- paste(
  "-Wl,--wrap=_Znwm", "-Wl,--wrap=_Znam",
  "-Wl,--wrap=_ZdlPv", "-Wl,--wrap=_ZdaPv",
  "-Wl,--wrap=_ZdlPvm", "-Wl,--wrap=_ZdaPvm"
)
dll <- compile_test_dso(
  file.path(package_root, "tools", "native-cleanup", "native_cleanup_probe.cpp"),
  c(
    sprintf('PKG_CPPFLAGS += -I"%s"', src_dir),
    paste("PKG_LIBS +=", wrap_flags)
  ),
  "native Store cleanup probe"
)

symbol <- function(name) getNativeSymbolInfo(name, PACKAGE = dll[["name"]])
invoke <- function(name, args = list()) {
  do.call(.Call, c(list(symbol(name)), args))
}

expect_error <- function(fn, pattern) {
  err <- tryCatch(fn(), error = identity)
  stopifnot(inherits(err, "error"))
  stopifnot(grepl(pattern, conditionMessage(err)))
}

gc_until_clear <- function() {
  for (i in seq_len(3L)) gc()
  stopifnot(identical(invoke("native_cleanup_count"), 0L))
}

run_failures <- function(label, fn) {
  cat(label, "failure stages\n")
  for (stage in 1:3) {
    for (attempt in seq_len(4L)) {
      invisible(invoke("native_cleanup_set_failure", list(as.integer(stage))))
      expect_error(fn, "native cleanup probe: injected")
      stopifnot(identical(invoke("native_cleanup_count"), 0L))
    }
  }
}

run_success <- function(label, fn) {
  cat(label, "success/finalizer cleanup\n")
  invisible(invoke("native_cleanup_set_failure", list(0L)))
  value <- fn()
  stopifnot(invoke("native_cleanup_count") > 0L)
  invisible(invoke("native_cleanup_stop_tracking"))
  rm(value)
  gc_until_clear()
}

seed <- invoke("native_cleanup_as", list(c("alpha", "beta", NA_character_, "payload")))
stopifnot(is.character(seed), identical(as.character(seed),
                                        c("alpha", "beta", NA_character_, "payload")))
serialized <- invoke("native_cleanup_serialize", list(seed))
index <- 1:4

run_failures("C_charvec_alloc", function() {
  invoke("native_cleanup_alloc", list(4L))
})
run_failures("C_as_charvec", function() {
  invoke("native_cleanup_as", list(c("alpha", "payload", NA_character_)))
})
run_failures("bulk C constructor", function() {
  invoke("native_cleanup_bulk")
})
run_failures("Duplicate", function() {
  invoke("native_cleanup_duplicate", list(seed))
})
run_failures("Extract_subset", function() {
  invoke("native_cleanup_subset", list(seed, index))
})
run_failures("Unserialize", function() {
  invoke("native_cleanup_unserialize", list(serialized))
})

run_success("C_charvec_alloc", function() {
  invoke("native_cleanup_alloc", list(4L))
})
run_success("C_as_charvec", function() {
  invoke("native_cleanup_as", list(c("alpha", "payload", NA_character_)))
})
run_success("bulk C constructor", function() {
  invoke("native_cleanup_bulk")
})
run_success("Duplicate", function() {
  invoke("native_cleanup_duplicate", list(seed))
})
run_success("Extract_subset", function() {
  invoke("native_cleanup_subset", list(seed, index))
})
run_success("Unserialize", function() {
  invoke("native_cleanup_unserialize", list(serialized))
})

cat("native validation errors, including late partial ownership\n")
expect_native_error <- function(fn, pattern) {
  # native_cleanup_count() disables tracking; enable it again for every case.
  invisible(invoke("native_cleanup_set_failure", list(0L)))
  expect_error(fn, pattern)
  stopifnot(identical(invoke("native_cleanup_count"), 0L))
}

expect_native_error(
  function() invoke("native_cleanup_alloc", list(-1)), "invalid length"
)
expect_native_error(
  function() invoke("native_cleanup_as", list(1:3)), "character vector"
)
bad_state <- as.raw(c(0x43, 0x50, 0x56, 0x31, 0x01))
expect_native_error(
  function() invoke("native_cleanup_unserialize", list(bad_state)),
  "serialized_state is truncated"
)

# The first record is copied into Builder-owned storage before the second
# record's invalid encoding is rejected.  This exercises native cleanup after
# ownership has started, rather than only input validation before construction.
encoding_first <- 1L + 5L + 8L + 4L * length(index)
late_bad <- serialized
late_bad[encoding_first + 1L] <- as.raw(0x7f)
expect_native_error(
  function() invoke("native_cleanup_unserialize", list(late_bad)),
  "invalid string encoding"
)
trailing <- c(serialized, as.raw(0x00))
expect_native_error(
  function() invoke("native_cleanup_unserialize", list(trailing)),
  "trailing bytes"
)

cat("subset index data-pointer error before output ownership\n")
error_index <- invoke("native_cleanup_error_index")
expect_native_error(
  function() invoke("native_cleanup_subset", list(seed, error_index)),
  "injected index data-pointer error"
)

cat("Rcpp and cpp11 nested fault cleanup\n")
for (framework in c("rcpp", "cpp11")) {
  framework_package <- if (framework == "rcpp") "Rcpp" else "cpp11"
  if (!requireNamespace(framework_package, quietly = TRUE)) {
    stop(
      sprintf(
        "native cleanup nested fixture requires the %s package",
        framework_package
      ),
      call. = FALSE
    )
  }
  include_dir <- system.file("include", package = framework_package)
  stopifnot(nzchar(include_dir))
  framework_dll <- compile_test_dso(
    file.path(
      package_root, "tools", "native-cleanup", paste0(framework, "_cleanup.cpp")
    ),
    sprintf('PKG_CPPFLAGS += -I"%s"', include_dir),
    paste(framework, "nested cleanup fixture")
  )
  run_symbol <- getNativeSymbolInfo(
    paste0("C_", framework, "_native_cleanup_run"),
    PACKAGE = framework_dll[["name"]]
  )
  cleanup_symbol <- getNativeSymbolInfo(
    paste0("C_", framework, "_native_cleanup_count"),
    PACKAGE = framework_dll[["name"]]
  )
  expression_environment <- environment()
  expressions <- list(
    as = quote(invoke("native_cleanup_as", list(c("alpha", "payload", NA_character_)))),
    bulk = quote(invoke("native_cleanup_bulk"))
  )
  expected_messages <- c(
    "native cleanup probe: injected external-pointer error",
    "native cleanup probe: injected finalizer-registration error",
    "native cleanup probe: injected ALTREP-shell error"
  )
  for (operation in names(expressions)) {
    for (stage in seq_along(expected_messages)) {
      invisible(invoke("native_cleanup_set_failure", list(as.integer(stage))))
      cleanup_before <- .Call(cleanup_symbol)
      err <- tryCatch(
        .Call(run_symbol, expressions[[operation]], expression_environment),
        error = identity
      )
      stopifnot(
        inherits(err, "error"),
        identical(conditionMessage(err), expected_messages[[stage]]),
        identical(invoke("native_cleanup_count"), 0L),
        identical(.Call(cleanup_symbol), cleanup_before + 1L)
      )
      cat("nested", framework, operation, "stage", stage, "passed\n")
    }
  }
  cleanup_after <- .Call(cleanup_symbol)
  for (i in seq_len(3L)) gc()
  stopifnot(identical(.Call(cleanup_symbol), cleanup_after))
}

rm(seed, serialized, index, error_index)
gc_until_clear()
cat("native cleanup probe passed\n")
