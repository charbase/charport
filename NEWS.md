# charport 0.3.0 (2026-11-26)

* A `Reader`'s views of a `charvec` or `charpack` stay valid when the vector
  is materialized while the `Reader` is open, for example through another
  reference to it or a wrapper around it. Both classes count open Readers and
  keep their packed data while any is open; it is then freed with the vector.
  The `charpack` serialization format is unchanged.
* Readers are acquired and released on the main thread, and worker threads
  only call access methods. Keep the vector protected until its `Reader` is
  released. `charport_info()` now reports `stateful_reader` for `charvec` and
  `charpack`.
* A failed `resize()` or copy assignment of `charport::ByteViews` or
  `charport::StrViews` leaves the buffer as it was. An allocation failure
  could previously leave `size()` larger than one of its arrays. These are
  inline header classes, so a consumer package picks up the fix on rebuild.
* A moved-from `charport::Reader` is empty, with size 0. It previously kept
  its length and access callbacks without its state. Also a header fix.
* `charport_materialize()` materializes any character vector through R's
  accessor, including base R deferred strings.
* `Store(n, initial_slice_bytes)` and `RecordTable(n)` initialize records to NA.
  Pass `charvec::uninitialized` to skip the fill when the caller writes every
  record, including missing values. Fixed-size builders use that path.
* `charvec::placeholder()` creates a charvec that receives a completed Store
  through `Builder::fill_placeholder()` or `charvec::fill_store()`. To create an
  NA-filled charvec, wrap a two-argument Store. The `alloc_na()` entry point has
  been removed.
* `charvec::Writer` has been removed. Use `charvec::Builder` to fill the records
  and call `release_store()` when you need the finished Store.
* New `charport::is_ascii(ptr, len)` in `charport/ascii.h`: an R-free ASCII
  scan that is fast on ASCII text and stops early on non-ASCII text.
* Test-only native entry points and forced-wide charpack controls now live in
  a test-built library. `as_charpack()` has a dedicated production converter.
* `as_charvec()` and `as_charpack()` read registered ALTREP inputs through the
  broker, so conversion between charvec and charpack keeps the source packed.
  Unregistered ALTREP inputs retain the ordinary R fallback.
* Document that non-empty records in a finished `charvec::Store` must have
  disjoint byte ranges, because element assignment can reuse those bytes.

# charport 0.2.0 (2026-11-06)

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
* On R >= 4.6, `Reader` now handles base `wrap_string` ALTREP without
  materialization.
* `charport_materialize()` also materializes a `charpack`.
* Fix `x[i]` on a `charvec` returning `NA` for a real index between
  `length(x)` and `length(x) + 1`, instead of the last element.
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
* New C function `charport_native_is_utf8()` (C++:
  `charport::native_is_utf8()`) reports whether R currently reads native
  strings as UTF-8, following `Sys.setlocale()`.
* `Reader` now calls a registered class' `init` first, and otherwise falls
  back to `STRING_PTR_RO`.

# charport 0.1.1 (2026-09-18)

* Fix ODR issue for exported header functions.

# charport 0.1.0 (2026-09-08)

* Initial CRAN release.
