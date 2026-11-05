// Compile the vendored parser implementation once for the standalone SDK.
// The normal desktop provides the same implementation through BitmapCache.
#define NANOSVG_IMPLEMENTATION
#include "../deps_src/nanosvg/nanosvg.h"
