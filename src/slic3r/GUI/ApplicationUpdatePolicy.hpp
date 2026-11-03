#pragma once

namespace Slic3r::GUI {
// Re-enable together with the Honor Forge update service and its new UX.
// Preset and networking-plugin updates have independent policies.
inline constexpr bool application_updates_enabled() { return false; }
}
