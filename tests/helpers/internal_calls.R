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

register_charvec_backend <- function() {
  invisible(.Call(charport_native_symbol("C_register_charvec_backend")))
}

unregister_charvec_backend <- function() {
  invisible(.Call(charport_native_symbol("C_unregister_charvec_backend")))
}
