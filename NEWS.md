# charport (development version)

* New `charpack` class: a compact ALTREP character vector whose strings live
  in one R raw vector, for many short vectors.
* `charvec::alloc_na()`, `charvec::get_store()` and `charvec::Writer` fill a
  `charvec` in place after it is created.
* On R >= 4.6, `Reader` now handles base `wrap_string` ALTREP without materialization
* `charport_materialize()` also materializes a `charpack`.
* A filled `charpack` serializes in its compact form, like `charvec`, without
  materializing. Both write little-endian on every host; big-endian `charvec`
  states from earlier versions still load.
* `Reader` now calls a registered class' `init` first, and otherwise falls
  back to `STRING_PTR_RO`.

# charport 0.1.1 (2026-09-18)

* Fix ODR issue for exported header functions.

# charport 0.1.0 (2026-09-08)

* Initial CRAN release.
