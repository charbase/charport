charport
================

<img src="man/figures/logo.svg" align="right" width="160" alt="charport logo" />

[![R-CMD-check](https://github.com/charport/charport/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/charport/charport/actions)

## ALTREP string interoperability

`charport` provides a shared read interface for ALTREP character vectors.
Backend packages register their ALTREP classes, and compiled consumers
read string bytes through `charport::Reader` without depending on the
backend's storage layout. Plain and unregistered vectors fall back to R's
ordinary string representation.

*The work in this package is funded by the R Consortium Infrastructure
Steering Committee.*

## ALTREP string access

The diagram compares ordinary R access, which may materialize an ALTREP
vector, with access through a registered `charport::Reader` backend.

<img src="man/figures/altrep-string-access.svg" alt="ALTREP string access diagram" />

The benchmark uses the `enwik8` dataset, the first 100 million bytes of
Wikipedia split by line. The read baseline is `STRING_ELT` over an
unmaterialized `charvec` (a built-in ALTREP class). The
`charport::Reader` paths read the same data without materialization.

On the write path, writing standard R strings via `SET_STRING_ELT` is
the baseline, compared to writing the same data to `charvec`.

<img src="man/figures/bench.png" alt="charport benchmark" />

The results measure materialization and construction costs for this corpus and
are not a general performance guarantee.

## A broker for ALTREP strings

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
blocks alongside a vector of metadata.

To R, a `charvec` behaves like an ordinary character vector. In compiled
code, a `charvec` can be constructed serially or across multiple worker
threads, then read through `charport::Reader` without materialization. It
serves as both a reference implementation and an output type for packages
that produce string data.

## For package developers

Package authors can use `charport` from either side of the interface.

ALTREP string classes register through `register_altrep`. Consumer
packages read through `charport::Reader`, and packages that construct
string output can use `charvec` directly.

The package developer guide covers registration, reading, fallback
behavior, pointer lifetime, thread safety, and the `charvec` builder.

- [Package developer
  guide](https://charport.github.io/charport/articles/developer-guide.html),
  also listed by `utils::vignette(package = "charport")`.
