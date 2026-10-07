#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/CandidateSearchPipeline.hpp"

#include <algorithm>
#include <cmath>
#include <set>

using namespace Slic3r::AI::SmartSlicing;

namespace {

std::array<double, 16> transform(double marker = 0.0)
{
    std::array<double, 16> value{};
    value[0] = value[5] = value[10] = value[15] = 1.0;
    value[12] = marker;
    return value;
}

ModelFeatureSnapshot safe_features(uint64_t object_id)
{
    ModelFeatureSnapshot value;
    value.object_id = object_id;
    value.volume_id = object_id * 10;
    value.small_text_candidates = FeatureValue<std::vector<SmallTextCandidate>>::known({});
    value.thin_wall_candidates = FeatureValue<std::vector<ThinWallCandidate>>::known({});
    value.overhangs = FeatureValue<OverhangSummary>::known({});
    value.orientation_sensitive_surfaces = FeatureValue<SurfaceCandidateSummary>::known({});
    return value;
}

OrientationSearchOption option(std::string id, OrientationStrategy strategy, double marker,
                               double cost = 0.0)
{
    return {std::move(id), strategy, transform(marker), FeatureAvailability::Known,
            "fixture_geometry", "fixture-orientation/v1", cost};
}

CandidateSearchObjectInput object(uint64_t object_id, size_t option_count = 5)
{
    CandidateSearchObjectInput value;
    value.object_id = object_id;
    value.instance_id = object_id + 100;
    value.current_transform = transform(static_cast<double>(object_id));
    value.model_features.push_back(safe_features(object_id));
    value.protected_region_status = ProtectedRegionBindingStatus::Accepted;
    const std::array<OrientationStrategy, 3> strategies{
        OrientationStrategy::StablePlane,
        OrientationStrategy::LowSupport,
        OrientationStrategy::StablePlane,
    };
    for (size_t index = 0; index < option_count; ++index)
        value.orientation_options.push_back(option("orientation-" + std::to_string(index),
            strategies[index % strategies.size()], static_cast<double>(object_id * 10 + index + 1),
            static_cast<double>(index) / 10.0));
    return value;
}

ProfileParameterValue profile_value(ConfigValue value, bool valid = true)
{
    return {std::move(value), valid, "fixture_profile", "fixture-profile/v1"};
}

CandidateSearchParameterInput parameter(std::string key, ConfigValue current,
                                        std::vector<ProfileParameterValue> options)
{
    CandidateSearchParameterInput value;
    value.scope = ConfigScope::Plate;
    value.owner = PresetOwner::Process;
    value.target_id = 7;
    value.key = std::move(key);
    value.current_value = std::move(current);
    value.current_source_code = "fixture_process";
    value.current_source_version = "fixture-process/v1";
    value.options = std::move(options);
    return value;
}

CandidateSearchInput fixture(size_t object_count = 2)
{
    CandidateSearchInput input;
    input.workspace_revision = {1, 2, 3, "candidate-search-fixture"};
    input.usage_purpose = UsagePurpose::General;
    input.may_increase_process_speed = true;
    for (size_t index = 0; index < object_count; ++index)
        input.objects.push_back(object(index + 1));

    auto layer = parameter("layer_height", 0.20,
        {profile_value(0.12), profile_value(0.16), profile_value(0.20),
         profile_value(0.24), profile_value(0.28)});
    layer.bounds.push_back({ConfigScope::Plate, PresetOwner::Process, 7, "layer_height",
                            BoundEvidenceSource::ProcessProfile, true, 0.12, 0.28, {},
                            "fixture-process/v1"});
    input.profile_parameters.push_back(std::move(layer));
    input.profile_parameters.push_back(parameter("wall_loops", int64_t{2},
        {profile_value(int64_t{2}), profile_value(int64_t{3})}));
    input.profile_parameters.push_back(parameter("sparse_infill_density", 20.0,
        {profile_value(10.0), profile_value(12.0), profile_value(15.0), profile_value(20.0)}));
    input.profile_parameters.push_back(parameter("top_shell_layers", int64_t{5},
        {profile_value(int64_t{3}), profile_value(int64_t{4}), profile_value(int64_t{5}),
         profile_value(int64_t{6}), profile_value(int64_t{7}), profile_value(int64_t{8}),
         profile_value(int64_t{9}), profile_value(int64_t{10})}));
    input.profile_parameters.push_back(parameter("bottom_shell_layers", int64_t{4},
        {profile_value(int64_t{3}), profile_value(int64_t{4}), profile_value(int64_t{5}),
         profile_value(int64_t{6}), profile_value(int64_t{7}), profile_value(int64_t{8})}));
    input.profile_parameters.push_back(parameter("enable_support", false,
        {profile_value(false), profile_value(true)}));
    input.profile_parameters.push_back(parameter("support_interface_top_layers", int64_t{0},
        {profile_value(int64_t{0}), profile_value(int64_t{2}), profile_value(int64_t{3}),
         profile_value(int64_t{4})}));
    auto speed = parameter("outer_wall_speed", 50.0,
        {profile_value(50.0), profile_value(60.0), profile_value(80.0)});
    for (const BoundEvidenceSource source : {BoundEvidenceSource::MachineProfile,
                                             BoundEvidenceSource::ProcessProfile,
                                             BoundEvidenceSource::MaterialProfile})
        speed.bounds.push_back({ConfigScope::Plate, PresetOwner::Process, 7, "outer_wall_speed",
                                source, true, 1.0, 100.0, {}, "fixture-speed-bounds/v1"});
    input.profile_parameters.push_back(std::move(speed));
    input.native_validator = [](const ParameterProposal&) {
        return NativeParameterValidationResult{true, {}};
    };
    return input;
}

const ConfigPatchEntry* patch(const CandidateSearchDraft& draft, const std::string& key)
{
    const auto found = std::find_if(draft.parameters.entries.begin(), draft.parameters.entries.end(),
        [&](const ConfigPatchEntry& entry) { return entry.key == key; });
    return found == draft.parameters.entries.end() ? nullptr : &*found;
}

std::vector<CandidateId> ids(const GoalCandidateDrafts& goal)
{
    std::vector<CandidateId> result;
    for (const CandidateSearchDraft& draft : goal.drafts)
        result.push_back(draft.candidate_id);
    return result;
}

bool same_placement(const PlacementCandidate& lhs, const PlacementCandidate& rhs)
{
    if (lhs.transforms.size() != rhs.transforms.size())
        return false;
    for (size_t index = 0; index < lhs.transforms.size(); ++index) {
        if (lhs.transforms[index].object_id != rhs.transforms[index].object_id ||
            lhs.transforms[index].instance_id != rhs.transforms[index].instance_id ||
            lhs.transforms[index].matrix != rhs.transforms[index].matrix)
            return false;
    }
    return true;
}

bool same_proposal(const ParameterProposal& lhs, const ParameterProposal& rhs)
{
    if (lhs.goal != rhs.goal || lhs.policy_version != rhs.policy_version ||
        lhs.entries.size() != rhs.entries.size())
        return false;
    for (size_t index = 0; index < lhs.entries.size(); ++index) {
        const ConfigPatchEntry& left = lhs.entries[index];
        const ConfigPatchEntry& right = rhs.entries[index];
        if (left.scope != right.scope || left.owner != right.owner ||
            left.target_id != right.target_id || left.key != right.key ||
            left.expected_value != right.expected_value || left.new_value != right.new_value ||
            left.reason_code != right.reason_code)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("formal goal contracts freeze three goals thresholds weights and search budgets",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CHECK(RECOMMENDATION_GOALS == std::array{RecommendationGoal::Balanced,
                                             RecommendationGoal::Speed,
                                             RecommendationGoal::Quality});
    const GoalContract& balanced = goal_contract(RecommendationGoal::Balanced);
    CHECK(balanced.version == GOAL_CONTRACT_VERSION);
    CHECK(balanced.minimum_strength_ratio == Catch::Approx(0.95));
    CHECK(balanced.maximum_time_ratio == Catch::Approx(1.15));
    CHECK(balanced.maximum_material_ratio == Catch::Approx(1.10));
    CHECK(balanced.critical_surface_maximum_time_ratio == Catch::Approx(1.25));
    CHECK(balanced.critical_surface_maximum_material_ratio == Catch::Approx(1.15));
    CHECK(balanced.critical_surface_relaxation_explanation_code ==
          "balanced_critical_surface_resource_relaxation");
    CHECK(balanced.weights.quality == Catch::Approx(0.45));
    CHECK(balanced.weights.reliability == Catch::Approx(0.25));
    CHECK(balanced.weights.time == Catch::Approx(0.20));
    CHECK(balanced.weights.material_and_multicolor_waste == Catch::Approx(0.10));

    const GoalContract& speed = goal_contract(RecommendationGoal::Speed);
    CHECK(speed.minimum_time_reduction_ratio == Catch::Approx(0.10));
    CHECK(speed.general_minimum_wall_loops == 2);
    CHECK(speed.general_minimum_infill_percent == Catch::Approx(15.0));
    CHECK(speed.decoration_minimum_infill_percent == Catch::Approx(10.0));
    CHECK(speed.decoration_maximum_infill_percent == Catch::Approx(12.0));
    const GoalContract& quality = goal_contract(RecommendationGoal::Quality);
    CHECK(quality.maximum_time_ratio == Catch::Approx(2.0));
    CHECK(quality.maximum_material_ratio == Catch::Approx(1.2));
    CHECK(quality.weights.quality == Catch::Approx(0.0));
    CHECK(quality.weights.reliability == Catch::Approx(0.0));
    CHECK(quality.weights.time == Catch::Approx(0.0));
    CHECK(quality.weights.material_and_multicolor_waste == Catch::Approx(0.0));

    const CandidateSearchBudget& budget = candidate_search_budget();
    CHECK(budget.version == CANDIDATE_SEARCH_BUDGET_VERSION);
    CHECK(budget.maximum_orientations_per_object == 4);
    CHECK(budget.beam_width == 8);
    CHECK(budget.maximum_static_drafts_per_goal == 6);
    CHECK(budget.maximum_trial_selected_per_goal == 3);
    CHECK(budget.baseline_trial_slots == 1);
    CHECK(budget.maximum_total_trial_slots == 10);
}

TEST_CASE("candidate search enforces exact orientation beam draft and trial budgets",
          "[AI][SmartSlicing][CandidateSearch]")
{
    const CandidateSearchResult result = CandidateSearchPipeline().search(fixture());
    REQUIRE(result.objects.size() == 2);
    CHECK(result.objects[0].normalized_orientation_count == 4);
    CHECK(result.objects[1].normalized_orientation_count == 4);
    CHECK(result.baseline.trial_slots == 1);
    CHECK(result.baseline.parameters.entries.empty());
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        const GoalCandidateDrafts& drafts = result.goal(goal);
        CHECK(drafts.board_beam_count == 8);
        CHECK(drafts.generated_static_draft_count == 6);
        CHECK(drafts.drafts.size() == 6);
        CHECK(drafts.selected_for_trial.size() == 3);
        for (const CandidateSearchDraft& draft : drafts.drafts)
            CHECK(draft.status == CandidateStatus::Draft);
    }
    CHECK(result.total_trial_slots == 10);
}

TEST_CASE("candidate search normalizes input order and deduplicates IDs transforms and patches",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput first = fixture();
    first.objects.front().orientation_options.push_back(
        option("duplicate-transform", OrientationStrategy::LowSupport,
               first.objects.front().orientation_options.front().transform[12]));
    first.profile_parameters.front().options.push_back(first.profile_parameters.front().options.front());
    CandidateSearchInput shuffled = first;
    std::reverse(shuffled.objects.begin(), shuffled.objects.end());
    std::reverse(shuffled.profile_parameters.begin(), shuffled.profile_parameters.end());
    for (auto& object : shuffled.objects)
        std::reverse(object.orientation_options.begin(), object.orientation_options.end());
    for (auto& parameter : shuffled.profile_parameters)
        std::reverse(parameter.options.begin(), parameter.options.end());

    const CandidateSearchResult lhs = CandidateSearchPipeline().search(first);
    const CandidateSearchResult rhs = CandidateSearchPipeline().search(shuffled);
    CHECK(lhs.baseline.candidate_id == rhs.baseline.candidate_id);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        const std::vector<CandidateId> goal_ids = ids(lhs.goal(goal));
        CHECK(goal_ids == ids(rhs.goal(goal)));
        std::set<CandidateId> unique(goal_ids.begin(), goal_ids.end());
        CHECK(unique.size() == lhs.goal(goal).drafts.size());
        for (size_t index = 0; index < lhs.goal(goal).drafts.size(); ++index) {
            CHECK(same_placement(lhs.goal(goal).drafts[index].placement,
                                 rhs.goal(goal).drafts[index].placement));
            CHECK(same_proposal(lhs.goal(goal).drafts[index].parameters,
                                rhs.goal(goal).drafts[index].parameters));
        }
    }
}

TEST_CASE("plate and target locks preserve current transforms without suppressing other objects",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput plate = fixture();
    plate.plate_locked = true;
    const CandidateSearchResult plate_result = CandidateSearchPipeline().search(plate);
    for (const CandidateSearchObjectSummary& object : plate_result.objects) {
        CHECK(object.locked);
        CHECK(object.normalized_orientation_count == 1);
    }
    for (const auto& draft : plate_result.goal(RecommendationGoal::Balanced).drafts)
        for (const auto& selected : draft.placement.transforms)
            CHECK(selected.matrix == transform(static_cast<double>(selected.object_id)));

    CandidateSearchInput one = fixture();
    one.objects.front().locked = true;
    const CandidateSearchResult one_result = CandidateSearchPipeline().search(one);
    REQUIRE(one_result.objects.size() == 2);
    CHECK(one_result.objects[0].normalized_orientation_count == 1);
    CHECK(one_result.objects[1].normalized_orientation_count == 4);

    CandidateSearchInput instance = fixture();
    IntentConstraintRecord instance_lock;
    instance_lock.type = IntentConstraintType::InstancePlacementLock;
    instance_lock.state = IntentConstraintState::Active;
    instance_lock.object_id = instance.objects.front().object_id;
    instance_lock.instance_id = instance.objects.front().instance_id + 1;
    instance.intent_constraints.records.push_back(instance_lock);
    const CandidateSearchResult other_instance = CandidateSearchPipeline().search(instance);
    CHECK(other_instance.objects.front().normalized_orientation_count == 4);
    instance.intent_constraints.records.front().instance_id = instance.objects.front().instance_id;
    const CandidateSearchResult target_instance = CandidateSearchPipeline().search(instance);
    CHECK(target_instance.objects.front().normalized_orientation_count == 1);
}

TEST_CASE("profile options are the only source of parameter values and unknown evidence stays unknown",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput input = fixture(1);
    auto& layer = *std::find_if(input.profile_parameters.begin(), input.profile_parameters.end(),
        [](const auto& parameter) { return parameter.key == "layer_height"; });
    layer.options.erase(std::remove_if(layer.options.begin(), layer.options.end(), [](const auto& value) {
        const auto* layer_height = std::get_if<double>(&value.value);
        return layer_height != nullptr && (*layer_height == 0.24 || *layer_height == 0.28);
    }), layer.options.end());
    input.objects.front().model_features.front().small_text_candidates =
        FeatureValue<std::vector<SmallTextCandidate>>::unknown("not_analyzed");
    input.objects.front().protected_region_status = ProtectedRegionBindingStatus::Unknown;
    input.objects.front().orientation_options.push_back(
        option("unknown-protected-surface", OrientationStrategy::ProtectedSurface, 99.0));

    const CandidateSearchResult result = CandidateSearchPipeline().search(input);
    const auto& speed = result.goal(RecommendationGoal::Speed);
    REQUIRE_FALSE(speed.drafts.empty());
    for (const CandidateSearchDraft& draft : speed.drafts) {
        CHECK(patch(draft, "layer_height") == nullptr);
        CHECK(std::find(draft.diagnostic_codes.begin(), draft.diagnostic_codes.end(),
                        "feature_or_protected_region_evidence_unknown") != draft.diagnostic_codes.end());
        CHECK(std::none_of(draft.explanation_codes.begin(), draft.explanation_codes.end(),
                           [](const std::string& value) { return value.find("improved") != std::string::npos; }));
        CHECK(std::none_of(draft.placement.transforms.begin(), draft.placement.transforms.end(),
                           [](const ObjectTransform& value) { return value.matrix[12] == 99.0; }));
    }
}

TEST_CASE("usage floors TPU permission and known risks constrain speed templates",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput general = fixture(1);
    auto& general_walls = *std::find_if(general.profile_parameters.begin(),
        general.profile_parameters.end(), [](const auto& parameter) { return parameter.key == "wall_loops"; });
    general_walls.current_value = int64_t{1};
    const auto general_result = CandidateSearchPipeline().search(general);
    REQUIRE_FALSE(general_result.goal(RecommendationGoal::Speed).drafts.empty());
    const CandidateSearchDraft& general_speed = general_result.goal(RecommendationGoal::Speed).drafts.front();
    REQUIRE(patch(general_speed, "wall_loops") != nullptr);
    CHECK(std::get<int64_t>(patch(general_speed, "wall_loops")->new_value) == 2);
    const ConfigPatchEntry* general_infill = patch(general_speed, "sparse_infill_density");
    CHECK((general_infill == nullptr ? 20.0 : std::get<double>(general_infill->new_value)) >= 15.0);

    CandidateSearchInput unknown = fixture(1);
    unknown.usage_purpose = UsagePurpose::Unknown;
    const auto unknown_result = CandidateSearchPipeline().search(unknown);
    REQUIRE_FALSE(unknown_result.goal(RecommendationGoal::Speed).drafts.empty());
    const ConfigPatchEntry* unknown_infill = patch(
        unknown_result.goal(RecommendationGoal::Speed).drafts.front(), "sparse_infill_density");
    CHECK((unknown_infill == nullptr ? 20.0 : std::get<double>(unknown_infill->new_value)) >= 15.0);

    CandidateSearchInput decoration = fixture(1);
    decoration.usage_purpose = UsagePurpose::Decoration;
    const auto decoration_result = CandidateSearchPipeline().search(decoration);
    REQUIRE_FALSE(decoration_result.goal(RecommendationGoal::Speed).drafts.empty());
    const ConfigPatchEntry* decoration_infill = patch(
        decoration_result.goal(RecommendationGoal::Speed).drafts.front(), "sparse_infill_density");
    CHECK((decoration_infill == nullptr ? 20.0 : std::get<double>(decoration_infill->new_value)) >= 10.0);

    CandidateSearchInput tpu = fixture(1);
    tpu.tpu_speed_restricted = true;
    const auto tpu_result = CandidateSearchPipeline().search(tpu);
    for (const CandidateSearchDraft& draft : tpu_result.goal(RecommendationGoal::Speed).drafts)
        CHECK(patch(draft, "outer_wall_speed") == nullptr);

    CandidateSearchInput no_permission = fixture(1);
    no_permission.may_increase_process_speed = false;
    const auto no_permission_result = CandidateSearchPipeline().search(no_permission);
    for (const CandidateSearchDraft& draft : no_permission_result.goal(RecommendationGoal::Speed).drafts)
        CHECK(patch(draft, "outer_wall_speed") == nullptr);

    CandidateSearchInput risky = fixture(1);
    risky.objects.front().model_features.front().thin_wall_candidates =
        FeatureValue<std::vector<ThinWallCandidate>>::known({ThinWallCandidate{}});
    const auto risky_result = CandidateSearchPipeline().search(risky);
    for (const CandidateSearchDraft& draft : risky_result.goal(RecommendationGoal::Speed).drafts) {
        const ConfigPatchEntry* layer_height = patch(draft, "layer_height");
        CHECK((layer_height == nullptr || std::get<double>(layer_height->new_value) != Catch::Approx(0.28)));
    }
}

TEST_CASE("speed usage floors fail closed and select the smallest valid profile correction",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput unavailable = fixture(1);
    auto& unavailable_walls = *std::find_if(unavailable.profile_parameters.begin(),
        unavailable.profile_parameters.end(), [](const auto& parameter) { return parameter.key == "wall_loops"; });
    unavailable_walls.current_value = int64_t{1};
    unavailable_walls.options = {profile_value(int64_t{1})};
    auto& unavailable_infill = *std::find_if(unavailable.profile_parameters.begin(),
        unavailable.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "sparse_infill_density";
        });
    unavailable_infill.current_value = 10.0;
    unavailable_infill.options = {profile_value(10.0)};

    const CandidateSearchResult unavailable_result = CandidateSearchPipeline().search(unavailable);
    CHECK(unavailable_result.goal(RecommendationGoal::Speed).drafts.empty());
    CHECK(unavailable_result.goal(RecommendationGoal::Speed).selected_for_trial.empty());
    REQUIRE(unavailable_result.goal(RecommendationGoal::Speed).diagnostic_codes.size() == 1);
    CHECK(unavailable_result.goal(RecommendationGoal::Speed).diagnostic_codes.front() ==
          "speed_usage_floor_unavailable");
    CHECK_FALSE(unavailable_result.goal(RecommendationGoal::Balanced).drafts.empty());
    CHECK_FALSE(unavailable_result.goal(RecommendationGoal::Quality).drafts.empty());

    CandidateSearchInput corrected = fixture(1);
    auto& corrected_walls = *std::find_if(corrected.profile_parameters.begin(),
        corrected.profile_parameters.end(), [](const auto& parameter) { return parameter.key == "wall_loops"; });
    corrected_walls.current_value = int64_t{1};
    corrected_walls.options = {profile_value(int64_t{4}), profile_value(int64_t{3})};
    auto& corrected_infill = *std::find_if(corrected.profile_parameters.begin(),
        corrected.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "sparse_infill_density";
        });
    corrected_infill.current_value = 10.0;
    corrected_infill.options = {profile_value(20.0), profile_value(16.0)};

    const CandidateSearchResult corrected_result = CandidateSearchPipeline().search(corrected);
    REQUIRE_FALSE(corrected_result.goal(RecommendationGoal::Speed).drafts.empty());
    const CandidateSearchDraft& speed = corrected_result.goal(RecommendationGoal::Speed).drafts.front();
    REQUIRE(patch(speed, "wall_loops") != nullptr);
    CHECK(std::get<int64_t>(patch(speed, "wall_loops")->new_value) == 3);
    REQUIRE(patch(speed, "sparse_infill_density") != nullptr);
    CHECK(std::get<double>(patch(speed, "sparse_infill_density")->new_value) == Catch::Approx(16.0));
}

TEST_CASE("orientation-only candidates require a real unlocked placement change",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput input = fixture(1);
    input.profile_parameters.clear();
    const CandidateSearchResult result = CandidateSearchPipeline().search(input);
    for (const RecommendationGoal goal : {RecommendationGoal::Balanced, RecommendationGoal::Quality}) {
        const GoalCandidateDrafts& drafts = result.goal(goal);
        REQUIRE_FALSE(drafts.drafts.empty());
        for (const CandidateSearchDraft& draft : drafts.drafts) {
            CHECK(draft.parameters.entries.empty());
            REQUIRE(draft.placement.transforms.size() == 1);
            CHECK(draft.placement.transforms.front().matrix != input.objects.front().current_transform);
        }
    }
    CHECK(result.goal(RecommendationGoal::Speed).drafts.empty());
    CHECK(result.goal(RecommendationGoal::Speed).diagnostic_codes.front() ==
          "speed_usage_floor_unavailable");

    input.plate_locked = true;
    const CandidateSearchResult locked = CandidateSearchPipeline().search(input);
    CHECK(locked.goal(RecommendationGoal::Balanced).drafts.empty());
    CHECK(locked.goal(RecommendationGoal::Quality).drafts.empty());
}

TEST_CASE("candidate validation continues to later templates before applying the accepted draft budget",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput input = fixture(1);
    input.native_validator = [](const ParameterProposal& proposal) {
        const auto layer = std::find_if(proposal.entries.begin(), proposal.entries.end(),
            [](const ConfigPatchEntry& entry) { return entry.key == "layer_height"; });
        const bool reject_fine = proposal.goal == RecommendationGoal::Quality &&
            layer != proposal.entries.end() &&
            std::abs(std::get<double>(layer->new_value) - 0.12) <= 1e-9;
        return NativeParameterValidationResult{!reject_fine, reject_fine ? "fixture_fine_rejected" : ""};
    };

    const CandidateSearchResult result = CandidateSearchPipeline().search(input);
    const GoalCandidateDrafts& quality = result.goal(RecommendationGoal::Quality);
    REQUIRE_FALSE(quality.rejected_drafts.empty());
    REQUIRE_FALSE(quality.drafts.empty());
    CHECK(quality.drafts.size() <= candidate_search_budget().maximum_static_drafts_per_goal);
    CHECK(quality.selected_for_trial.size() <= candidate_search_budget().maximum_trial_selected_per_goal);
    for (const CandidateSearchDraft& draft : quality.drafts) {
        REQUIRE(patch(draft, "layer_height") != nullptr);
        CHECK(std::get<double>(patch(draft, "layer_height")->new_value) == Catch::Approx(0.16));
    }
    const CandidateSearchResult repeated = CandidateSearchPipeline().search(input);
    CHECK(ids(quality) == ids(repeated.goal(RecommendationGoal::Quality)));
    REQUIRE(quality.rejected_drafts.size() ==
            repeated.goal(RecommendationGoal::Quality).rejected_drafts.size());
    for (size_t index = 0; index < quality.rejected_drafts.size(); ++index)
        CHECK(quality.rejected_drafts[index].candidate_id ==
              repeated.goal(RecommendationGoal::Quality).rejected_drafts[index].candidate_id);
}

TEST_CASE("shell thickness floors use effective layer height and fail closed without profile correction",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput unavailable = fixture(1);
    auto& unavailable_top = *std::find_if(unavailable.profile_parameters.begin(),
        unavailable.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "top_shell_layers";
        });
    unavailable_top.current_value = int64_t{1};
    unavailable_top.options = {profile_value(int64_t{1})};
    auto& unavailable_bottom = *std::find_if(unavailable.profile_parameters.begin(),
        unavailable.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "bottom_shell_layers";
        });
    unavailable_bottom.current_value = int64_t{1};
    unavailable_bottom.options = {profile_value(int64_t{1})};

    const CandidateSearchResult unavailable_result = CandidateSearchPipeline().search(unavailable);
    CHECK(unavailable_result.goal(RecommendationGoal::Balanced).drafts.empty());
    REQUIRE(unavailable_result.goal(RecommendationGoal::Balanced).diagnostic_codes.size() == 1);
    CHECK(unavailable_result.goal(RecommendationGoal::Balanced).diagnostic_codes.front() ==
          "shell_thickness_floor_unavailable");

    CandidateSearchInput no_preferred_layer = fixture(1);
    auto& no_preferred_layer_height = *std::find_if(no_preferred_layer.profile_parameters.begin(),
        no_preferred_layer.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "layer_height";
        });
    no_preferred_layer_height.current_value = 0.30;
    no_preferred_layer_height.options = {profile_value(0.30)};
    auto& no_preferred_top = *std::find_if(no_preferred_layer.profile_parameters.begin(),
        no_preferred_layer.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "top_shell_layers";
        });
    no_preferred_top.current_value = int64_t{2};
    no_preferred_top.options = {profile_value(int64_t{2})};
    auto& no_preferred_bottom = *std::find_if(no_preferred_layer.profile_parameters.begin(),
        no_preferred_layer.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "bottom_shell_layers";
        });
    no_preferred_bottom.current_value = int64_t{2};
    no_preferred_bottom.options = {profile_value(int64_t{2})};
    const CandidateSearchResult no_preferred_result =
        CandidateSearchPipeline().search(no_preferred_layer);
    CHECK(no_preferred_result.goal(RecommendationGoal::Balanced).drafts.empty());
    REQUIRE(no_preferred_result.goal(RecommendationGoal::Balanced).diagnostic_codes.size() == 1);
    CHECK(no_preferred_result.goal(RecommendationGoal::Balanced).diagnostic_codes.front() ==
          "shell_thickness_floor_unavailable");

    CandidateSearchInput corrected = fixture(1);
    auto& corrected_top = *std::find_if(corrected.profile_parameters.begin(),
        corrected.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "top_shell_layers";
        });
    corrected_top.current_value = int64_t{1};
    corrected_top.options = {profile_value(int64_t{4}), profile_value(int64_t{5}),
                             profile_value(int64_t{7}), profile_value(int64_t{9})};
    auto& corrected_bottom = *std::find_if(corrected.profile_parameters.begin(),
        corrected.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "bottom_shell_layers";
        });
    corrected_bottom.current_value = int64_t{1};
    corrected_bottom.options = {profile_value(int64_t{3}), profile_value(int64_t{4}),
                                profile_value(int64_t{5}), profile_value(int64_t{7})};

    const CandidateSearchResult corrected_result = CandidateSearchPipeline().search(corrected);
    REQUIRE_FALSE(corrected_result.goal(RecommendationGoal::Balanced).drafts.empty());
    for (const CandidateSearchDraft& draft : corrected_result.goal(RecommendationGoal::Balanced).drafts) {
        const ConfigPatchEntry* layer = patch(draft, "layer_height");
        const ConfigPatchEntry* top = patch(draft, "top_shell_layers");
        const ConfigPatchEntry* bottom = patch(draft, "bottom_shell_layers");
        const double effective_layer = layer == nullptr ? 0.20 : std::get<double>(layer->new_value);
        const int64_t effective_top = top == nullptr ? 1 : std::get<int64_t>(top->new_value);
        const int64_t effective_bottom = bottom == nullptr ? 1 : std::get<int64_t>(bottom->new_value);
        CHECK(static_cast<double>(effective_top) * effective_layer + 1e-9 >= 0.8);
        CHECK(static_cast<double>(effective_bottom) * effective_layer + 1e-9 >= 0.6);
    }
}

TEST_CASE("support interface floors apply only when support is enabled",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput disabled = fixture(1);
    auto& disabled_interface = *std::find_if(disabled.profile_parameters.begin(),
        disabled.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "support_interface_top_layers";
        });
    disabled_interface.options = {profile_value(int64_t{0})};
    const CandidateSearchResult disabled_result = CandidateSearchPipeline().search(disabled);
    REQUIRE_FALSE(disabled_result.goal(RecommendationGoal::Quality).drafts.empty());
    for (const CandidateSearchDraft& draft : disabled_result.goal(RecommendationGoal::Quality).drafts)
        CHECK(patch(draft, "support_interface_top_layers") == nullptr);

    CandidateSearchInput unknown = fixture(1);
    unknown.profile_parameters.erase(std::remove_if(unknown.profile_parameters.begin(),
        unknown.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "enable_support";
        }), unknown.profile_parameters.end());
    const CandidateSearchResult unknown_result = CandidateSearchPipeline().search(unknown);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        CHECK(unknown_result.goal(goal).drafts.empty());
        REQUIRE(unknown_result.goal(goal).diagnostic_codes.size() == 1);
        CHECK(unknown_result.goal(goal).diagnostic_codes.front() == "support_enablement_unknown");
    }

    CandidateSearchInput unavailable = fixture(1);
    auto& unavailable_support = *std::find_if(unavailable.profile_parameters.begin(),
        unavailable.profile_parameters.end(), [](const auto& parameter) { return parameter.key == "enable_support"; });
    unavailable_support.current_value = true;
    auto& unavailable_interface = *std::find_if(unavailable.profile_parameters.begin(),
        unavailable.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "support_interface_top_layers";
        });
    unavailable_interface.current_value = int64_t{1};
    unavailable_interface.options = {profile_value(int64_t{1})};
    const CandidateSearchResult unavailable_result = CandidateSearchPipeline().search(unavailable);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        CHECK(unavailable_result.goal(goal).drafts.empty());
        REQUIRE(unavailable_result.goal(goal).diagnostic_codes.size() == 1);
        CHECK(unavailable_result.goal(goal).diagnostic_codes.front() ==
              "support_interface_floor_unavailable");
    }

    CandidateSearchInput corrected = fixture(1);
    auto& corrected_support = *std::find_if(corrected.profile_parameters.begin(),
        corrected.profile_parameters.end(), [](const auto& parameter) { return parameter.key == "enable_support"; });
    corrected_support.current_value = true;
    auto& corrected_interface = *std::find_if(corrected.profile_parameters.begin(),
        corrected.profile_parameters.end(), [](const auto& parameter) {
            return parameter.key == "support_interface_top_layers";
        });
    corrected_interface.current_value = int64_t{1};
    corrected_interface.options = {profile_value(int64_t{2}), profile_value(int64_t{3}),
                                   profile_value(int64_t{4})};
    const CandidateSearchResult corrected_result = CandidateSearchPipeline().search(corrected);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        REQUIRE_FALSE(corrected_result.goal(goal).drafts.empty());
        const ConfigPatchEntry* interface =
            patch(corrected_result.goal(goal).drafts.front(), "support_interface_top_layers");
        REQUIRE(interface != nullptr);
        const int64_t minimum = goal == RecommendationGoal::Quality ? 3 : 2;
        CHECK(std::get<int64_t>(interface->new_value) == minimum);
    }
}

TEST_CASE("conflicting parameter options and bounds fail closed independent of input order",
          "[AI][SmartSlicing][CandidateSearch]")
{
    auto require_conflict = [](const CandidateSearchInput& input) {
        const CandidateSearchResult result = CandidateSearchPipeline().search(input);
        for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
            CHECK(result.goal(goal).drafts.empty());
            REQUIRE(result.goal(goal).diagnostic_codes.size() == 1);
            CHECK(result.goal(goal).diagnostic_codes.front() ==
                  "candidate_parameter_evidence_conflict");
        }
    };

    CandidateSearchInput duplicate = fixture(1);
    duplicate.profile_parameters.push_back(duplicate.profile_parameters.front());
    duplicate.profile_parameters.back().current_value = 0.18;
    require_conflict(duplicate);
    std::reverse(duplicate.profile_parameters.begin(), duplicate.profile_parameters.end());
    require_conflict(duplicate);

    CandidateSearchInput option_conflict = fixture(1);
    auto conflicting_option = option_conflict.profile_parameters.front().options.front();
    conflicting_option.valid = false;
    option_conflict.profile_parameters.front().options.push_back(conflicting_option);
    require_conflict(option_conflict);

    CandidateSearchInput bound_conflict = fixture(1);
    ParameterBoundEvidence conflicting_bound = bound_conflict.profile_parameters.front().bounds.front();
    conflicting_bound.maximum = 0.30;
    bound_conflict.profile_parameters.front().bounds.push_back(conflicting_bound);
    require_conflict(bound_conflict);

    CandidateSearchInput exact_duplicate = fixture(1);
    exact_duplicate.profile_parameters.push_back(exact_duplicate.profile_parameters.front());
    const CandidateSearchResult first = CandidateSearchPipeline().search(exact_duplicate);
    std::reverse(exact_duplicate.profile_parameters.begin(), exact_duplicate.profile_parameters.end());
    const CandidateSearchResult second = CandidateSearchPipeline().search(exact_duplicate);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS)
        CHECK(ids(first.goal(goal)) == ids(second.goal(goal)));
}

TEST_CASE("invalid revisions and conflicting duplicate object identities fail closed",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput invalid = fixture(1);
    invalid.workspace_revision.fingerprint.clear();
    const CandidateSearchResult invalid_result = CandidateSearchPipeline().search(invalid);
    CHECK(invalid_result.baseline.candidate_id.empty());
    CHECK(invalid_result.baseline.trial_slots == 0);
    CHECK(invalid_result.total_trial_slots == 0);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        CHECK(invalid_result.goal(goal).drafts.empty());
        REQUIRE(invalid_result.goal(goal).diagnostic_codes.size() == 1);
        CHECK(invalid_result.goal(goal).diagnostic_codes.front() == "workspace_revision_invalid");
    }

    CandidateSearchInput duplicate = fixture(1);
    duplicate.objects.push_back(duplicate.objects.front());
    duplicate.objects.back().current_transform = transform(999.0);
    const CandidateSearchResult duplicate_result = CandidateSearchPipeline().search(duplicate);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        CHECK(duplicate_result.goal(goal).drafts.empty());
        REQUIRE(duplicate_result.goal(goal).diagnostic_codes.size() == 1);
        CHECK(duplicate_result.goal(goal).diagnostic_codes.front() ==
              "candidate_object_identity_conflict");
    }
}

TEST_CASE("goal validation failures remain isolated and never enter trial selection",
          "[AI][SmartSlicing][CandidateSearch]")
{
    CandidateSearchInput input = fixture(1);
    input.native_validator = [](const ParameterProposal& proposal) {
        return NativeParameterValidationResult{proposal.goal != RecommendationGoal::Quality,
                                               "fixture_quality_rejected"};
    };
    const CandidateSearchResult result = CandidateSearchPipeline().search(input);
    CHECK_FALSE(result.goal(RecommendationGoal::Balanced).selected_for_trial.empty());
    CHECK_FALSE(result.goal(RecommendationGoal::Speed).selected_for_trial.empty());
    CHECK(result.goal(RecommendationGoal::Quality).selected_for_trial.empty());
    CHECK(result.goal(RecommendationGoal::Quality).drafts.empty());
    CHECK_FALSE(result.goal(RecommendationGoal::Quality).rejected_drafts.empty());
    for (const RejectedCandidateDraft& rejected : result.goal(RecommendationGoal::Quality).rejected_drafts) {
        CHECK(rejected.diagnostic_codes.front() == "parameter_native_validation_failed");
        CHECK(rejected.diagnostic_codes.back() == "fixture_quality_rejected");
    }
}
