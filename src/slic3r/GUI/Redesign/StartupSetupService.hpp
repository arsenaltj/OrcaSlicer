#pragma once

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::GUI {

enum class StartupSetupPage { Welcome, Region, Printers, Filaments };
enum class StartupSetupRegion { China, AsiaPacific, Europe, NorthAmerica };

struct StartupSetupFilament {
    std::string preset;
    std::string label;
    std::string type;
    std::string colour;
};

struct StartupSetupPrinter {
    std::string model;
    std::string label;
    std::string preset;
    std::string image_path;
    size_t head_count = 0;
    std::vector<StartupSetupFilament> filaments;
    std::string error;
};

struct StartupSetupHead {
    std::string filament;
    std::string colour;
};

struct StartupSetupDraft {
    StartupSetupRegion region = StartupSetupRegion::China;
    std::string printer_model;
    std::vector<StartupSetupHead> heads;
    std::vector<std::string> custom_colours;
};

struct StartupSetupSnapshot {
    StartupSetupPage page = StartupSetupPage::Welcome;
    std::vector<StartupSetupPrinter> printers;
    StartupSetupDraft draft;
    size_t active_head = 0;
    bool loading = false;
    bool submitting = false;
    bool completed = false;
    bool cancelled = false;
    bool can_next = true;
    bool can_finish = false;
    std::string error;
};

// The legacy guide and native setup use the same preset installation boundary.
bool apply_startup_vendor_selection(PresetBundle& bundle, AppConfig& config,
    const AppConfig::VendorMap& vendors, const std::map<std::string, std::string>& filaments,
    bool overwrite, const std::string& model, const std::string& variant,
    const std::string& preferred_filament = {});

class StartupSetupService final
{
public:
    using SaveConfig = std::function<void(AppConfig&)>;

    explicit StartupSetupService(const AppConfig& config);
    static bool needs_setup(bool config_exists, bool valid_printer);
    static StartupSetupRegion region_from_config(const std::string& value);
    static std::string region_config_value(StartupSetupRegion value);
    static bool valid_colour(const std::string& value);
    static std::vector<StartupSetupPrinter> catalog_from_bundle(const PresetBundle& bundle,
                                                               const std::string& resources);
    static std::vector<StartupSetupPrinter> load_catalog(const std::string& resources,
        const std::string& data, const std::atomic<bool>& cancel);

    StartupSetupSnapshot snapshot() const;
    std::uint64_t begin_load();
    bool finish_load(std::uint64_t revision, std::vector<StartupSetupPrinter> printers,
                     std::string error = {});
    bool choose_region(StartupSetupRegion region);
    bool choose_printer(const std::string& model);
    bool choose_head(size_t index);
    bool choose_filament(const std::string& name);
    bool choose_colour(const std::string& colour);
    void set_custom_colours(std::vector<std::string> colours);
    bool next();
    bool back();
    void cancel();
    bool complete(AppConfig& config, PresetBundle& bundle, SaveConfig save = {});

private:
    const StartupSetupPrinter* selected_printer() const;
    bool editable() const;
    bool valid_draft() const;
    void reconcile_heads();

    StartupSetupSnapshot m_state;
    std::uint64_t m_revision = 0;
};

}
