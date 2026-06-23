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
#'   classes), `view_persistence` (logical vector: whether returned byte
#'   views remain valid until the reader borrow ends), `thread_safe_access`
#'   (logical vector: whether the reader accessor may be called concurrently),
#'   and `reentrant` (logical vector: whether both capabilities are true).
#' @examples
#' charport_classes()
#' @export
charport_classes <- function() {
  .Call(C_charport_classes)
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
#' @return `"package::class"` for a registered class match (on R < 4.6.0,
#'   where class names cannot be queried, a placeholder string); otherwise
#'   `NA_character_` (plain vectors and unregistered ALTREP classes).
#' @examples
#' charport_class_of(charvec("a"))
#' charport_class_of(letters)
#' @export
charport_class_of <- function(x) {
  .Call(C_charport_class_of, x)
}
