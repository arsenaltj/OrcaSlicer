#pragma once

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { class Model; }

namespace Slic3r::GUI::PrinterSettings {

struct Choice {
    std::string vendor, model, variant, preset;
    double diameter = 0;
    bool operator==(const Choice& other) const;
};

struct Machine {
    std::string name;
    std::vector<double> diameters;
    Vec3d space {0., 0., 0.};
};

struct Material {
    size_t slot;
    std::string name, type, color, recipe, routing;
    bool mixed, valid;
};

struct Plate {
    const DynamicPrintConfig* config = nullptr;
    std::vector<size_t> objects;
};

struct Reference {
    enum class Kind { Object, Part, Layer, Paint, Support, ToolChange, WipeTower, PrintOrder, Custom };
    size_t plate, slot; // Zero-based, with the plate index taken from the project.
    Kind kind;
};

struct Impact {
    Reference reference;
    bool blocking;
    std::string before, after;
};

struct Plan {
    Choice target;
    Machine machine;
    std::vector<Material> materials;
    std::vector<Impact> impacts;
    std::vector<size_t> replaced_saved_slots;
    bool remembered = false;
    std::string error;
    std::string fingerprint;
    std::unique_ptr<PresetBundle> staged;
    std::unique_ptr<AppConfig> persistence;
};

enum class Result { Applied, NoChange, Busy, Stale, Blocked, SaveFailed, RestoreFailed };
using Save = std::function<bool(AppConfig&)>;
using Publish = std::function<void(const PresetBundle&)>;

// Catalog and planning read only. The additional bundle contains just the shipped vendor(s)
// that are missing from the installed preset collection.
std::unique_ptr<PresetBundle> read_missing_catalog(const PresetBundle& live);
std::vector<Choice> choices(const PresetBundle& live, const PresetBundle* additional = nullptr);
Machine describe(const Preset& preset);
std::vector<Reference> collect_references(const Model& model, const std::vector<Plate>& plates);
Plan prepare(const PresetBundle& live, const AppConfig& config, const Choice& target,
             const Model& model, const std::vector<Plate>& plates, const PresetBundle* additional = nullptr);
bool blocks(const Plan& plan);
bool save_checked(AppConfig& config);
// The caller is on the GUI thread and owns the busy check and the final refresh. The
// optional publish hook is before that refresh, while restoring is still possible.
Result apply(PresetBundle& live, AppConfig& config, const Plan& reviewed, const Model& model,
             const std::vector<Plate>& plates, const PresetBundle* additional = nullptr,
             Save save = {}, Publish before_publish = {});

} // namespace Slic3r::GUI::PrinterSettings
