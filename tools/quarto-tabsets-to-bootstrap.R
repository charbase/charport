#!/usr/bin/env Rscript

# Workaround for pkgdown 2.2.0 with Quarto 1.9.38.
#
# pkgdown's Quarto article pipeline currently renders Quarto panel tabsets as
# inert `.panel-tabset-tabby` markup, and in some cases partially rewrites the
# first panel into malformed Bootstrap tab markup. pkgdown already ships
# Bootstrap for R Markdown tabsets, so this post-process converts Quarto's
# generated tabset HTML into the Bootstrap tab structure pkgdown can activate.
# Remove this once pkgdown natively supports Quarto `.panel-tabset` articles.

suppressPackageStartupMessages(library(xml2))

args <- commandArgs(trailingOnly = TRUE)
files <- if (length(args)) {
  args
} else {
  c(
    if (dir.exists("vignettes")) {
      list.files("vignettes", pattern = "[.]html$", full.names = TRUE)
    },
    if (dir.exists("docs/articles")) {
      list.files("docs/articles", pattern = "[.]html$", full.names = TRUE)
    }
  )
}
files <- files[file.exists(files)]

escape_html <- function(x) {
  x <- gsub("&", "&amp;", x, fixed = TRUE)
  x <- gsub("<", "&lt;", x, fixed = TRUE)
  x <- gsub(">", "&gt;", x, fixed = TRUE)
  x <- gsub("\"", "&quot;", x, fixed = TRUE)
  x
}

node_inner_html <- function(node) {
  if (inherits(node, "xml_missing")) {
    return("")
  }
  paste(vapply(xml2::xml_children(node), as.character, character(1)), collapse = "\n")
}

fragment_first_node <- function(html) {
  doc <- xml2::read_html(
    paste0("<!doctype html><html><body>", html, "</body></html>"),
    options = c("RECOVER", "NOERROR", "NOWARNING")
  )
  xml2::xml_find_first(doc, "//body/*[1]")
}

convert_tabset <- function(tabset) {
  tablist <- xml2::xml_find_first(
    tabset,
    "./ul[contains(concat(' ', normalize-space(@class), ' '), ' panel-tabset-tabby ')]"
  )
  if (inherits(tablist, "xml_missing")) {
    return(FALSE)
  }

  tabs <- xml2::xml_find_all(tablist, "./li/a[starts-with(@href, '#')]")
  if (!length(tabs)) {
    return(FALSE)
  }
  default <- which(!is.na(vapply(tabs, xml2::xml_attr, character(1), "data-tabby-default")))
  active_index <- if (length(default)) default[[1]] else 1L

  tabset_id <- xml2::xml_attr(tablist, "id")
  if (is.na(tabset_id) || !nzchar(tabset_id)) {
    tabset_id <- paste0("tabset-", as.integer(runif(1, 1e6, 1e7)))
  }

  any_converted <- FALSE
  tab_items <- character(length(tabs))
  pane_items <- character(length(tabs))

  for (i in seq_along(tabs)) {
    tab <- tabs[[i]]
    pane_id <- sub("^#", "", xml2::xml_attr(tab, "href"))
    if (!nzchar(pane_id)) {
      next
    }

    tab_id <- xml2::xml_attr(tab, "id")
    if (is.na(tab_id) || !nzchar(tab_id)) {
      tab_id <- paste0(pane_id, "-tab")
    }
    active <- identical(i, active_index)
    label <- escape_html(xml2::xml_text(tab, trim = TRUE))

    pane <- xml2::xml_find_first(tabset, paste0(".//div[@id='", pane_id, "']"))
    pane_html <- node_inner_html(pane)

    # pkgdown currently half-processes the first Quarto panel into a Bootstrap
    # nav button with the pane contents inside the button. Recover that content
    # when Quarto's original pane wrapper is no longer present.
    if (!nzchar(pane_html) && active) {
      fallback <- xml2::xml_find_first(
        tabset,
        paste0(
          "./ul[contains(concat(' ', normalize-space(@class), ' '), ' nav-tabs ')",
          " and not(contains(concat(' ', normalize-space(@class), ' '), ' panel-tabset-tabby '))]",
          "//button"
        )
      )
      pane_html <- node_inner_html(fallback)
    }

    active_class <- if (active) " active" else ""
    active_pane_class <- if (active) " show active" else ""
    tabindex <- if (active) "" else ' tabindex="-1"'
    selected <- if (active) "true" else "false"

    tab_items[[i]] <- paste0(
      '<li class="nav-item" role="presentation">',
      '<a href="#', escape_html(pane_id), '" id="', escape_html(tab_id), '"',
      ' class="nav-link', active_class, '" data-bs-toggle="tab"',
      ' data-bs-target="#', escape_html(pane_id), '" role="tab"',
      ' aria-controls="', escape_html(pane_id), '"',
      ' aria-selected="', selected, '"', tabindex, ">",
      label,
      "</a></li>"
    )
    pane_items[[i]] <- paste0(
      '<div id="', escape_html(pane_id), '" class="tab-pane fade',
      active_pane_class, '" role="tabpanel" aria-labelledby="',
      escape_html(tab_id), '" tabindex="0">',
      pane_html,
      "</div>"
    )
    any_converted <- TRUE
  }

  if (!any_converted) {
    return(FALSE)
  }

  replacement <- paste0(
    '<div class="pkgdown-tabset tabset">',
    '<ul id="', escape_html(tabset_id), '" class="nav nav-tabs" role="tablist">',
    paste(tab_items, collapse = ""),
    "</ul>",
    '<div class="tab-content">',
    paste(pane_items, collapse = ""),
    "</div>",
    "</div>"
  )
  xml2::xml_replace(tabset, fragment_first_node(replacement))
  TRUE
}

converted <- 0L
for (file in files) {
  doc <- xml2::read_html(file, options = c("RECOVER", "NOERROR", "NOWARNING"))
  changed <- FALSE
  tabsets_converted <- 0L
  tabsets <- xml2::xml_find_all(
    doc,
    "//div[contains(concat(' ', normalize-space(@class), ' '), ' panel-tabset ')]"
  )
  for (tabset in tabsets) {
    if (convert_tabset(tabset)) {
      tabsets_converted <- tabsets_converted + 1L
      changed <- TRUE
    }
  }
  if (changed) {
    xml2::write_html(doc, file, options = "format")
    converted <- converted + tabsets_converted
    details <- c(
      if (tabsets_converted) {
        paste0(
          tabsets_converted,
          " tabset",
          if (tabsets_converted == 1L) "" else "s"
        )
      }
    )
    message("Updated ", file, " (", paste(details, collapse = ", "), ")")
  }
}

message("Converted ", converted, " Quarto tabset", if (converted == 1L) "" else "s", ".")
