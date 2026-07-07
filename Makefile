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

.PHONY: doc build install check check-no-vignette check-rhub test bench vignette \
	reflow-docs pkgdown pkgdown-index clean-pkgdown clean clean-native \
	clean-build-products

check: $(BUILD)
	R CMD check --as-cran $<

check-no-vignette: $(BUILD)
	R CMD check --as-cran --no-build-vignettes $<

check-rhub: $(BUILD)
	Rscript -e 'rhub::rhub_check(platform = $(RHUB_ALL_PLATFORMS))'

doc:
	Rscript -e 'roxygen2::roxygenise(roclets = "rd")'

$(BUILD): clean-native doc
	rm -f $(BUILD)
	R CMD build .
	$(MAKE) clean-native

build: $(BUILD)

install: $(BUILD)
	$(MAKE) clean-native
	R CMD INSTALL $(BUILD)
	$(MAKE) clean-native
	rm -f $(BUILD)

test: install
	for f in tests/test_*.R; do \
	  echo "== $$f"; \
	  Rscript $$f || exit 1; \
	done

bench:
	Rscript inst/extra/benchmark.R 5

# ASan + UBSan over the full test suite. Only the package (and the
# test-compiled consumer DSO, via R_MAKEVARS_USER) is instrumented. R
# itself is not, so libasan must be preloaded and leak checking disabled
# (the R interpreter intentionally leaks at exit).
#
# --no-test-load is required because R CMD INSTALL's post-install load test
# runs without the libasan LD_PRELOAD. Loading the instrumented .so then
# aborts with "ASan runtime does not come first", and INSTALL removes the
# package. Without this flag, the empty temporary library can cause the suite
# to load an uninstrumented user-library copy of charport. The test loop below
# performs the preloaded load.
test-san:
	tmp_lib=$$(mktemp -d /tmp/$(PACKAGE)-san-XXXXXX); \
	printf 'CXXFLAGS = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all\nSHLIB_CXXLDFLAGS = -fsanitize=address,undefined -shared\n' > $$tmp_lib/Makevars.san; \
	R_MAKEVARS_USER=$$tmp_lib/Makevars.san R CMD INSTALL --preclean --no-test-load -l $$tmp_lib .; \
	for f in tests/test_*.R; do \
	  echo "== $$f"; \
	  LD_PRELOAD=$$(gcc -print-file-name=libasan.so) ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
	  R_MAKEVARS_USER=$$tmp_lib/Makevars.san R_LIBS=$$tmp_lib Rscript $$f || exit 1; \
	done; \
	rm -rf $$tmp_lib

# Full suite under valgrind memcheck (no rebuild; slow). Requires valgrind.
test-valgrind:
	tmp_lib=$$(mktemp -d /tmp/$(PACKAGE)-vg-XXXXXX); \
	R CMD INSTALL --preclean -l $$tmp_lib .; \
	for f in tests/test_*.R; do \
	  echo "== $$f"; \
	  R_LIBS=$$tmp_lib R --vanilla -d "valgrind --tool=memcheck --leak-check=no --error-exitcode=1" -f $$f || exit 1; \
	done; \
	rm -rf $$tmp_lib

vignette:
	mkdir -p local/cache
	XDG_CACHE_HOME=$(CURDIR)/local/cache quarto render vignettes/charport.qmd --to html
	XDG_CACHE_HOME=$(CURDIR)/local/cache quarto render vignettes/charport.qmd --to gfm --output README.md --output-dir .
	XDG_CACHE_HOME=$(CURDIR)/local/cache quarto render vignettes/developer-guide.qmd --to html
	XDG_CACHE_HOME=$(CURDIR)/local/cache quarto render vignettes/design-rationale.qmd --to html
	Rscript tools/quarto-tabsets-to-bootstrap.R vignettes/*.html

reflow-docs:
	Rscript tools/reflow-rmd.R --width 80 vignettes/charport.qmd vignettes/developer-guide.qmd vignettes/design-rationale.qmd

pkgdown: clean-native clean-pkgdown doc
	$(MAKE) pkgdown-index
	mkdir -p local/cache
	XDG_CACHE_HOME=$(CURDIR)/local/cache R_USER_CACHE_DIR=$(CURDIR)/local/cache/R \
	  IN_PKGDOWN=true Rscript -e 'pkgdown::build_site(new_process = FALSE, install = FALSE, quiet = FALSE, override = list(home = list(sidebar = FALSE)))'
	Rscript tools/quarto-tabsets-to-bootstrap.R docs/articles/*.html
	$(MAKE) clean-native

pkgdown-index:
	mkdir -p pkgdown
	Rscript -e 'x <- readLines("README.md", warn = FALSE); keep <- !grepl("^<img src=\"man/figures/logo\\.svg\"", x); writeLines(x[keep], "pkgdown/index.md")'

clean-pkgdown:
	rm -rf docs
	rm -f pkgdown/index.md

clean: clean-native clean-build-products

clean-native:
	find . -iname "*.a" -exec rm {} \;
	find . -iname "*.o" -exec rm {} \;
	find . -iname "*.so" -exec rm {} \;
	find . -iname "*.dll" -exec rm {} \;
	rm -f src/symbols.rds

clean-build-products:
	rm -f vignettes/*.html
	rm -f $(PACKAGE)_*.tar.gz
	rm -rf $(PACKAGE).Rcheck ..Rcheck
	rm -rf docs
