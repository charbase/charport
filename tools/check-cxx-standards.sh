#!/usr/bin/env bash

set -euo pipefail

read -r compiler _ < <(R CMD config CXX)
r_include=$(Rscript -e 'cat(R.home("include"))')

for standard in c++11 c++14 c++17 c++20 c++23; do
  echo "== ${standard}"
  "${compiler}" -std="${standard}" -pedantic-errors -fsyntax-only \
    -Iinst/include -Itests -I"${r_include}" tests/charport_consumer.cpp
done

echo "== c++11 public header without RTTI"
"${compiler}" -std=c++11 -fno-rtti -pedantic-errors -fsyntax-only \
  -Iinst/include -I"${r_include}" tests/charport_header_portability.cpp

rcpp_include=$(Rscript -e 'cat(system.file("include", package = "Rcpp"))')
if [[ -n "${rcpp_include}" ]]; then
  echo "== c++11 Rcpp adapters"
  "${compiler}" -std=c++11 -pedantic-errors -fsyntax-only \
    -Iinst/include -I"${r_include}" -I"${rcpp_include}" \
    tests/rcpp_consumer.cpp
fi

cpp11_include=$(Rscript -e 'cat(system.file("include", package = "cpp11"))')
if [[ -n "${cpp11_include}" ]]; then
  echo "== c++11 cpp11 adapters"
  "${compiler}" -std=c++11 -pedantic-errors -fsyntax-only \
    -Iinst/include -I"${r_include}" -I"${cpp11_include}" \
    tests/cpp11_consumer.cpp
fi

read -r c_compiler _ < <(R CMD config CC)
echo "== c99"
"${c_compiler}" -x c -std=c99 -fsyntax-only \
  -Iinst/include -I"${r_include}" tests/charport_c_header.c
