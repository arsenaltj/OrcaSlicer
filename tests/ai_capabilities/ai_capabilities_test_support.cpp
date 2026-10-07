// libslic3r's SVG helpers expect the executable to own the NanoSVG implementation.
// Match the existing headless libslic3r and FFF test executables.
#include "libslic3r/libslic3r.h"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
