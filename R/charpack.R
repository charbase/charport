#' Test for a charpack
#'
#' A `charpack` is a compact ALTREP character vector whose strings live in one
#' raw vector. Packages create them through the C API.
#'
#' @param x object to test.
#' @return `TRUE` if `x` is a `charpack` ALTREP vector, `FALSE` otherwise. A
#'   materialized `charpack` is still a `charpack`.
#' @examples
#' is_charpack(letters)
#' @export
is_charpack <- function(x) {
  .Call(C_is_charpack, x)
}

#' Convert to a charpack
#'
#' Converts a character vector (or anything [as.character()] accepts) to a
#' `charpack`, copying its strings. If `x` is already a `charpack` it is
#' returned unchanged. Names are preserved; other attributes are dropped.
#'
#' A `charpack` stays compact only while it is read through charport. Any
#' element access from R, such as `x[1]` or `print(x)`, materializes the whole
#' vector. This makes it mainly useful for testing code that reads strings
#' through charport.
#'
#' @param x object to convert.
#' @return A `charpack` (an ALTREP character vector).
#' @examples
#' x <- as_charpack(c("a", NA, "b"))
#' is_charpack(x)
#' @export
as_charpack <- function(x) {
  if (is_charpack(x)) {
    return(x)
  }
  nm <- names(x)
  if (!is.character(x)) {
    x <- as.character(x)
  }
  ret <- .Call(C_charpack_fill_from, .Call(C_charpack_placeholder), x, 0L, FALSE)
  if (!is.null(nm) && length(nm) == length(ret)) {
    names(ret) <- nm
  }
  ret
}
