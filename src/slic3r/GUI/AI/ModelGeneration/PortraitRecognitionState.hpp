#pragma once

namespace Slic3r::GUI::PortraitPrint {

inline const char* recognition_status(bool person_detected, bool evidence_available, bool applied)
{
    if (applied) return "partial_regions_applied";
    if (!evidence_available) return "unavailable";
    return person_detected ? "person_without_reliable_bindings" : "no_person";
}

} // namespace Slic3r::GUI::PortraitPrint
