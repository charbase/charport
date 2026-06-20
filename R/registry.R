#' Registered charport backends
#'
#' Reports on the broker's backend registry. Backends are ALTREP character
#' vector classes whose authors registered a reader with charport (via the
#' `charport_register_backend` C entry point, fetched with
#' `R_GetCCallable`). The reference `charvec` class is not registered by
#' default, so a freshly loaded session may report zero backends.
#'
#' Backend *names* require an instance to query (R's
#' `R_altrep_class_name` takes a vector, not a class descriptor), so this
#' registry view reports the count and capability flags; use
#' [charport_backend_of()] on a vector to get its backend's name.
#'
#' @return A list with elements `n` (integer: number of registered
#'   backends) and `reentrant` (logical vector: each backend's registered
#'   reentrancy capability).
#' @examples
#' charport_backends()
#' @export
charport_backends <- function() {
  .Call(C_charport_backends)
}

#' Backend serving a character vector
#'
#' Identifies which registered backend claims `x`'s ALTREP class. This is a
#' class-membership question answered without touching the vector's data:
#' it never materializes `x` and reports a match even when the backend
#' would decline to serve this particular instance (for example, a
#' materialized `charvec`).
#'
#' @param x a character vector.
#' @return `"package::class"` for a registered backend match (on R < 4.6.0,
#'   where class names cannot be queried, a placeholder string); otherwise
#'   `NA_character_` (plain vectors and unregistered ALTREP classes).
#' @examples
#' is.na(charport_backend_of(charvec("a")))
#' charport_backend_of(letters)
#' @export
charport_backend_of <- function(x) {
  .Call(C_charport_backend_of, x)
}
