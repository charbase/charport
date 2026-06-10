.onUnload <- function(libpath) {
  library.dynam.unload("charport", libpath)
}
