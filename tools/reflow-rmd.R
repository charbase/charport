args <- commandArgs(trailingOnly = TRUE)

width <- 80L
if(length(args) >= 2L && args[[1L]] %in% c("-w", "--width")) {
  width <- as.integer(args[[2L]])
  args <- args[-c(1L, 2L)]
}

if(!length(args) || is.na(width) || width < 20L) {
  stop(
    "Usage: Rscript tools/reflow-rmd.R [--width 80] file [file ...]",
    call. = FALSE
  )
}

is_blank <- function(x) {
  grepl("^\\s*$", x)
}

is_fence <- function(x) {
  grepl("^\\s*(```|~~~)", x)
}

is_plain_line <- function(x) {
  !grepl("^\\s", x) &
    !grepl("^(#{1,6}\\s|[-+*]\\s|[-+*]{3,}\\s*$|\\d+[.)]\\s|>\\s?|\\|)", x) &
    !grepl("^\\[[^]]+\\]:", x) &
    !grepl("^<[^>]+>", x) &
    !grepl("^:::", x) &
    !grepl("\\|", x) &
    !is_fence(x)
}

can_reflow_block <- function(block) {
  length(block) > 0L &&
    all(is_plain_line(block)) &&
    !any(grepl("\\s{2,}$", block))
}

wrap_block <- function(block, width) {
  text <- paste(trimws(block), collapse = " ")
  strwrap(text, width = width)
}

reflow_file <- function(path, width) {
  lines <- readLines(path, warn = FALSE)
  out <- character()
  block <- character()
  in_yaml <- FALSE
  yaml_open <- FALSE
  in_fence <- FALSE

  flush_block <- function() {
    if(can_reflow_block(block)) {
      out <<- c(out, wrap_block(block, width))
    } else if(length(block)) {
      out <<- c(out, block)
    }
    block <<- character()
  }

  for(i in seq_along(lines)) {
    line <- lines[[i]]

    if(i == 1L && identical(line, "---")) {
      flush_block()
      in_yaml <- TRUE
      yaml_open <- TRUE
      out <- c(out, line)
      next
    }

    if(in_yaml) {
      out <- c(out, line)
      if(yaml_open && i > 1L && grepl("^(---|\\.\\.\\.)\\s*$", line)) {
        in_yaml <- FALSE
      }
      next
    }

    if(in_fence) {
      out <- c(out, line)
      if(is_fence(line)) {
        in_fence <- FALSE
      }
      next
    }

    if(is_fence(line)) {
      flush_block()
      in_fence <- TRUE
      out <- c(out, line)
      next
    }

    if(is_blank(line)) {
      flush_block()
      out <- c(out, line)
      next
    }

    block <- c(block, line)
  }

  flush_block()

  if(!identical(lines, out)) {
    writeLines(out, path, useBytes = TRUE)
    message("Reflowed: ", path)
  }
}

for(path in args) {
  reflow_file(path, width)
}
