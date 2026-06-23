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
