#!/usr/bin/env bash
# Linux/GCC: [minimal|package] [reproduce|verify] [source-directory]
# reproduce expects an LTO-only ODR failure; verify requires both builds to pass.
set -euo pipefail
mode=${1:-minimal}
case "$mode" in minimal|package) ;; *) echo 'Expected minimal or package' >&2; exit 2;; esac
expectation=${2:-reproduce}
case "$expectation" in reproduce|verify) ;; *) echo 'Expected reproduce or verify' >&2; exit 2;; esac
repo_dir=$(cd "${3:-$(dirname "${BASH_SOURCE[0]}")/..}" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/charport-asan-odr.XXXXXX")
if [[ -n ${GITHUB_OUTPUT:-} ]]; then echo "results=$out" >> "$GITHUB_OUTPUT"; fi
cxx=${CXX:-g++}
cc=${CC:-gcc}
asan=$("$cxx" -print-file-name=libasan.so)
[[ -f "$asan" ]] || { echo "Cannot locate libasan: $asan" >&2; exit 1; }
echo "Results: $out"
"$cxx" --version > "$out/compiler.txt"
printf 'mode=%s\nexpectation=%s\nsource=%s\n' "$mode" "$expectation" "$repo_dir" > "$out/run.txt"
git -C "$repo_dir" rev-parse HEAD >> "$out/run.txt" 2>/dev/null || true

if [[ $mode == minimal ]]; then
  cat > "$out/cache.h" <<'EOF'
using function_pointer = void (*)();
inline function_pointer cached() {
  static function_pointer fn = nullptr;
  return fn;
}
EOF
  cat > "$out/a.cpp" <<'EOF'
#include "cache.h"
extern "C" function_pointer from_a() { return cached(); }
EOF
  cat > "$out/b.cpp" <<'EOF'
#include "cache.h"
extern "C" function_pointer from_b() { return cached(); }
EOF
  cat > "$out/load.cpp" <<'EOF'
#include <dlfcn.h>
#include <cstdio>
int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (!dlopen(argv[i], RTLD_NOW | RTLD_GLOBAL)) {
      std::fprintf(stderr, "%s\n", dlerror());
      return 2;
    }
  }
}
EOF
  "$cxx" -g -fsanitize=address -no-pie "$out/load.cpp" -ldl -o "$out/load"
else
  Rscript -e 'stopifnot(requireNamespace("Rcpp", quietly=TRUE), requireNamespace("cpp11", quietly=TRUE), packageVersion("cpp11") >= "0.5.2"); sessionInfo()' > "$out/session.txt" 2>&1
  mkdir "$out/library"
  # Isolate installation outputs so concurrent runs cannot overwrite each other.
  mkdir "$out/source"
  cp -a "$repo_dir/." "$out/source/"
  if ! R CMD INSTALL --preclean --clean --no-test-load -l "$out/library" "$out/source" > "$out/install.log" 2>&1; then
    cat "$out/install.log"
    exit 1
  fi
  r_home=$(R RHOME)
  cat > "$out/run.R" <<'EOF'
# ASan stays loaded in this R process. Avoid preloading it into build tools.
Sys.unsetenv("LD_PRELOAD")
sessionInfo()
source("tests/test_charport_wrappers.R", echo=FALSE)
EOF
fi

for variant in no-lto lto; do
  flags=(-O2 -g -fsanitize=address -fno-omit-frame-pointer)
  if [[ $variant == lto ]]; then flags+=(-flto); else flags+=(-fno-lto); fi
  if [[ $mode == minimal ]]; then
    for unit in a b; do
      "$cxx" "${flags[@]}" -fPIC -shared "$out/$unit.cpp" -o "$out/$variant-$unit.so"
      nm -DC "$out/$variant-$unit.so" > "$out/$variant-$unit.symbols"
    done
    status=0
    ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=2 \
      "$out/load" "$out/$variant-a.so" "$out/$variant-b.so" > "$out/$variant.log" 2>&1 || status=$?
  else
    {
      echo "CC = $cc ${flags[*]}"
      for name in CXX CXX11 CXX14 CXX17 CXX20 CXX23 CXX26; do
        echo "$name = $cxx ${flags[*]}"
        echo "${name}FLAGS = -O2 -g"
      done
      echo 'CFLAGS = -O2 -g'
    } > "$out/$variant.Makevars"
    status=0
    # exec/R does not put R_HOME/lib on the loader path; the R wrapper does.
    (cd "$repo_dir" && env R_HOME="$r_home" \
      R_LIBS="$out/library${R_LIBS:+:$R_LIBS}" \
      LD_LIBRARY_PATH="$r_home/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      R_MAKEVARS_USER="$out/$variant.Makevars" LD_PRELOAD="$asan" \
      ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=2 \
      "$r_home/bin/exec/R" --vanilla --slave -f "$out/run.R") > "$out/$variant.log" 2>&1 || status=$?
  fi
  echo "$status" > "$out/$variant.status"
  echo "$variant: exit $status"
  if [[ $variant == no-lto || $expectation == verify ]]; then
    [[ $status == 0 ]] || { cat "$out/$variant.log"; exit 1; }
  else
    if [[ $status == 0 ]] || ! grep -q 'ERROR: AddressSanitizer: odr-violation' "$out/$variant.log" ||
       { [[ $mode == package ]] && ! grep -q 'charport/charvec/builder.h:137' "$out/$variant.log"; }; then
      cat "$out/$variant.log"
      echo 'Expected ODR report was not reproduced with this toolchain.' >&2
      exit 1
    fi
    grep -A2 'ERROR: AddressSanitizer: odr-violation' "$out/$variant.log"
  fi
done
if [[ $expectation == reproduce ]]; then
  echo 'Reproduced: ASan passes without LTO and reports an ODR violation with LTO.'
else
  echo 'Verified: ASan passes both without and with LTO.'
fi
