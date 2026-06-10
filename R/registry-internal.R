# Internal test hooks for the broker (registry + charport_resolve). Not
# exported, not API; signatures may change at any time.

# Resolve x and rebuild a plain character vector by looping the reader --
# the equivalence-test seam: must be value- and mark-identical to
# as.character(x).
charport_read_all <- function(x) {
  .Call(C_charport_reader_read_all, x)
}

# Resolve x and report the reader: n, reentrant, and which path serves it
# ("backend" or "direct").
charport_reader_info <- function(x) {
  .Call(C_charport_reader_info, x)
}

# Remove / restore charvec's own registry entry, to exercise the
# unregistered-fallback and re-registration paths.
charport_test_unregister_charvec <- function() {
  invisible(.Call(C_charport_test_unregister_charvec))
}

charport_test_register_charvec <- function() {
  invisible(.Call(C_charport_test_register_charvec))
}

# TRUE if R_GetCCallable hands back the broker's own entry points and the
# ABI version matches.
charport_ccallable_check <- function() {
  .Call(C_charport_ccallable_check)
}

# cp:: wrapper exercises (charport consuming its own public header through
# R_GetCCallable, the way an external consumer would).

# Loop a cp::Reader and rebuild a plain character vector.
cp_reader_roundtrip <- function(x) {
  .Call(C_cp_reader_roundtrip, x)
}

# Reader -> Builder -> charvec, no CHARSXPs in between. n_shards = 0 drives
# the serial Builder::set path; >= 1 partitions into that many BuilderShards
# over contiguous disjoint ranges (driven serially).
cp_builder_from_reader <- function(x, n_shards = 1L) {
  .Call(C_cp_builder_from_reader, x, as.integer(n_shards))
}

# TRUE if the builder error contract holds (set throws on policy/bounds/NA
# violations, finish is single-shot, abandonment is safe).
cp_builder_errors <- function() {
  .Call(C_cp_builder_errors)
}
