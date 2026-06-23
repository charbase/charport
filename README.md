charport
================

<img src="man/figures/logo.svg" align="right" width="160" alt="charport logo" style="border: 0; padding: 0; background: transparent;" />

[![R-CMD-check](https://github.com/charport/charport/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/charport/charport/actions)

## ALTREP string interoperability

`charport` provides a shared read interface for ALTREP character vectors.
Backend packages register their ALTREP classes, and compiled consumers
read string bytes through `charport::Reader` without depending on the
backend's storage layout. Plain and unregistered vectors fall back to R's
ordinary string representation.

*The work in this package is funded by the R Consortium Infrastructure
Steering Committee.*

## Why use a shared reader?

Compiled packages normally access character vectors through R's string
API. An unmaterialized ALTREP class may need to allocate ordinary
`CHARSXP` storage to support that access. Class-specific integration can
avoid materialization, but couples the consumer to one backend.
`charport` keeps that integration at one boundary: a backend registers
once, and consumers use the same reader for every supported class.

The benchmark below uses the enwik8 corpus split into 1.13 million
strings. The conventional read baseline is `STRING_ELT` materialization
over a `charvec` ALTREP class. The comparison is `charport::Reader` over
the same data, avoiding materialization. On the write path, writing
standard R strings via `SET_STRING_ELT` is the baseline, compared to
writing the same data to `charvec`.

<img src="man/figures/bench.png" title="charport benchmark"
style="width:100.0%" />

The results show the materialization and construction costs for this
corpus. They are not a general performance guarantee.

## Interoperability

`charport` acts as a small broker for ALTREP strings:

1.  A package registers the ALTREP string class it owns via
    `register_altrep`.
2.  A consumer package reads string data through `charport::Reader`,
    without knowing that class’s storage layout.
3.  Registered ALTREP classes can be read directly; ordinary vectors and
    unregistered ALTREP classes fall back to standard R behavior.

The contract covers bytes, encoding marks, pointer lifetime, and access
capabilities. `charport` does not define string semantics, locale policy,
normalization, or a new user-facing string API.

## `charvec`: a reference ALTREP character vector

`charport` also includes `charvec`, which stores string data in native
blocks alongside a vector of metadata. To R, a `charvec` behaves like an
ordinary character vector. Compiled code can construct one serially or
across multiple worker threads, then read it through `charport::Reader`
without materialization. It serves as both a reference implementation
and an output type for packages that produce string data.

## For package developers

Package authors can use `charport` from either side of the interface.

ALTREP string classes register through `register_altrep`. Consumer
packages read through `charport::Reader`, and packages that construct
string output can use `charvec` directly.

The package developer guide covers registration, reading, fallback
behavior, pointer lifetime, thread safety, and the `charvec` builder.

- [Package developer guide](vignettes/developer-guide.Rmd), also
  available from R with
  `vignette("developer-guide", package = "charport")`.
