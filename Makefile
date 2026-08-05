SHELL   := /bin/bash
PACKAGE := $(shell perl -aF: -ne 'print, exit if s/^Package:\s+//' DESCRIPTION)
VERSION := $(shell perl -aF: -ne 'print, exit if s/^Version:\s+//' DESCRIPTION)
BUILD   := $(PACKAGE)_$(VERSION).tar.gz

.PHONY: doc build install check check-no-vignette rhub-platforms test test-cxx \
	bench bench-list vignette \
	reflow-docs pkgdown pkgdown-index clean-pkgdown clean clean-native \
	clean-build-products

check: $(BUILD)
	R CMD check --as-cran $<

check-no-vignette: $(BUILD)
	R CMD check --as-cran --no-build-vignettes $<

# R-hub runs from .github/workflows/rhub.yaml, triggered by hand from the
# Actions tab. The platform list is hardcoded there so a run is reproducible,
# but upstream adds platforms a few times a year and has never removed one, so
# a stale list under-tests silently instead of failing. This compares the
# workflow against upstream and offers to rewrite it. The pinned action
# revision resolves the platform names, so the pin and the list are rewritten
# together and always come from one upstream revision. Upstream force-moves the
# v1 tag on every release, so each pin is annotated with the exact semver tag it
# came from and that annotation is rewritten too.
RHUB_WORKFLOW := .github/workflows/rhub.yaml
RHUB_ACTIONS  := https://raw.githubusercontent.com/r-hub/actions

rhub-platforms:
	@set -o pipefail; \
	for tool in curl jq; do \
	  command -v $$tool >/dev/null || { echo "$$tool is required." >&2; exit 1; }; \
	done; \
	have_list=$$(sed -n "s/^.*default: '\([^']*\)'.*# rhub-platforms.*$$/\1/p" $(RHUB_WORKFLOW)); \
	have_sha=$$(sed -n 's|^.*r-hub/actions/[a-z-]*@\([0-9a-f]\{40\}\).*$$|\1|p' $(RHUB_WORKFLOW) | sort -u); \
	have_ver=$$(sed -n 's|^.*r-hub/actions/[a-z-]*@[0-9a-f]\{40\} # \(v[0-9.]*\).*$$|\1|p' $(RHUB_WORKFLOW) | sort -u | head -1); \
	if [ -z "$$have_list" ]; then \
	  echo "No '# rhub-platforms' marker in $(RHUB_WORKFLOW)." >&2; exit 1; \
	fi; \
	if [ "$$(echo "$$have_sha" | wc -l)" -ne 1 ]; then \
	  echo "$(RHUB_WORKFLOW) pins more than one r-hub/actions revision:" >&2; \
	  echo "$$have_sha" >&2; exit 1; \
	fi; \
	sha=$$(curl -fsSL https://api.github.com/repos/r-hub/actions/commits/v1 | jq -r '.sha'); \
	case "$$sha" in [0-9a-f]*) ;; *) echo "Could not resolve the r-hub/actions v1 tip." >&2; exit 1;; esac; \
	ver=$$(curl -fsSL 'https://api.github.com/repos/r-hub/actions/tags?per_page=100' | jq -r --arg sha "$$sha" '[.[] | select(.commit.sha == $$sha) | .name | select(test("^v[0-9]+\\.[0-9]+\\.[0-9]+$$"))] | first // "v1"'); \
	list=$$(curl -fsSL $(RHUB_ACTIONS)/$$sha/setup/platforms.json | jq -r '[.[].name] | join(",")'); \
	if [ -z "$$list" ]; then echo "Could not read platforms.json at $$sha." >&2; exit 1; fi; \
	if [ "$$have_list" = "$$list" ] && [ "$$have_sha" = "$$sha" ]; then \
	  echo "$(RHUB_WORKFLOW) is current: $$(echo "$$list" | tr ',' '\n' | wc -l) platforms at $$ver ($${sha:0:7})."; \
	  exit 0; \
	fi; \
	if [ "$$have_list" != "$$list" ]; then \
	  echo "The r-hub platform list has changed:"; \
	  diff <(echo "$$have_list" | tr ',' '\n' | sort) \
	       <(echo "$$list" | tr ',' '\n' | sort) \
	    | sed -n 's/^< /  removed: /p; s/^> /  added:   /p'; \
	else \
	  echo "The platform list is unchanged."; \
	fi; \
	if [ "$$have_sha" != "$$sha" ]; then \
	  echo "The r-hub/actions pin has moved: $$have_ver ($${have_sha:0:7}) -> $$ver ($${sha:0:7})."; \
	fi; \
	read -r -p "Rewrite $(RHUB_WORKFLOW)? [y/N] " reply; \
	case "$$reply" in [yY]*) ;; *) echo "Left unchanged."; exit 0;; esac; \
	sed -i \
	  -e "s|^\(\s*default: \)'[^']*'\(.*# rhub-platforms.*\)$$|\1'$$list'\2|" \
	  -e "s|\(r-hub/actions/[a-z-]*\)@[0-9a-f]\{40\} # v[0-9.]*|\1@$$sha # $$ver|g" \
	  $(RHUB_WORKFLOW); \
	echo "Updated $(RHUB_WORKFLOW). Review it, commit it, and push it to the"; \
	echo "default branch, or the Run workflow button will use the old list."

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

test-cxx:
	bash tools/check-cxx-standards.sh

bench:
	Rscript tools/benchmark/benchmark.R 5

bench-list:
	Rscript tools/benchmark/benchmark_list.R 5 1 100000

# ASan + UBSan over the full test suite. Only the package (and the
# test-compiled consumer library, via R_MAKEVARS_USER) is instrumented -- R
# itself is not, so libasan must be preloaded and leak checking disabled
# (the R interpreter intentionally leaks at exit).
#
# --no-test-load is REQUIRED: R CMD INSTALL's post-install load test runs R
# without the libasan LD_PRELOAD, so loading the instrumented .so aborts with
# "ASan runtime does not come first" and INSTALL removes the package. Without
# this flag the temp lib ends up empty and the suite silently falls through to
# the uninstrumented user-library charport -- i.e. it stops instrumenting at
# all. The test loop below does the real (preloaded) load.
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
	XDG_CACHE_HOME=$(CURDIR)/local/cache quarto render vignettes/error-handling.qmd --to html
	XDG_CACHE_HOME=$(CURDIR)/local/cache quarto render vignettes/design-rationale.qmd --to html

reflow-docs:
	Rscript tools/reflow-rmd.R --width 80 vignettes/charport.qmd vignettes/developer-guide.qmd vignettes/error-handling.qmd vignettes/design-rationale.qmd

pkgdown: clean-native clean-pkgdown doc
	$(MAKE) pkgdown-index
	mkdir -p local/cache
	XDG_CACHE_HOME=$(CURDIR)/local/cache R_USER_CACHE_DIR=$(CURDIR)/local/cache/R \
	  IN_PKGDOWN=true Rscript -e 'pkgdown::build_site(new_process = FALSE, install = FALSE, quiet = FALSE, override = list(home = list(sidebar = FALSE)))'
	Rscript tools/quarto-tabsets-to-bootstrap.R docs/articles/*.html
	# pkgdown turns every root-level .md into a page and its file list is hard
	# coded, so CLAUDE.md becomes CLAUDE.html with no way to configure it out.
	# The CI workflow that publishes the site drops the same two files.
	rm -f docs/CLAUDE.html docs/CLAUDE.md
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
	rm -f $(PACKAGE)_*.tar.gz
	rm -rf $(PACKAGE).Rcheck ..Rcheck
	rm -rf docs
