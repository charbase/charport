#' Construct a charvec
#'
#' Builds a `charvec` -- charport's reference ALTREP character vector class --
#' from the given values. A `charvec` is an ordinary character vector to R
#' code (`typeof(x)` is `"character"`); its strings live as UTF-8/ASCII byte
#' views in stable native memory blocks and are only converted to R's interned
#' `CHARSXP` strings when something forces materialization.
#'
#' Element encodings are normalized at construction: ASCII and UTF-8 bytes are
#' stored as-is, latin1 and native-encoded strings are translated to UTF-8,
#' and `bytes`-encoded strings are stored verbatim. `NA_character_` is
#' preserved.
#'
#' @param ... values to combine, as in [c()]; non-character values are coerced
#'   with [as.character()].
#' @return A `charvec` (an ALTREP character vector).
#' @examples
#' x <- charvec("hello", "world", NA)
#' is_charvec(x)
#' x[1]
#' @export
charvec <- function(...) {
  as_charvec(c(character(0L), ...))
}

#' Convert to a charvec
#'
#' Converts a character vector (or anything [as.character()] accepts) to a
#' `charvec`. If `x` is already a `charvec` it is returned unchanged. Names
#' are preserved; other attributes are dropped.
#'
#' @param x object to convert.
#' @return A `charvec` (an ALTREP character vector).
#' @examples
#' as_charvec(letters)
#' @export
as_charvec <- function(x) {
  if (is_charvec(x)) {
    return(x)
  }
  if (!is.character(x)) {
    x <- as.character(x)
  }
  ret <- .Call(C_charvec_from_character, x)
  nm <- names(x)
  if (!is.null(nm)) {
    names(ret) <- nm
  }
  ret
}

#' Test for a charvec
#'
#' @param x object to test.
#' @return `TRUE` if `x` is a `charvec` ALTREP vector, `FALSE` otherwise.
#'   Note a materialized `charvec` is still a `charvec`; serialization of a
#'   materialized `charvec` round-trips to a plain character vector.
#' @examples
#' is_charvec(charvec("a"))
#' is_charvec(letters)
#' @export
is_charvec <- function(x) {
  .Call(C_is_charvec, x)
}

#' Force materialization of a character vector
#'
#' Forces a `charvec` to materialize its R-level strings (`CHARSXP`s), caching
#' them on the object; the native store is released. Ordinary character
#' vectors are returned unchanged. This is a diagnostic/escape hatch: code
#' that needs guaranteed-plain string storage (for example, before handing a
#' vector to C code that bypasses ALTREP accessors) can call it explicitly.
#'
#' @param x a character vector (plain or `charvec`).
#' @return `x`, invisibly, after forcing materialization.
#' @examples
#' x <- charvec("a", "b")
#' charport_materialize(x)
#' @export
charport_materialize <- function(x) {
  invisible(.Call(C_charport_materialize, x))
}
