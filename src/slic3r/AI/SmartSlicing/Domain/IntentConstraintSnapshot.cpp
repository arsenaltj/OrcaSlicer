#include "IntentConstraintSnapshot.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace Slic3r::AI::SmartSlicing {
namespace {

uint64_t fnv1a(const std::string& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string canonical_record(const IntentConstraintRecord& record)
{
    std::vector<std::string> parameter_keys = record.parameter_keys;
    std::sort(parameter_keys.begin(), parameter_keys.end());

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10)
           << intent_constraint_type_name(record.type) << ':'
           << intent_constraint_source_name(record.source) << ':' << static_cast<int>(record.state) << ':'
           << record.object_id << ':' << record.instance_id << ':' << record.volume_id << ':'
           << record.source_revision << ':' << record.subtype_code << ':' << record.has_layer_range << ':'
           << record.lower_z << ':' << record.upper_z;
    for (const std::string& key : parameter_keys)
        stream << ":key=" << key;
    return stream.str();
}

} // namespace

const char* intent_constraint_type_name(IntentConstraintType type)
{
    switch (type) {
    case IntentConstraintType::PlatePlacementLock: return "plate_placement_lock";
    case IntentConstraintType::ObjectPlacementLock: return "object_placement_lock";
    case IntentConstraintType::InstancePlacementLock: return "instance_placement_lock";
    case IntentConstraintType::SupportPainting: return "support_painting";
    case IntentConstraintType::SeamPainting: return "seam_painting";
    case IntentConstraintType::MulticolorPainting: return "multicolor_painting";
    case IntentConstraintType::FuzzySkinPainting: return "fuzzy_skin_painting";
    case IntentConstraintType::ModifierVolume: return "modifier_volume";
    case IntentConstraintType::ObjectConfigOverride: return "object_config_override";
    case IntentConstraintType::LayerHeightRange: return "layer_height_range";
    case IntentConstraintType::VariableLayerHeight: return "variable_layer_height";
    }
    return "unknown";
}

const char* intent_constraint_source_name(IntentConstraintSource source)
{
    switch (source) {
    case IntentConstraintSource::OrcaPartPlate: return "orca_part_plate";
    case IntentConstraintSource::UnavailableInOrcaModel: return "unavailable_in_orca_model";
    case IntentConstraintSource::ModelVolumeAnnotation: return "model_volume_annotation";
    case IntentConstraintSource::ModelVolumeType: return "model_volume_type";
    case IntentConstraintSource::ModelObjectConfig: return "model_object_config";
    case IntentConstraintSource::ModelLayerConfigRange: return "model_layer_config_range";
    case IntentConstraintSource::ModelLayerHeightProfile: return "model_layer_height_profile";
    }
    return "unknown";
}

uint64_t intent_constraints_revision(const IntentConstraintSnapshot& snapshot)
{
    std::vector<std::string> records;
    records.reserve(snapshot.records.size());
    for (const IntentConstraintRecord& record : snapshot.records)
        records.push_back(canonical_record(record));
    std::sort(records.begin(), records.end());

    std::vector<std::string> process_keys = snapshot.global_process_parameters.parameter_keys;
    std::sort(process_keys.begin(), process_keys.end());

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "global_process_parameters:optimizable=1:hard_constraint=0";
    for (const std::string& key : process_keys)
        stream << ":key=" << key;
    stream << '\n';
    for (const std::string& record : records)
        stream << record << '\n';
    return fnv1a(stream.str());
}

} // namespace Slic3r::AI::SmartSlicing
