#!/usr/bin/env bash

set -euo pipefail

read -r compiler _ < <(R CMD config CXX)
r_include=$(Rscript -e 'cat(R.home("include"))')

for standard in c++11 c++14 c++17 c++20 c++23; do
  echo "== ${standard}"
  "${compiler}" -std="${standard}" -pedantic-errors -fsyntax-only \
    -Iinst/include -I"${r_include}" tests/charport_consumer.cpp
done
