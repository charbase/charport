SHELL   := /bin/bash
PACKAGE := $(shell perl -aF: -ne 'print, exit if s/^Package:\s+//' DESCRIPTION)
VERSION := $(shell perl -aF: -ne 'print, exit if s/^Version:\s+//' DESCRIPTION)
BUILD   := $(PACKAGE)_$(VERSION).tar.gz

RHUB_ALL_PLATFORMS := c( \
  "linux",      "m1-san",      "macos",        "macos-arm64",  "windows", \
  "atlas",      "c23",         "clang-asan",   "clang-ubsan",  "clang16", \
  "clang17",    "clang18",     "clang19",      "clang20",      "donttest", \
  "gcc-asan",   "gcc13",       "gcc14",        "gcc15",        "intel", \
  "mkl",        "nold",        "noremap",      "nosuggests",   "rchk", \
  "ubuntu-clang","ubuntu-gcc12","ubuntu-next", "ubuntu-release","valgrind" \
)

.PHONY: doc build install check check-no-vignette check-rhub test bench vignette clean $(BUILD)

check: $(BUILD)
	R CMD check --as-cran $<

check-no-vignette: $(BUILD)
	R CMD check --as-cran --no-build-vignettes $<

check-rhub: $(BUILD)
	Rscript -e 'rhub::rhub_check(platform = $(RHUB_ALL_PLATFORMS))'

doc:
	Rscript -e 'roxygen2::roxygenise(roclets = "rd")'

build: clean doc
	R CMD build .

install: build
	R CMD INSTALL $(BUILD)

test:
	tmp_lib=$$(mktemp -d /tmp/$(PACKAGE)-lib-XXXXXX); \
	R CMD INSTALL -l $$tmp_lib .; \
	for f in tests/test_*.R; do \
	  echo "== $$f"; \
	  Rscript -e '.libPaths(c(commandArgs(TRUE)[2], .libPaths())); source(commandArgs(TRUE)[1])' $$f $$tmp_lib || exit 1; \
	done; \
	rm -rf $$tmp_lib

bench:
	Rscript inst/extra/benchmark.R 5

# ASan + UBSan over the full test suite. Only the package (and the
# test-compiled consumer DSO, via R_MAKEVARS_USER) is instrumented -- R
# itself is not, so libasan must be preloaded and leak checking disabled
# (the R interpreter intentionally leaks at exit).
test-san:
	tmp_lib=$$(mktemp -d /tmp/$(PACKAGE)-san-XXXXXX); \
	printf 'CXXFLAGS = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all\nSHLIB_CXXLDFLAGS = -fsanitize=address,undefined -shared\n' > $$tmp_lib/Makevars.san; \
	R_MAKEVARS_USER=$$tmp_lib/Makevars.san R CMD INSTALL -l $$tmp_lib .; \
	for f in tests/test_*.R; do \
	  echo "== $$f"; \
	  LD_PRELOAD=$$(gcc -print-file-name=libasan.so) ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
	  R_MAKEVARS_USER=$$tmp_lib/Makevars.san R_LIBS=$$tmp_lib Rscript $$f || exit 1; \
	done; \
	rm -rf $$tmp_lib

# Full suite under valgrind memcheck (no rebuild; slow). Requires valgrind.
test-valgrind:
	tmp_lib=$$(mktemp -d /tmp/$(PACKAGE)-vg-XXXXXX); \
	R CMD INSTALL -l $$tmp_lib .; \
	for f in tests/test_*.R; do \
	  echo "== $$f"; \
	  R_LIBS=$$tmp_lib R --vanilla -d "valgrind --tool=memcheck --leak-check=no --error-exitcode=1" -f $$f || exit 1; \
	done; \
	rm -rf $$tmp_lib

vignette:
	Rscript -e "rmarkdown::render(input='vignettes/vignette.rmd', output_format='html_vignette')"
	IS_GITHUB=Yes Rscript -e "rmarkdown::render(input='vignettes/vignette.rmd', output_file='../README.md', output_format=rmarkdown::github_document(html_preview=FALSE))"

clean:
	find . -iname "*.o" -exec rm {} \;
	find . -iname "*.so" -exec rm {} \;
	find . -iname "*.dll" -exec rm {} \;
	rm -f $(PACKAGE)_*.tar.gz
	rm -rf $(PACKAGE).Rcheck
