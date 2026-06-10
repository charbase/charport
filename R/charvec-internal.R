# Internal test/diagnostic hooks for the charvec store. Not exported, not
# API: these reach through the ALTREP abstraction to observe store internals
# (allocation accounting, record addresses) so the test suite can pin down
# slice-store semantics. Signatures may change at any time.

# Allocate a charvec of length n with every element NA; initial_slice_size
# preallocates the first slice (bytes, rounded up to 64; NULL or 0 = no
# preallocation, first write uses the growth heuristic).
charvec_alloc <- function(n, initial_slice_size = NULL) {
  .Call(C_charvec_alloc, n, initial_slice_size)
}

# Mutating single-element assignment (1-based i), bypassing R's
# copy-on-write. value is normalized exactly like construction input.
charvec_assign <- function(x, i, value) {
  invisible(.Call(C_charvec_assign, x, i, as.character(value)))
}

# Store accounting snapshot: length, allocated_bytes, dead_bytes, n_slices,
# tail_used, tail_capacity, materialized.
charvec_stats <- function(x) {
  .Call(C_charvec_stats, x)
}

# Address of element i's payload bytes as a hex string (NA for NA records).
# Used to assert in-place vs relocating assignment and compaction moves.
charvec_element_addr <- function(x, i) {
  .Call(C_charvec_element_addr, x, i)
}

# Force compaction regardless of thresholds.
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
