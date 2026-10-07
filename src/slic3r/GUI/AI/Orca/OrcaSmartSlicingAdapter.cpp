#include "OrcaSmartSlicingAdapter.hpp"
#include "OrcaPlateRevisionConfig.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Jobs/ArrangeJob.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"

#include <wx/thread.h>
#include <algorithm>
#include <array>
#include <boost/filesystem/fstream.hpp>
#include <openssl/evp.h>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
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

std::array<double, 16> matrix_values(const Transform3d& matrix)
{
    std::array<double, 16> values{};
    for (Eigen::Index row = 0; row < matrix.rows(); ++row)
        for (Eigen::Index column = 0; column < matrix.cols(); ++column)
            values[static_cast<size_t>(row * matrix.cols() + column)] = matrix(row, column);
    return values;
}

struct EvpContextDeleter
{
    void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
};

std::string sha256_file(const std::string& path)
{
    boost::filesystem::ifstream stream(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, EvpContextDeleter> context(EVP_MD_CTX_new());
    if (!stream || !context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        return {};
    std::array<char, 64 * 1024> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if (count > 0 && EVP_DigestUpdate(context.get(), buffer.data(), static_cast<size_t>(count)) != 1)
            return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (!stream.eof() || EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1 || length != 32)
        return {};
    static constexpr char HEX[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (unsigned int index = 0; index < length; ++index) {
        result[index * 2] = HEX[digest[index] >> 4];
        result[index * 2 + 1] = HEX[digest[index] & 0x0f];
    }
    return result;
}

ProfileIdentity profile_identity(const Preset& preset)
{
    ProfileIdentity identity;
    identity.setting_id = preset.setting_id;
    if (!preset.inherits().empty())
        identity.inheritance_chain.push_back(preset.inherits());
    if (!preset.is_dirty)
        identity.fingerprint = sha256_file(preset.file);
    return identity;
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

bool OrcaSmartSlicingAdapter::set_usage_purpose(AI::SmartSlicing::UsagePurpose purpose)
{
    using AI::SmartSlicing::UsagePurpose;
    switch (purpose) {
    case UsagePurpose::Decoration:
    case UsagePurpose::General:
    case UsagePurpose::Functional:
        m_usage_purpose = purpose;
        return true;
    case UsagePurpose::Unknown:
    default:
        m_usage_purpose = UsagePurpose::General;
        return false;
    }
}

std::optional<AI::SmartSlicing::ProtectedRegionBindingTarget>
OrcaSmartSlicingAdapter::current_region_target(uint64_t object_id, uint64_t volume_id) const
{
    using namespace AI::SmartSlicing;
    if (!wxIsMainThread() || m_plater == nullptr || object_id == 0 || volume_id == 0)
        return std::nullopt;
    if (m_plater->get_partplate_list().get_curr_plate() == nullptr)
        return std::nullopt;
    const Model& model = m_plater->model();
    for (const ModelObject* object : model.objects) {
        if (object == nullptr || object->id().id != object_id)
            continue;
        for (const ModelVolume* volume : object->volumes) {
            if (volume == nullptr || !volume->is_model_part() || volume->id().id != volume_id)
                continue;
            const indexed_triangle_set& mesh = volume->mesh().its;
            const std::string fingerprint = AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh);
            if (mesh.indices.empty() || fingerprint.empty())
                return std::nullopt;
            try {
                return ProtectedRegionBindingTarget{current_revision(), object_id, volume_id,
                                                   fingerprint, mesh.indices.size(), {}};
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

AI::SmartSlicing::ProtectedRegionBindingResult OrcaSmartSlicingAdapter::mark_user_marked_region(
    uint64_t object_id, uint64_t volume_id, std::vector<AI::ProtectedFacetRange> ranges)
{
    using namespace AI::SmartSlicing;
    if (!wxIsMainThread()) {
        ProtectedRegionBindingResult result;
        result.status = ProtectedRegionBindingStatus::Rejected;
        result.rejection_code = ProtectedRegionRejectionCode::InvalidBindingTarget;
        return result;
    }
    const auto target = current_region_target(object_id, volume_id);
    if (!target) {
        ProtectedRegionBindingResult result;
        result.status = ProtectedRegionBindingStatus::Rejected;
        result.rejection_code = ProtectedRegionRejectionCode::InvalidBindingTarget;
        return result;
    }
    return m_user_marked_regions.replace(*target, std::move(ranges));
}

bool OrcaSmartSlicingAdapter::clear_user_marked_region(uint64_t object_id, uint64_t volume_id)
{
    if (!wxIsMainThread())
        return false;
    return m_user_marked_regions.clear(object_id, volume_id);
}

AI::SmartSlicing::ProtectedRegionSourceResult OrcaSmartSlicingAdapter::query_user_marked_region(
    uint64_t object_id, uint64_t volume_id) const
{
    using namespace AI::SmartSlicing;
    const auto target = current_region_target(object_id, volume_id);
    if (!target)
        return {ProtectedRegionSourceStatus::Rejected, {}, "user_marked_region_target_unavailable"};
    return m_user_marked_regions.regions_for(
        {target->workspace_revision, object_id, volume_id, target->geometry_fingerprint, target->facet_count});
}

OrcaCandidateSearchCaptureResult OrcaSmartSlicingAdapter::capture_candidate_search_input() const
{
    using namespace AI::SmartSlicing;
    if (m_plater == nullptr || wxGetApp().preset_bundle == nullptr)
        return {OrcaCandidateSearchCaptureStatus::InvalidInput,
                "candidate_capture_workspace_unavailable", std::nullopt};
    PartPlateList& plates = m_plater->get_partplate_list();
    PartPlate* plate = plates.get_curr_plate();
    if (plate == nullptr)
        return {OrcaCandidateSearchCaptureStatus::InvalidInput,
                "candidate_capture_current_plate_unavailable", std::nullopt};

    PresetBundle& bundle = *wxGetApp().preset_bundle;
    const Preset& process_preset = bundle.prints.get_edited_preset();
    OrcaCandidateSearchCaptureSource source;
    source.context = capture_context_impl(false);
    source.plate_id = static_cast<int64_t>(plate->id().id);
    source.plate_locked = plate->is_locked();
    source.usage_purpose = m_usage_purpose;
    source.process_profile.dirty = process_preset.is_dirty;
    source.process_profile.identity = profile_identity(process_preset);
    source.process_profile.process_config = process_preset.config;
    source.process_profile.effective_config = bundle.full_config();
    source.process_profile.effective_config.apply(*plate->config(), true);

    const Model& model = m_plater->model();
    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const ModelObject* object = model.objects[object_index];
        if (object == nullptr)
            continue;
        for (size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
            if (!plate->contain_instance(static_cast<int>(object_index),
                                         static_cast<int>(instance_index)))
                continue;
            const ModelInstance* instance = object->instances[instance_index];
            if (instance == nullptr)
                continue;
            OrcaCandidateSearchObjectSource captured;
            captured.object_id = object->id().id;
            captured.instance_id = instance->id().id;
            captured.current_transform = matrix_values(instance->get_matrix());
            captured.locked = plate->is_locked();
            for (const ModelVolume* volume : object->volumes) {
                if (volume == nullptr || !volume->is_model_part())
                    continue;
                const indexed_triangle_set& mesh = volume->mesh().its;
                const std::string fingerprint = AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh);
                if (mesh.indices.empty() || fingerprint.empty())
                    continue;
                const auto marked = m_user_marked_regions.regions_for(
                    {source.context.revision, captured.object_id, volume->id().id, fingerprint,
                     mesh.indices.size()});
                if (marked.status == ProtectedRegionSourceStatus::Rejected) {
                    captured.protected_region_status = ProtectedRegionBindingStatus::Rejected;
                    continue;
                }
                if (marked.status != ProtectedRegionSourceStatus::Available)
                    continue;
                captured.protected_region_status = ProtectedRegionBindingStatus::Accepted;
                for (const AI::ProtectedRegionManifest& manifest : marked.manifests)
                    captured.protected_regions.push_back(
                        {manifest.object_id(), manifest.volume_id(), manifest.geometry_fingerprint(),
                         manifest.facet_count(), manifest.kind(), manifest.source(), manifest.source_version(),
                         manifest.facet_ranges(), manifest.confidence()});
            }
            source.objects.push_back(std::move(captured));
        }
    }
    return OrcaCandidateSearchAdapter::capture(source);
}

OrcaModelFeatureCaptureResult OrcaSmartSlicingAdapter::capture_model_feature_inputs(
    const OrcaModelFeatureTarget& target,
    const AI::SmartSlicing::ModelFeatureAnalysisLimits& limits) const
{
    if (m_plater == nullptr) {
        OrcaModelFeatureCaptureResult result;
        result.status = OrcaModelFeatureCaptureStatus::InvalidInput;
        result.diagnostic_code = "model_feature_capture_plater_unavailable";
        return result;
    }
    return OrcaModelFeatureAnalyzer::capture_current_plate(*m_plater, target, limits);
}

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
    const AI::SmartSlicing::WorkspaceContext context = capture_context_impl(false);
    input.intent_constraints       = context.intent_constraints;
    input.evidence_revision        = context.revision;
    if (!context.material_compatibility.registry_version.empty() &&
        !context.material_compatibility.materials.empty()) {
        input.materials_compatible = AI::SmartSlicing::MetricValue<bool>::known(
            context.material_compatibility.compatible(), {"orca_material_compatibility_registry"});
    }
    switch (context.multicolor.physical_slot_compatibility) {
    case AI::SmartSlicing::PhysicalSlotCompatibility::NotApplicable:
        input.physical_slots_compatible =
            AI::SmartSlicing::MetricValue<bool>::not_applicable({"single_material"});
        break;
    case AI::SmartSlicing::PhysicalSlotCompatibility::Compatible:
        input.physical_slots_compatible =
            AI::SmartSlicing::MetricValue<bool>::known(true, {"orca_physical_slot_compatibility"});
        break;
    case AI::SmartSlicing::PhysicalSlotCompatibility::Incompatible:
    case AI::SmartSlicing::PhysicalSlotCompatibility::InvalidTemperatureRange:
        input.physical_slots_compatible =
            AI::SmartSlicing::MetricValue<bool>::known(false, {"orca_physical_slot_compatibility"});
        break;
    case AI::SmartSlicing::PhysicalSlotCompatibility::Unavailable:
        input.physical_slots_compatible =
            AI::SmartSlicing::MetricValue<bool>::unknown({"physical_slot_evidence_unavailable"});
        break;
    }
    if (context.multicolor.used_logical_filament_ids.size() <= 1) {
        input.color_mapping_degraded =
            AI::SmartSlicing::MetricValue<bool>::not_applicable({"single_material"});
    } else {
        input.color_mapping_degraded = AI::SmartSlicing::MetricValue<bool>::known(
            context.multicolor.color_mapping_degraded, {"orca_workspace_color_mapping"});
    }
    return input;
}

std::vector<AI::SmartSlicing::SliceCandidate>
OrcaSmartSlicingAdapter::candidate_proposals(const AI::SmartSlicing::WorkspaceRevision& revision) const
{
    if (m_plater == nullptr || wxGetApp().preset_bundle == nullptr)
        return {};
    PartPlateList& plates = m_plater->get_partplate_list();
    PartPlate* plate = plates.get_curr_plate();
    if (plate == nullptr)
        return {};

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

    DynamicPrintConfig current_config = wxGetApp().preset_bundle->full_config();
    current_config.apply(*plate->config(), true);
    const double current_brim_width = current_config.opt_float("brim_width");
    bool benefits_from_brim = false;
    const Model& model = m_plater->model();
    for (size_t object_index = 0; object_index < model.objects.size() && !benefits_from_brim; ++object_index) {
        const ModelObject* object = model.objects[object_index];
        if (object == nullptr)
            continue;
        for (size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
            const ModelInstance* instance = object->instances[instance_index];
            if (instance == nullptr || !instance->printable ||
                !plate->contain_instance(static_cast<int>(object_index), static_cast<int>(instance_index)))
                continue;
            const Vec3d size = object->instance_bounding_box(*instance).size();
            const double minimum_footprint = std::min(size.x(), size.y());
            benefits_from_brim = minimum_footprint > 0.0 &&
                (minimum_footprint <= 8.0 || size.z() >= 2.0 * minimum_footprint);
            if (benefits_from_brim)
                break;
        }
    }
    if (benefits_from_brim && current_brim_width < 10.0) {
        const double proposed_brim_width = std::min(10.0, std::max(5.0, current_brim_width + 2.0));
        AI::SmartSlicing::SliceCandidate candidate;
        candidate.id            = "parameter-brim-stability-v1";
        candidate.base_revision = revision;
        candidate.goal          = AI::SmartSlicing::CandidateGoal::Stability;
        candidate.explanation   = "small_or_slender_footprint_brim_candidate";
        candidate.parameters.entries.push_back({AI::SmartSlicing::ConfigScope::Plate,
                                                AI::SmartSlicing::PresetOwner::Process,
                                                static_cast<int64_t>(plate->id().id),
                                                "brim_width",
                                                current_brim_width,
                                                proposed_brim_width,
                                                "improve_small_footprint_adhesion"});
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
    context.intent_constraints.global_process_parameters.parameter_keys =
        bundle.prints.get_edited_preset().config.keys();
    std::sort(context.intent_constraints.global_process_parameters.parameter_keys.begin(),
              context.intent_constraints.global_process_parameters.parameter_keys.end());
    context.intent_constraints.records.push_back({
        IntentConstraintType::PlatePlacementLock,
        IntentConstraintSource::OrcaPartPlate,
        plate->is_locked() ? IntentConstraintState::Active : IntentConstraintState::Inactive,
    });
    if (const auto* nozzles = full_config.option<ConfigOptionFloats>("nozzle_diameter"))
        context.nozzle_diameters = nozzles->values;

    MachineCapabilityEvidence machine_evidence;
    machine_evidence.nozzles.reserve(context.nozzle_diameters.size());
    for (size_t index = 0; index < context.nozzle_diameters.size(); ++index) {
        NozzleCapabilitySnapshot nozzle;
        nozzle.physical_number = static_cast<int>(index + 1);
        nozzle.diameter_mm = context.nozzle_diameters[index];
        machine_evidence.nozzles.push_back(std::move(nozzle));
    }
    context.machine_capability = evaluate_machine_capability(
        profile_identity(bundle.printers.get_edited_preset()), std::move(machine_evidence));

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

    std::vector<ProfileIdentity> used_material_profiles;
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
        if (material.used_on_plate) {
            const Preset* preset = bundle.filaments.find_preset(bundle.filament_presets[index]);
            used_material_profiles.push_back(preset == nullptr ? ProfileIdentity{} : profile_identity(*preset));
        }
        context.materials.emplace_back(std::move(material));
    }
    context.material_compatibility = evaluate_material_compatibility(std::move(used_material_profiles));

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
            IntentConstraintRecord instance_lock;
            instance_lock.type = IntentConstraintType::InstancePlacementLock;
            instance_lock.source = IntentConstraintSource::UnavailableInOrcaModel;
            instance_lock.state = IntentConstraintState::Unknown;
            instance_lock.object_id = object->id().id;
            instance_lock.instance_id = instance->id().id;
            context.intent_constraints.records.push_back(std::move(instance_lock));
            model_stream << "instance:" << instance->id().id << ':' << instance->timestamp() << ':' << instance->printable << ':'
                         << instance->auto_drop << ':' << instance->arrange_order << ':';
            append_matrix(model_stream, instance->get_matrix());
        }
        if (instance_count == 0)
            continue;

        IntentConstraintRecord object_lock;
        object_lock.type = IntentConstraintType::ObjectPlacementLock;
        object_lock.source = IntentConstraintSource::UnavailableInOrcaModel;
        object_lock.state = IntentConstraintState::Unknown;
        object_lock.object_id = object->id().id;
        context.intent_constraints.records.push_back(std::move(object_lock));

        IntentConstraintRecord object_config;
        object_config.type = IntentConstraintType::ObjectConfigOverride;
        object_config.source = IntentConstraintSource::ModelObjectConfig;
        object_config.state = object->config.empty() ? IntentConstraintState::Inactive : IntentConstraintState::Active;
        object_config.object_id = object->id().id;
        object_config.source_revision = fnv1a(canonical_config(object->config.get()));
        object_config.parameter_keys = object->config.keys();
        context.intent_constraints.records.push_back(std::move(object_config));

        for (const auto& [range, range_config] : object->layer_config_ranges) {
            IntentConstraintRecord layer_range;
            layer_range.type = IntentConstraintType::LayerHeightRange;
            layer_range.source = IntentConstraintSource::ModelLayerConfigRange;
            layer_range.state = IntentConstraintState::Active;
            layer_range.object_id = object->id().id;
            layer_range.source_revision = fnv1a(canonical_config(range_config.get()));
            layer_range.has_layer_range = true;
            layer_range.lower_z = range.first;
            layer_range.upper_z = range.second;
            layer_range.parameter_keys = range_config.keys();
            context.intent_constraints.records.push_back(std::move(layer_range));
        }

        IntentConstraintRecord variable_layers;
        variable_layers.type = IntentConstraintType::VariableLayerHeight;
        variable_layers.source = IntentConstraintSource::ModelLayerHeightProfile;
        variable_layers.state = object->layer_height_profile.empty() ? IntentConstraintState::Inactive :
                                                                       IntentConstraintState::Active;
        variable_layers.object_id = object->id().id;
        variable_layers.source_revision = object->layer_height_profile.timestamp();
        context.intent_constraints.records.push_back(std::move(variable_layers));

        size_t facets     = 0;
        size_t open_edges = 0;
        for (const ModelVolume* volume : object->volumes) {
            if (volume == nullptr)
                continue;
            const size_t volume_facets  = volume->mesh().facets_count();
            const int volume_open_edges = volume->mesh().stats().open_edges;
            if (!volume->is_model_part()) {
                IntentConstraintRecord modifier;
                modifier.type = IntentConstraintType::ModifierVolume;
                modifier.source = IntentConstraintSource::ModelVolumeType;
                modifier.state = IntentConstraintState::Active;
                modifier.object_id = object->id().id;
                modifier.volume_id = volume->id().id;
                modifier.source_revision = volume->timestamp();
                modifier.subtype_code = static_cast<int>(volume->type());
                modifier.parameter_keys = volume->config.keys();
                context.intent_constraints.records.push_back(std::move(modifier));
            }
            const auto add_annotation = [&](IntentConstraintType type, const FacetsAnnotation& annotation) {
                IntentConstraintRecord record;
                record.type = type;
                record.source = IntentConstraintSource::ModelVolumeAnnotation;
                record.state = annotation.empty() ? IntentConstraintState::Inactive : IntentConstraintState::Active;
                record.object_id = object->id().id;
                record.volume_id = volume->id().id;
                record.source_revision = annotation.timestamp();
                context.intent_constraints.records.push_back(std::move(record));
            };
            add_annotation(IntentConstraintType::SupportPainting, volume->supported_facets);
            add_annotation(IntentConstraintType::SeamPainting, volume->seam_facets);
            add_annotation(IntentConstraintType::MulticolorPainting, volume->mmu_segmentation_facets);
            add_annotation(IntentConstraintType::FuzzySkinPainting, volume->fuzzy_skin_facets);
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
    model_stream << "intent_constraints_revision:" << intent_constraints_revision(context.intent_constraints) << '\n';
    model_stream << "capability_registry_revision:"
                 << capability_registry_revision_token(context.machine_capability, context.material_compatibility) << '\n';
    model_stream << "machine_profile:" << context.machine_capability.profile.setting_id << ':'
                 << context.machine_capability.profile.fingerprint << ':'
                 << static_cast<int>(context.machine_capability.support_status) << '\n';
    for (const std::string& parent : context.machine_capability.profile.inheritance_chain)
        model_stream << "machine_parent:" << parent << '\n';
    for (const MachineCapabilityReason reason : context.machine_capability.reasons)
        model_stream << "machine_reason:" << machine_capability_reason_name(reason) << '\n';
    for (const MaterialCapabilitySnapshot& material : context.material_compatibility.materials) {
        model_stream << "material_profile:" << material.profile.setting_id << ':' << material.profile.fingerprint << ':'
                     << static_cast<int>(material.family) << ':' << static_cast<int>(material.support_status) << '\n';
        for (const std::string& parent : material.profile.inheritance_chain)
            model_stream << "material_parent:" << parent << '\n';
    }

    std::ostringstream plate_stream;
    plate_stream.imbue(std::locale::classic());
    plate_stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    plate_stream << context.plate_index << ':' << plate->id().id << ':' << static_cast<int>(plate->get_bed_type(true)) << ':'
                 << static_cast<int>(plate->get_real_print_seq()) << ':' << static_cast<int>(plate->get_filament_map_mode()) << ':'
                 << plate->is_locked() << ':' << plate->get_spiral_vase_mode();
    plate_stream << ":config:\n" << canonical_config(slice_input_plate_config(
        *plate->config(), plate->get_real_filament_map_mode(bundle.project_config)));
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
