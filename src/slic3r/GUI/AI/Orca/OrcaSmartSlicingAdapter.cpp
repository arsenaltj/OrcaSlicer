#include "OrcaSmartSlicingAdapter.hpp"

#include "OrcaOrientationCandidateProvider.hpp"
#include "OrcaParameterAdvisor.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Jobs/ArrangeJob.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <iomanip>
#include <iterator>
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

Model current_plate_model_copy(const Plater& plater, PartPlate& plate)
{
    Model copied = plater.model();
    for (size_t object_index = copied.objects.size(); object_index > 0; --object_index) {
        const size_t source_object_index = object_index - 1;
        ModelObject* object = copied.objects[source_object_index];
        for (size_t instance_index = object->instances.size(); instance_index > 0; --instance_index) {
            const size_t source_instance_index = instance_index - 1;
            if (!plate.contain_instance(static_cast<int>(source_object_index), static_cast<int>(source_instance_index)))
                object->delete_instance(source_instance_index);
        }
        if (object->instances.empty())
            copied.delete_object(source_object_index);
    }
    return copied;
}

} // namespace

AI::SmartSlicing::WorkspaceRevision OrcaSmartSlicingAdapter::current_revision() const { return capture_context_impl(false).revision; }

AI::SmartSlicing::WorkspaceContext OrcaSmartSlicingAdapter::capture_context() const { return capture_context_impl(true); }

OrcaTrialSliceInput OrcaSmartSlicingAdapter::capture_trial_slice_input() const
{
    if (m_plater == nullptr || wxGetApp().preset_bundle == nullptr)
        throw std::runtime_error("Orca workspace is unavailable.");
    PartPlateList& plates = m_plater->get_partplate_list();
    PartPlate* plate = plates.get_curr_plate();
    if (plate == nullptr)
        throw std::runtime_error("The current plate is unavailable.");

    OrcaTrialSliceInput input;
    input.model                    = current_plate_model_copy(*m_plater, *plate);
    input.config                   = wxGetApp().preset_bundle->full_config();
    input.config.apply(*plate->config(), true);
    input.plate_index              = plates.get_curr_plate_index();
    input.plate_id                 = plate->id().id;
    input.plate_name               = plate->get_plate_name();
    input.extruder_filament_info   = wxGetApp().preset_bundle->get_extruder_filament_info();
    return input;
}

std::vector<AI::SmartSlicing::SliceCandidate>
OrcaSmartSlicingAdapter::candidate_proposals(const AI::SmartSlicing::WorkspaceContext& context) const
{
    if (m_plater == nullptr || wxGetApp().preset_bundle == nullptr)
        return {};
    PartPlateList& plates = m_plater->get_partplate_list();
    PartPlate* plate = plates.get_curr_plate();
    if (plate == nullptr)
        return {};
    const AI::SmartSlicing::WorkspaceRevision& revision = context.revision;

    OrcaPlacementCandidateInput input;
    input.model          = current_plate_model_copy(*m_plater, *plate);
    input.config         = wxGetApp().preset_bundle->full_config();
    input.arrange_params = init_arrange_params(m_plater);
    input.plate_locked   = plate->is_locked();
    const bool enable_wrapping = input.config.opt_bool("enable_wrapping_detection");
    plates.preprocess_exclude_areas(input.arrange_params.excluded_regions, enable_wrapping, 1, scale_(1));
    if (const auto wipe_tower = get_wipe_tower_arrangepoly(*m_plater))
        input.fixed_regions.push_back(*wipe_tower);
    std::vector<AI::SmartSlicing::SliceCandidate> candidates =
        OrcaPlacementCandidateProvider().generate(std::move(input), revision);

    OrcaOrientationCandidateInput orientation_input;
    orientation_input.model        = current_plate_model_copy(*m_plater, *plate);
    orientation_input.config       = wxGetApp().preset_bundle->full_config();
    orientation_input.plate_locked = plate->is_locked();
    std::vector<AI::SmartSlicing::SliceCandidate> orientation_candidates =
        OrcaOrientationCandidateProvider().generate(std::move(orientation_input), revision);
    candidates.insert(candidates.end(), std::make_move_iterator(orientation_candidates.begin()),
                      std::make_move_iterator(orientation_candidates.end()));

    DynamicPrintConfig current_config = wxGetApp().preset_bundle->full_config();
    current_config.apply(*plate->config(), true);
    OrcaParameterAdvisorInput advisor_input;
    advisor_input.plate_id = static_cast<int64_t>(plate->id().id);
    advisor_input.current_brim_width = current_config.opt_float("brim_width");
    const Model& model = m_plater->model();
    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const ModelObject* object = model.objects[object_index];
        if (object == nullptr)
            continue;
        for (size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
            const ModelInstance* instance = object->instances[instance_index];
            if (instance == nullptr || !instance->printable ||
                !plate->contain_instance(static_cast<int>(object_index), static_cast<int>(instance_index)))
                continue;
            const Vec3d size = object->instance_bounding_box(*instance).size();
            advisor_input.printable_instances.push_back({size.x(), size.y(), size.z()});
        }
    }
    AI::SmartSlicing::ParameterProposal parameter_proposal =
        OrcaParameterAdvisor(std::move(advisor_input)).advise(context);
    if (!parameter_proposal.entries.empty()) {
        AI::SmartSlicing::SliceCandidate candidate;
        candidate.id            = "parameter-brim-stability-v1";
        candidate.base_revision = revision;
        candidate.goal          = AI::SmartSlicing::CandidateGoal::Stability;
        candidate.explanation   = "small_or_slender_footprint_brim_candidate";
        candidate.parameters    = std::move(parameter_proposal);
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

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
    const auto* types = full_config.option<ConfigOptionStrings>("filament_type");
    const auto* temperatures = full_config.option<ConfigOptionInts>("nozzle_temperature");
    const auto* temperature_lows = full_config.option<ConfigOptionInts>("nozzle_temperature_range_low");
    const auto* temperature_highs = full_config.option<ConfigOptionInts>("nozzle_temperature_range_high");
    context.multicolor.used_logical_filament_ids = plate->get_extruders(true);
    context.multicolor.filament_to_physical_slot = plate->get_real_filament_maps(bundle.project_config);
    context.multicolor.first_layer_tool_sequence = plate->get_first_layer_print_sequence();
    for (const LayerPrintSequence& sequence : plate->get_other_layers_print_sequence())
        context.multicolor.other_layer_tool_sequences.push_back(
            {sequence.first.first, sequence.first.second, sequence.second});
    if (const auto* prime_tower = full_config.option<ConfigOptionBool>("enable_prime_tower"))
        context.multicolor.prime_tower_enabled = prime_tower->value;
    context.multicolor.flush_matrix_available = full_config.option("flush_volumes_matrix") != nullptr;
    context.multicolor.flush_multiplier_available = full_config.option("flush_multiplier") != nullptr;

    context.materials.reserve(bundle.filament_presets.size());
    for (size_t index = 0; index < bundle.filament_presets.size(); ++index) {
        MaterialSnapshot material;
        material.preset_id = bundle.filament_presets[index];
        material.logical_filament_id = static_cast<int>(index + 1);
        if (colors != nullptr && index < colors->values.size())
            material.color = colors->values[index];
        if (types != nullptr && index < types->values.size())
            material.filament_type = types->get_at(index);
        if (temperatures != nullptr && index < temperatures->values.size())
            material.nozzle_temperature = temperatures->get_at(index);
        if (temperature_lows != nullptr && index < temperature_lows->values.size())
            material.nozzle_temperature_range_low = temperature_lows->get_at(index);
        if (temperature_highs != nullptr && index < temperature_highs->values.size())
            material.nozzle_temperature_range_high = temperature_highs->get_at(index);
        if (index < context.multicolor.filament_to_physical_slot.size())
            material.physical_slot_id = context.multicolor.filament_to_physical_slot[index];
        material.used_on_plate = std::find(context.multicolor.used_logical_filament_ids.begin(),
                                           context.multicolor.used_logical_filament_ids.end(),
                                           material.logical_filament_id) != context.multicolor.used_logical_filament_ids.end();
        context.materials.emplace_back(std::move(material));
    }

    if (context.multicolor.used_logical_filament_ids.size() >= 2) {
        std::vector<std::string> used_types;
        std::vector<int> used_temperatures;
        std::vector<int> used_lows;
        std::vector<int> used_highs;
        bool complete = true;
        for (const int filament_id : context.multicolor.used_logical_filament_ids) {
            const size_t index = filament_id > 0 ? static_cast<size_t>(filament_id - 1) : context.materials.size();
            if (index >= context.materials.size() || context.materials[index].filament_type.empty()) {
                complete = false;
                break;
            }
            const MaterialSnapshot& material = context.materials[index];
            used_types.push_back(material.filament_type);
            used_temperatures.push_back(material.nozzle_temperature);
            used_lows.push_back(material.nozzle_temperature_range_low);
            used_highs.push_back(material.nozzle_temperature_range_high);
        }
        if (!complete) {
            context.multicolor.physical_slot_compatibility = PhysicalSlotCompatibility::Unavailable;
        } else {
            const FilamentCompatibilityType compatibility = Print::check_multi_filaments_compatibility(
                used_types, used_temperatures, used_lows, used_highs);
            context.multicolor.physical_slot_compatibility =
                compatibility == FilamentCompatibilityType::Compatible ? PhysicalSlotCompatibility::Compatible :
                compatibility == FilamentCompatibilityType::InvalidTemperatureRange ?
                    PhysicalSlotCompatibility::InvalidTemperatureRange : PhysicalSlotCompatibility::Incompatible;
        }
        const size_t physical_slot_count = context.nozzle_diameters.size();
        for (const int filament_id : context.multicolor.used_logical_filament_ids) {
            const size_t index = filament_id > 0 ? static_cast<size_t>(filament_id - 1) :
                                                   context.multicolor.filament_to_physical_slot.size();
            if (index >= context.multicolor.filament_to_physical_slot.size() || physical_slot_count == 0 ||
                context.multicolor.filament_to_physical_slot[index] <= 0 ||
                static_cast<size_t>(context.multicolor.filament_to_physical_slot[index]) > physical_slot_count) {
                context.multicolor.color_mapping_degraded = true;
                break;
            }
        }
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
    plate_stream << ":config:\n" << canonical_config(*plate->config());
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
