#pragma once

#include <array>

namespace Slic3r { namespace tex2color { namespace color_utils { namespace detail {

// Internal implementation/test seam, not a supported public API.
// Rejects fast/finite-only math, then verifies double evaluation, FE_TONEAREST
// and default SSE2 MXCSR controls on x86/x64;
// unsupported environments conservatively reject prepared distances.
bool has_default_lab_chroma_environment();

// Success preserves exactly sqrt(a*a+b*b). Non-finite inputs/results or an
// unqualified current FP environment fail without changing the output.
bool prepare_lab_chroma(const std::array<double, 3>& lab, double& chroma);

// Both chroma values must have been successfully prepared from the exact
// corresponding immutable inputs, in the same verified default FP mode.
// Pair-dependent CIEDE2000 terms are not cached.
double ciede2000_prepared(const std::array<double, 3>& lab1,
                          const std::array<double, 3>& lab2,
                          double chroma1, double chroma2);

}}}} // namespace Slic3r::tex2color::color_utils::detail
