#' Registered charport ALTREP classes
#'
#' Reports on the broker's ALTREP class registry. Registered classes are
#' ALTREP character vector classes whose authors registered a reader with
#' charport (via the `charport_register_altrep` C entry point, fetched with
#' `R_GetCCallable`). The reference `charvec` class is registered when
#' `charport` loads, so a freshly loaded session normally reports at least
#' that class.
#'
#' ALTREP class names require an instance to query (R's
#' `R_altrep_class_name` takes a vector, not a class descriptor), so this
#' registry view reports the count and capability flags; use
#' [charport_class_of()] on a vector to get its registered class name.
#'
#' @return A list with elements `n` (integer: number of registered
#'   classes), `persistent_views` (logical vector: whether returned byte
#'   views remain valid until the reader borrow ends), `concurrent_access`
#'   (logical vector: whether reader access calls may run concurrently),
#'   and `reentrant` (logical vector: whether both capabilities are true).
#' @examples
#' charport_classes()
#' @export
charport_classes <- function() {
  .Call(C_charport_classes)
}

#' Character vector diagnostics
#'
#' Reports non-forcing diagnostics for a possible character vector. This is a
#' preflight/development helper: it does not call `STRING_ELT()`,
#' `STRING_PTR_RO()`, `DATAPTR()`, or any registered reader callback.
#'
#' `is_materialized` means ordinary string pointer storage is available without
#' forcing (`DATAPTR_OR_NULL(x) != NULL`). For base R deferred strings, some
#' elements may have been cached by `STRING_ELT()` while this still reports
#' `FALSE`; the field is a full-materialization/direct-pointer diagnostic.
#'
#' @param x object to inspect.
#' @return A named list containing `is_strsxp`, `length`, `is_altrep`,
#'   `is_materialized`, `is_registered`, reader capability flags,
#'   `stateful_reader`, `reentrant`, ALTREP class name/package fields, and
#'   `altrep_class` as `"package::class"` when class metadata is available.
#' @examples
#' charport_info(charvec("a"))
#' @export
charport_info <- function(x) {
  .Call(C_charport_info, x)
}

#' Registered ALTREP class serving a character vector
#'
#' Identifies whether a registered ALTREP class claims `x`. This is a
#' class-membership question answered without touching the vector's data:
#' it never materializes `x` and reports a match even when the class reader
#' would decline to serve this particular instance (for example, a
#' materialized `charvec`).
#'
#' @param x a character vector.
#' @return `"package::class"` for a registered class match when class metadata
#'   is available; otherwise `NA_character_`.
#' @examples
#' charport_class_of(charvec("a"))
#' charport_class_of(letters)
#' @export
charport_class_of <- function(x) {
  info <- charport_info(x)
  if (!isTRUE(info$is_strsxp)) {
    stop("charport_class_of: x must be a character vector", call. = FALSE)
  }
  if (isTRUE(info$is_registered)) {
    return(info$altrep_class)
  }
  NA_character_
}
