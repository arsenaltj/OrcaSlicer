#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IOrcaWorkspace.hpp"
#include <optional>
#include <chrono>

class wxWindow;
namespace Slic3r::GUI {
class Plater;
enum class OrcaPrintConnectionState { Unconfigured, SelectionRequired, Unknown, Offline, Online };
struct OrcaPrintConnectionFacts {
    bool native_backend {false};
    bool web_entry_configured {false};
    bool native_selected {false};
    bool native_online {false};
    bool native_connected {false};
    std::optional<std::chrono::milliseconds> telemetry_age;
};
// Configuration or a loaded WebView never proves a live device. Only the
// currently selected native backend's fresh authoritative telemetry can do so.
inline OrcaPrintConnectionState orca_print_connection_state(const OrcaPrintConnectionFacts& facts) noexcept
{
    if (!facts.native_backend)
        return facts.web_entry_configured ? OrcaPrintConnectionState::Unknown : OrcaPrintConnectionState::Unconfigured;
    if (!facts.native_selected)
        return OrcaPrintConnectionState::SelectionRequired;
    if (!facts.native_online)
        return OrcaPrintConnectionState::Offline;
    if (!facts.native_connected || !facts.telemetry_age ||
        facts.telemetry_age->count() < 0 || facts.telemetry_age->count() > 15000)
        return OrcaPrintConnectionState::Unknown;
    return OrcaPrintConnectionState::Online;
}

enum class OrcaDeviceMediaState { ConnectionRequired, InformationPending, Unsupported, StorageUnavailable, Busy, Available };
struct OrcaDeviceMediaFacts {
    OrcaPrintConnectionFacts connection;
    bool information_ready {false};
    bool storage_readable {false};
    bool file_protocol_supported {false};
    bool busy {false};
};
// Viewing existing files is independent from slice readiness and live camera.
// Fail closed on stale/backend-incompatible information, before any file request.
inline OrcaDeviceMediaState orca_device_media_state(const OrcaDeviceMediaFacts& facts) noexcept
{
    if (orca_print_connection_state(facts.connection) != OrcaPrintConnectionState::Online)
        return OrcaDeviceMediaState::ConnectionRequired;
    if (!facts.information_ready)
        return OrcaDeviceMediaState::InformationPending;
    if (!facts.file_protocol_supported)
        return OrcaDeviceMediaState::Unsupported;
    if (!facts.storage_readable)
        return OrcaDeviceMediaState::StorageUnavailable;
    if (facts.busy)
        return OrcaDeviceMediaState::Busy;
    return OrcaDeviceMediaState::Available;
}

// A confirmation is bound to every existing workspace input revision, not
// just a still-present native G-code file. Read failures fail closed.
inline std::optional<AI::SmartSlicing::WorkspaceRevision>
capture_orca_print_confirmation_revision(const AI::SmartSlicing::IOrcaWorkspace& workspace) noexcept
{
    try {
        auto revision = workspace.current_revision();
        if (revision.valid())
            return revision;
    } catch (...) {}
    return std::nullopt;
}

inline bool orca_print_confirmation_revision_current(
    const std::optional<AI::SmartSlicing::WorkspaceRevision>& confirmed,
    const AI::SmartSlicing::IOrcaWorkspace& workspace) noexcept
{
    if (!confirmed || !confirmed->valid())
        return false;
    const auto current = capture_orca_print_confirmation_revision(workspace);
    return current && *current == *confirmed;
}

// Read-only review followed by the existing native export/device confirmation.
// This view owns no print job and never dispatches on opening.
void show_orca_print_confirmation(wxWindow* parent, Plater& plater);
}
