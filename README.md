charport
================

[![R-CMD-check](https://github.com/traversc/charport/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/traversc/charport/actions)

ALTREP lets an R package store character vectors outside R’s usual
per-element `CHARSXP` representation: in files, in lazily decoded data,
or in compact byte storage. This optimization is normally local to the
package that defines the ALTREP class. Code that does not recognize the
class accesses the vector through standard R interfaces, which may
materialize ordinary `CHARSXP`s and remove the storage advantage. For
example, `arrow` can defer string materialization while reading a
parquet file, only for a downstream operation such as a `dplyr` join to
incur the cost. Benchmarks in
[stringfish](https://github.com/traversc/stringfish) report 10-40x
speedups over base R for string operations that avoid materialization.

`charport` provides a shared read interface across package boundaries.
Packages keep their own ALTREP string backends and register them at load
time. Consumers resolve plain, registered, and unregistered character
vectors into the same reader interface. Registered backends can expose
their existing storage without materializing `CHARSXP`s.

`charport` is the infrastructure half of a two-package project supported
by a grant from the [R
Consortium](https://r-consortium.org/all-projects/call-for-proposals.html)
Infrastructure Steering Committee; the companion package is an
ALTREP-aware fork of stringi.

`charport` provides:

1.  **`charvec`** - the reference ALTREP string class, backed by stable
    memory slices (strview-shaped records + bump-allocated slice
    blocks), with cheap mutation, threshold compaction, and a sharded
    parallel-constructible builder.
2.  **Runtime registration interface** - a C API a backend package calls
    at load time (via `R_GetCCallable`) to register its own ALTREP
    string class: a per-vector state lookup plus a per-element accessor.
3.  **Common read path** - `charport_resolve()` turns any character
    vector into a `charport_reader`: one resolve per vector, then a
    uniform `get(state, i)` per element.

The ABI in the public header (the `R_GetCCallable` symbol set and the
`charport_strview`/`charport_reader` layouts) is **not frozen** until
the first CRAN release.

## The numbers

The benchmark tests the cost of one function-pointer indirection per
element and measures a registered backend that requires no per-element R
API calls. `make bench` hashes every byte of 1.1 million strings (94 MB,
taken from the enwik8 Wikipedia corpus split into lines) with FNV-1a.

<img src="vignettes/bench.png" title="charport benchmark" width="700" />

The first two bars compare a conventional `STRING_ELT` loop with the
same loop through `cp::Reader`. In this run, the additional indirection
had no measurable cost on plain vectors, and reading the registered
`charvec` backend was slightly faster. Readers are plain structs that
workers can copy, so the same loop can also run in parallel.
Construction through the builder, which never creates a `CHARSXP`, was
about 15x faster than the `Rf_mkCharLenCE` + `SET_STRING_ELT` baseline
on this corpus and 40x faster with four shards.

One measurement note: the construction baseline is benchmarked cold.
`Rf_mkCharLenCE` interns every string in R’s global string cache, and an
already-interned string costs a hash lookup instead of an allocation, so
a corpus that already exists as `CHARSXP`s (say, via `readLines`) would
hand the baseline cache hits and flatter it considerably - and a `gc()`
does not undo that, because the cache keeps its grown table afterwards.
The bench therefore ingests the corpus directly from the file in C (no
`CHARSXP` is ever created) and runs every baseline repetition in its own
fresh R session. (`charvec` does not use the string cache, so cache
state does not affect the builder timings.) All paths are verified to
produce identical results, and `inst/extra/benchmark.R` reproduces the
plot on any machine.

## R surface

The R-level API is small and mainly diagnostic:

``` r
library(charport)
x <- charvec("hello", "world", NA)
is_charvec(x)
```

    ## [1] TRUE

``` r
as_charvec(letters[1:3])
```

    ## [1] "a" "b" "c"

``` r
charport_materialize(x)
charport_backends()
```

    ## $n
    ## [1] 1
    ## 
    ## $reentrant
    ## [1] TRUE

``` r
charport_backend_of(charvec("a"))
```

    ## [1] "charport::charvec"

`charvec` objects are ordinary character vectors to R code (`typeof` is
`"character"`); strings are kept as UTF-8/ASCII byte views in native
memory and only materialized to R `CHARSXP`s when something asks for
them.

Everything else happens in compiled code, through the single public
header `charport.hpp`, which is C++11-compatible and does not require
consumers to declare a `CXX_STD`. A consumer package adds
`LinkingTo: charport` and `Imports: charport` to its DESCRIPTION,
includes the header, and calls `cp::check_abi()` once at load time (from
`.onLoad`) so a binary compiled against older charport headers fails
with a clean “please reinstall” message instead of misbehaving.

## Reading any character vector

`cp::Reader` is the consumer-side wrapper around `charport_resolve()`.
It works uniformly on every character vector: a registered ALTREP
backend is served zero-copy from its own storage; everything else (plain
vectors, unregistered or already-materialized ALTREP) is served by a
built-in accessor over the vector’s `CHARSXP`s, materializing once up
front if needed - the same cost consumers pay today.

``` cpp
#include "charport.hpp"

extern "C" SEXP C_total_bytes(SEXP x) {
  cp::Reader r(x);            // one resolve; x must be a character vector
  double total = 0;
  for(cp::StrView v : r) {    // v.ptr / v.len / v.enc; v.is_na()
    if(!v.is_na()) total += v.len;
  }
  return Rf_ScalarReal(total);
}
```

`cp::StrView` (= `charport_strview`) is
`{const char * ptr; uint32_t len; charport_enc enc}`. `ptr == NULL`
means `NA`. Registered backends only emit ASCII/UTF-8/byte views; the
fallback path over plain vectors can additionally surface
`CE_NATIVE`/`CE_LATIN1`, exactly as `Rf_getCharCE()` would.

**A reader is a borrow, in Rust terms.** Its views point into memory
owned by the vector’s backend; nothing is copied. Between resolving the
reader and the last use of any view, keep `x` protected and do not touch
it through the R API at all - no writes, no `DATAPTR()` or
`STRING_PTR_RO()`, and no element access either: nothing in the ALTREP
contract stops a class from materializing eagerly on `STRING_ELT()`, and
mutation and materialization can both move or free the storage the views
point into. While the borrow lasts, the reader is the only safe way to
look at `x`. When R-level access is needed, finish with the reader (and
every copy of it) first, then resolve a fresh one afterwards. A reader
has no close operation. The borrow ends after the reader and all derived
views are no longer used; C++ does not enforce this lifetime.

For threaded reads, check `r.reentrant()` (true for registered reentrant
backends and for the fallback path): the underlying `charport_reader` is
an SEXP-free plain struct, so hand each worker a copy of `r.raw()` and
adopt it with `cp::Reader(raw)` - workers never touch the R API. Resolve
on the main thread; the borrow holds for as long as any copy is in use.

## Building a charvec

`cp::charvec::Builder` constructs a `charvec` without ever creating
`CHARSXP`s. Serially:

``` cpp
extern "C" SEXP C_repeat_word(SEXP word, SEXP n_) {
  cp::Reader in(word);
  cp::StrView w = in[0];
  const R_xlen_t n = Rf_asInteger(n_);

  cp::charvec::Builder b(n);     // all elements start NA
  for(R_xlen_t i = 0; i < n; ++i) {
    b.set(i, w);                 // copies the bytes into the store
  }
  return b.finish();             // wraps the store in a charvec ALTREP
}
```

For parallel construction, request one `BuilderShard` per thread on the
main thread, give each worker a disjoint index range, and `finish()`
after the join - the merge moves slice blocks, it does not copy payload:

``` cpp
cp::charvec::Builder b(n);
std::vector<cp::charvec::BuilderShard> shards;
for(int t = 0; t < n_threads; ++t) shards.push_back(b.shard());

// on worker t, for i in its own range only:
//   shards[t].set(i, ptr, len, charport_enc::CE_UTF8);
// set() touches no R API; errors are thrown as std::runtime_error,
// so catch in the worker (or let RcppParallel propagate to the join)

SEXP out = b.finish();  // main thread
```

`set()` enforces the emission policy at the write: encodings must be
`CE_ASCII`, `CE_UTF8`, `CE_ASCII_OR_UTF8`, or `CE_BYTES` (translate
latin1/native text to UTF-8 first), and `NA` is exactly
`{NULL, 0, CE_NA}` (or `set_na()`).

## Registering a backend

A package that defines its own ALTREP string class can register it so
every charport consumer reads it zero-copy. Two functions, registered
once at load time:

``` cpp
// called once per vector by charport_resolve, on the main R thread;
// return the per-vector state, or NULL for "cannot serve this vector"
static void * my_init(SEXP x) {
  return R_ExternalPtrAddr(R_altrep_data1(x));
}

// called per element, with the state my_init returned; no R API here
static charport_strview my_get(void * state, R_xlen_t i) {
  const my_store * s = static_cast<const my_store *>(state);
  return { s->ptr(i), s->len(i), charport_enc::CE_UTF8 };
}

// in R_init_mypkg / .onLoad, after creating the ALTREP class:
cp::register_backend(my_class, my_init, my_get, /* reentrant = */ true);
// and in .onUnload: cp::unregister_backend(my_class)
```

`reentrant = true` is a promise about `my_get`: no R allocation, no GC
trigger, no mutation of backend state, callable concurrently from any
thread, and the returned pointer stays valid while the vector is alive,
unwritten, and unmaterialized. If your accessor can’t promise that,
register with `false`: consumers then call it serially on the main R
thread and copy each view before the next call.

`charvec` itself registers through this exact interface, so the
third-party path is exercised by charport’s own test suite.

## Status

Implemented: the `charvec` class and its store; the backend registry,
`charport_resolve`, and both fallback paths; the public `charport.hpp`
consumer header (`cp::Reader`, `cp::charvec::Builder` /
`cp::charvec::BuilderShard`); and real-thread test coverage via a mini
consumer package compiled at test time, which doubles as a true
external-DSO consumer of the installed headers. Still to come: example
backend/consumer packages on GitHub, load-order tests, and integration
with the companion stringi fork.

Writable interop (consumers mutating another package’s backend through a
generic interface) is explicitly out of scope.

## Lineage

`charvec` reuses ideas from the `slice_store` design in
[stringfish](https://github.com/traversc/stringfish). It does not
promise value, layout, or serialization compatibility with stringfish.

## Acknowledgments

Development of `charport` is funded by the R Consortium through an
Infrastructure Steering Committee grant.
