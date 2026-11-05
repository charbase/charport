charport_native_symbol <- function(name) {
  get(name, envir = asNamespace("charport"), inherits = FALSE)
}

charvec_alloc <- function(n) {
  .Call(charport_native_symbol("C_charvec_alloc"), n)
}

charvec_assign <- function(x, i, value) {
  invisible(.Call(charport_native_symbol("C_charvec_assign"), x, i, as.character(value)))
}

charvec_stats <- function(x) {
  .Call(charport_native_symbol("C_charvec_stats"), x)
}

charvec_compact <- function(x) {
  invisible(.Call(charport_native_symbol("C_charvec_compact"), x))
}

register_charvec <- function() {
  invisible(.Call(charport_native_symbol("C_register_charvec")))
}

unregister_charvec <- function() {
  invisible(.Call(charport_native_symbol("C_unregister_charvec")))
}

charpack_placeholder <- function() {
  .Call(charport_native_symbol("C_charpack_placeholder"))
}

# Fills placeholder x from strings: fill_views when n_slices <= 0, otherwise
# fill_slices with the payload cut into n_slices pieces.
charpack_fill_from <- function(x, strings, n_slices = 0L, wide = FALSE) {
  .Call(charport_native_symbol("C_charpack_fill_from"), x, strings, as.integer(n_slices), wide)
}

charpack_from_slices <- function(strings, n_slices = 0L, wide = FALSE) {
  charpack_fill_from(charpack_placeholder(), strings, n_slices, wide)
}

charpack_fill_materialized_na <- function(x, n) {
  .Call(charport_native_symbol("C_charpack_fill_materialized_na"), x, n)
}

charpack_state <- function(x) {
  .Call(charport_native_symbol("C_charpack_state"), x)
}

charpack_data1 <- function(x) {
  .Call(charport_native_symbol("C_charpack_data1"), x)
}

charpack_packed_state <- function(x) {
  .Call(charport_native_symbol("C_charpack_packed_state"), x)
}

charpack_from_state <- function(state, wide = FALSE) {
  .Call(charport_native_symbol("C_charpack_from_state"), state, wide)
}

register_charpack <- function() {
  invisible(.Call(charport_native_symbol("C_register_charpack")))
}

unregister_charpack <- function() {
  invisible(.Call(charport_native_symbol("C_unregister_charpack")))
}
