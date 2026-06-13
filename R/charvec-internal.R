# Internal test/diagnostic hooks for the charvec store. Not exported, not
# API: these reach through the ALTREP abstraction to observe store internals
# (allocation accounting, record addresses) so the test suite can pin down
# slice-store semantics. Signatures may change at any time.

# Allocate a charvec of length n with every element NA.
charvec_alloc <- function(n) {
  .Call(C_charvec_alloc, n)
}

# Mutating single-element assignment (1-based i), bypassing R's
# copy-on-write. value is normalized exactly like construction input.
charvec_assign <- function(x, i, value) {
  invisible(.Call(C_charvec_assign, x, i, as.character(value)))
}

# Store snapshot: length, n_slices (payload-block count; NA once materialized),
# materialized.
charvec_stats <- function(x) {
  .Call(C_charvec_stats, x)
}

# Address of element i's payload bytes as a hex string (NA for NA records).
# Used to assert in-place vs relocating assignment and compaction moves.
charvec_element_addr <- function(x, i) {
  .Call(C_charvec_element_addr, x, i)
}

# Rewrite live payload into fresh exact-fit blocks, reclaiming bytes left
# unreferenced by grows/overwrites (and moving record pointers).
charvec_compact <- function(x) {
  invisible(.Call(C_charvec_compact, x))
}

# Per-record storage encodings ("ascii", "UTF-8", "bytes", ...; NA for NA
# records), straight from the store.
charvec_encodings <- function(x) {
  .Call(C_charvec_encodings, x)
}

# Build a charvec through the sharded construction path: one shard per chunk
# (driven serially; the shard/merge machinery is what's under test).
charvec_build_sharded <- function(chunks) {
  .Call(C_charvec_build_sharded, chunks)
}
