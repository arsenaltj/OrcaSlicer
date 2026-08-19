#include "OrcaSmartSlicingAdapter.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Slic3r::GUI {
namespace {

using namespace AI::SmartSlicing;

uint64_t fnv1a(std::string_view text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex_hash(uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

void append_matrix(std::ostringstream& stream, const Transform3d& matrix)
{
    for (Eigen::Index row = 0; row < matrix.rows(); ++row)
        for (Eigen::Index column = 0; column < matrix.cols(); ++column)
            stream << matrix(row, column) << ',';
}

std::string canonical_config(const DynamicPrintConfig& config)
{
    std::vector<std::string> keys = config.keys();
    std::sort(keys.begin(), keys.end());
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    for (const std::string& key : keys)
        stream << key << '=' << config.opt_serialize(key) << '\n';
    return stream.str();
}

} // namespace

AI::SmartSlicing::WorkspaceRevision OrcaSmartSlicingAdapter::current_revision() const { return capture_context_impl(false).revision; }

AI::SmartSlicing::WorkspaceContext OrcaSmartSlicingAdapter::capture_context() const { return capture_context_impl(true); }

AI::SmartSlicing::WorkspaceContext OrcaSmartSlicingAdapter::capture_context_impl(bool include_diagnostics) const
{
    using namespace AI::SmartSlicing;
    if (m_plater == nullptr || wxGetApp().preset_bundle == nullptr)
        throw std::runtime_error("Orca workspace is unavailable.");

    WorkspaceContext context;
    PresetBundle& bundle  = *wxGetApp().preset_bundle;
    PartPlateList& plates = m_plater->get_partplate_list();
    PartPlate* plate      = plates.get_curr_plate();
    if (plate == nullptr)
        throw std::runtime_error("The current plate is unavailable.");

    context.plate_index       = plates.get_curr_plate_index();
    context.printer_preset_id = bundle.printers.get_edited_preset().name;
    context.process_preset_id = bundle.prints.get_edited_preset().name;
    context.bed_type          = std::to_string(static_cast<int>(plate->get_bed_type(true)));

    const DynamicPrintConfig full_config = bundle.full_config();
    if (const auto* nozzles = full_config.option<ConfigOptionFloats>("nozzle_diameter"))
        context.nozzle_diameters = nozzles->values;

    const auto* colors = full_config.option<ConfigOptionStrings>("filament_colour");
    context.materials.reserve(bundle.filament_presets.size());
    for (size_t index = 0; index < bundle.filament_presets.size(); ++index) {
        MaterialSnapshot material;
        material.preset_id = bundle.filament_presets[index];
        if (colors != nullptr && index < colors->values.size())
            material.color = colors->values[index];
        context.materials.emplace_back(std::move(material));
    }

    std::ostringstream model_stream;
    model_stream.imbue(std::locale::classic());
    model_stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    const Model& model = m_plater->model();
    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        ModelObject* object = model.objects[object_index];
        if (object == nullptr)
            continue;

        model_stream << "object:" << object->id().id << ':' << object->timestamp() << ':' << object->name << ':' << object->printable
                     << ":config:\n"
                     << canonical_config(object->config.get());
        for (const auto& [range, range_config] : object->layer_config_ranges)
            model_stream << "layer_range:" << range.first << ':' << range.second << ":config:\n" << canonical_config(range_config.get());
        model_stream << "layer_height_profile:";
        for (const coordf_t value : object->layer_height_profile.get())
            model_stream << value << ',';
        model_stream << '\n';

        size_t instance_count = 0;
        bool outside          = false;
        for (size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
            if (!plate->contain_instance(static_cast<int>(object_index), static_cast<int>(instance_index)))
                continue;
            ++instance_count;
            if (include_diagnostics)
                outside |= plate->check_outside(static_cast<int>(object_index), static_cast<int>(instance_index));
            const ModelInstance* instance = object->instances[instance_index];
            model_stream << "instance:" << instance->id().id << ':' << instance->timestamp() << ':' << instance->printable << ':'
                         << instance->auto_drop << ':' << instance->arrange_order << ':';
            append_matrix(model_stream, instance->get_matrix());
        }
        if (instance_count == 0)
            continue;

        size_t facets     = 0;
        size_t open_edges = 0;
        for (const ModelVolume* volume : object->volumes) {
            if (volume == nullptr)
                continue;
            const size_t volume_facets  = volume->mesh().facets_count();
            const int volume_open_edges = volume->mesh().stats().open_edges;
            model_stream << "volume:" << volume->id().id << ':' << volume->timestamp() << ':' << volume->name << ':'
                         << static_cast<int>(volume->type()) << ':' << volume_facets << ':' << volume_open_edges << ':'
                         << volume->material_id() << ":config:\n"
                         << canonical_config(volume->config.get()) << "annotations:" << volume->supported_facets.timestamp() << ':'
                         << volume->seam_facets.timestamp() << ':' << volume->mmu_segmentation_facets.timestamp() << ':'
                         << volume->fuzzy_skin_facets.timestamp() << ':';
            append_matrix(model_stream, volume->get_matrix());
            model_stream << '\n';
            if (!volume->is_model_part())
                continue;
            facets += volume_facets;
            open_edges += static_cast<size_t>(std::max(volume_open_edges, 0));
        }

        context.objects.push_back({object->id().id, object->name, instance_count, facets, open_edges, outside});
    }

    std::ostringstream plate_stream;
    plate_stream.imbue(std::locale::classic());
    plate_stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    plate_stream << context.plate_index << ':' << plate->id().id << ':' << static_cast<int>(plate->get_bed_type(true)) << ':'
                 << static_cast<int>(plate->get_real_print_seq()) << ':' << static_cast<int>(plate->get_filament_map_mode()) << ':'
                 << plate->is_locked() << ':' << plate->get_spiral_vase_mode();
    for (const int map : plate->get_filament_maps())
        plate_stream << ":filament_map:" << map;
    for (const int extruder : plate->get_first_layer_print_sequence())
        plate_stream << ":first_layer_extruder:" << extruder;
    for (const LayerPrintSequence& layer_sequence : plate->get_other_layers_print_sequence()) {
        plate_stream << ":layer_range:" << layer_sequence.first.first << ':' << layer_sequence.first.second;
        for (const int extruder : layer_sequence.second)
            plate_stream << ':' << extruder;
    }
    if (const auto custom_gcodes = model.plates_custom_gcodes.find(context.plate_index); custom_gcodes != model.plates_custom_gcodes.end()) {
        plate_stream << ":custom_gcode_mode:" << static_cast<int>(custom_gcodes->second.mode);
        for (const CustomGCode::Item& item : custom_gcodes->second.gcodes)
            plate_stream << ":custom_gcode:" << item.print_z << ':' << static_cast<int>(item.type) << ':' << item.extruder << ':'
                         << item.color << ':' << item.extra;
    }

    std::string config_text = canonical_config(full_config);
    config_text.append("printer_preset=")
        .append(context.printer_preset_id)
        .append("\nprocess_preset=")
        .append(context.process_preset_id)
        .append("\n");
    for (const MaterialSnapshot& material : context.materials)
        config_text.append("material_preset=").append(material.preset_id).append("\n");
    context.revision.model_revision  = fnv1a(model_stream.str());
    context.revision.config_revision = fnv1a(config_text);
    context.revision.plate_revision  = fnv1a(plate_stream.str());
    context.revision.fingerprint     = hex_hash(fnv1a(model_stream.str() + config_text + plate_stream.str()));

    if (!include_diagnostics)
        return context;

    PrintBase* print_base = nullptr;
    plate->get_print(&print_base, nullptr, nullptr);
    if (const auto* print = dynamic_cast<const Print*>(print_base);
        plate->is_slice_result_valid() && print != nullptr && !print->objects().empty()) {
        context.native_validation_available = true;
        std::vector<StringObjectException> warnings;
        const StringObjectException error = print->validate(&warnings);
        if (!error.string.empty())
            context.validation_errors.push_back(error.string);
        for (const StringObjectException& warning : warnings)
            if (!warning.string.empty())
                context.validation_warnings.push_back(warning.string);
    }

    return context;
}

} // namespace Slic3r::GUI
