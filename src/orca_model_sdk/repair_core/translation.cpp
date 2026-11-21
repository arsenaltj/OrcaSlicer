#include "libslic3r/I18N.hpp"
// No desktop GUI translation callback exists in a headless SDK process.
namespace Slic3r { namespace I18N { translate_fn_type translate_fn = nullptr; } }
