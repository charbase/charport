// charport_charvec_wrap: the one builder step that must run inside charport
// (the ALTREP class object lives here). Consumers construct the store in
// their own compiled code via the header-only cp::charvec::Builder --
// header vintage is guarded by CHARPORT_ABI_VERSION / cp::check_abi -- and
// hand ownership across here. All R packages on a platform share one
// toolchain and heap, so deleting a consumer-new'd store in charport's
// finalizer is sound.

#include "charvec_altrep.h"
#include "charport_registry.h"

extern "C" {

SEXP charport_charvec_wrap(void * store_) {
  if(store_ == nullptr) {
    Rf_error("charport charvec wrap: store is NULL");
  }
  std::unique_ptr<cpi::charvec_data> store(static_cast<cpi::charvec_data *>(store_));

  // The store was built by the consumer's own compiled code (the cp::charvec
  // builders), not parsed from untrusted input, so there is no policy to
  // enforce here: the records are stored as the builder was given them
  // (Unserialize, which DOES read untrusted bytes, validates there). Just take
  // ownership and wrap.
  return charport_sexp_guard("charvec wrap", [&]() -> SEXP {
    return charvec_altrep::Make(store.release(), true);
  });
}

} // extern "C"
