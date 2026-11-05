# charport 0.2.0 (2026-11-03)

* New `charpack` class: a compact ALTREP character vector whose strings live
  in one R raw vector, for many short vectors. A `Reader` reaches any element
  directly, without allocating. `as_charpack()` converts to one and
  `is_charpack()` tests for one. In C++, `charpack::from_store()` copies a
  finished `charvec::Store` into one, so a producer can build with the
  `charvec` builders and choose the class at the end. Reading an element
  through ordinary R access materializes the whole `charpack`; a `Reader`
  keeps it packed, and so do vector subsets such as `x[i]`, which return a
  new `charpack`.
* `charvec::alloc_na()`, `charvec::get_store()` and `charvec::Writer` fill a
  `charvec` in place after it is created.
* On R >= 4.6, `Reader` now handles base `wrap_string` ALTREP without materialization
* `charport_materialize()` also materializes a `charpack`.
* A filled `charpack` serializes in its compact form, like `charvec`, without
  materializing; on a little-endian host its raw vector is written and read
  back without a copy. Both write little-endian on every host; big-endian
  `charvec` states from earlier versions still load.
* Serialization is now safe across locales. A `charvec` or `charpack` with
  native-encoded strings is saved in packed form only in a UTF-8 locale;
  elsewhere it is saved as an ordinary character vector, so R converts its
  strings on reading, and the source stays packed. Native strings in a packed
  state read outside a UTF-8 locale are marked UTF-8. `charvec` states are now
  version 2; version 1 states from 0.1 still load, with native strings read as
  the current locale's, as before.
* New `charport_native_is_utf8()` (C++: `charport::native_is_utf8()`) reports
  whether R currently reads native strings as UTF-8, following
  `Sys.setlocale()`.
* `Reader` now calls a registered class' `init` first, and otherwise falls
  back to `STRING_PTR_RO`.

# charport 0.1.1 (2026-09-18)

* Fix ODR issue for exported header functions.

# charport 0.1.0 (2026-09-08)

* Initial CRAN release.
