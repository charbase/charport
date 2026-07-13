#ifndef CHARPORT_API_H
#define CHARPORT_API_H

// Common C++ API. charport.h selects the unwind backend.

#ifndef CHARPORT_UNWIND_BACKEND
#error "include charport.h"
#endif

#include "interop/reader.h"
#include "charvec/builder.h"

namespace charport {
namespace detail {

using selected_backend = CHARPORT_UNWIND_BACKEND;

} // namespace detail

using Reader = BasicReader<detail::selected_backend>;

template<typename Fn>
static SEXP unwind_protect(Fn && fn) {
  return detail::selected_backend::call(std::forward<Fn>(fn));
}

namespace charvec {

using Builder = BasicBuilder<detail::selected_backend>;
using DirectBuilder = BasicDirectBuilder<detail::selected_backend>;
using ParallelBuilder = BasicParallelBuilder<detail::selected_backend>;

static inline SEXP build_scalar(const StrView & value) {
  return build_scalar_with<detail::selected_backend>(value);
}

} // namespace charvec
} // namespace charport

#endif
