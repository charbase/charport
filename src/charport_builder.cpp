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

  // the trust boundary into R: a charvec must never hold records the ABI
  // forbids (same policy unserialize enforces), wherever the store was built
  const char * violation = nullptr;
  for(const charport_strview & rec : store->records) {
    if(rec.is_na()) {
      if(rec.len != 0) {
        violation = "NA record with non-zero length";
        break;
      }
      continue;
    }
    switch(rec.enc) {
    case charport_enc::CE_ASCII:
    case charport_enc::CE_UTF8:
    case charport_enc::CE_ASCII_OR_UTF8:
    case charport_enc::CE_BYTES:
      break;
    default:
      violation = "record encoding violates the emission policy "
                  "(CE_ASCII / CE_UTF8 / CE_ASCII_OR_UTF8 / CE_BYTES)";
      break;
    }
    if(violation != nullptr) {
      break;
    }
  }
  if(violation != nullptr) {
    store.reset();  // free before the longjmp
    Rf_error("charport charvec wrap: %s", violation);
  }

  return charport_sexp_guard("charvec wrap", [&]() -> SEXP {
    return charvec_altrep::Make(store.release(), true);
  });
}

} // extern "C"
