#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/ApplyService.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <utility>

using namespace Slic3r::AI::SmartSlicing;

namespace {

EvidenceValue<double> available(double value, const char* source)
{
    return {EvidenceAvailability::Available, value, source};
}

RecommendationEvidence ready_evidence()
{
    RecommendationEvidence evidence;
    evidence.selection_policy_version = CANDIDATE_SELECTION_POLICY_VERSION;
    evidence.explanation_codes = {"protected_region_risk_assessed"};
    evidence.estimated_time_ratio = available(1.05, "trial_time");
    evidence.material_ratio = available(1.02, "trial_material");
    evidence.appearance_risk = available(0.1, "risk_appearance");
    evidence.dimensional_risk = available(0.2, "risk_dimensional");
    evidence.strength_risk = available(0.1, "risk_strength");
    evidence.retained_strength_ratio = available(0.98, "risk_strength_retained");
    evidence.reliability_risk = available(0.1, "risk_reliability");
    evidence.protected_region_risk = available(0.1, "risk_protected_region");
    REQUIRE(validate_recommendation_evidence(evidence).empty());
    return evidence;
}

ApplyExpectedContext base_context()
{
    ApplyExpectedContext context;
    context.session.workflow_id = 61;
    context.session.attempt_id = 4;
    context.session.workspace_revision = {2, 3, 5, "risk-confirmation-revision"};
    context.session.state = RecommendationSessionState::Ready;
    context.session.strategy_version = "candidate-search-strategy/v1";
    context.session.recommendation.baseline = {"baseline", GoalResultStatus::Ready, {}};
    GoalResult& goal = context.session.recommendation.goal_result(RecommendationGoal::Balanced);
    goal.candidate_id = "balanced-task";
    goal.status = GoalResultStatus::Ready;
    goal.evidence = ready_evidence();
    goal.selected_candidate_id = "balanced-selected";

    context.goal = RecommendationGoal::Balanced;
    context.selected_candidate.id = goal.selected_candidate_id;
    context.selected_candidate.base_revision = context.session.workspace_revision;
    context.selected_candidate.status = CandidateStatus::Ready;
    context.selected_candidate.parameters.goal = context.goal;
    context.selected_candidate.parameters.policy_version = PARAMETER_POLICY_VERSION;
    context.selected_candidate.parameters.entries.push_back(
        {ConfigScope::Plate, PresetOwner::Process, 0, "enable_support", false, true,
         "support_required"});

    context.workspace.revision = context.session.workspace_revision;
    context.workspace.machine_capability.registry_version = "machine-registry/v1";
    context.workspace.machine_capability.support_status = MachineSupportStatus::Enabled;
    context.workspace.material_compatibility.registry_version = "material-registry/v1";
    context.workspace.material_compatibility.combination_status =
        MaterialCombinationStatus::Compatible;
    context.workspace.material_compatibility.common_family = MaterialFamily::PLA;
    context.parameter_validation.goal = context.goal;
    context.parameter_validation.current_values.push_back(
        {ConfigScope::Plate, PresetOwner::Process, 0, "enable_support", false});
    context.parameter_validation.native_validator = [](const ParameterProposal&) {
        return NativeParameterValidationResult{true, {}};
    };
    context.activity.source_version = "orca-owner-activity/v1";
    return context;
}

RiskConfirmationPublicationIdentity publication_identity(const ApplyExpectedContext& context)
{
    const GoalResult& goal = context.session.recommendation.goal_result(context.goal);
    return {context.session.workflow_id,
            context.session.attempt_id,
            context.session.workspace_revision,
            context.goal,
            goal.candidate_id,
            goal.selected_candidate_id};
}

std::vector<RiskConfirmationFact> known_facts(bool support_contact, bool seam,
                                              std::string source_version =
                                                  "protected-region-owner/v1")
{
    return {
        {RiskConfirmationKind::ProtectedRegionSupportContact,
         RiskConfirmationFactAvailability::Known, support_contact,
         "protected_region_support_contact", source_version},
        {RiskConfirmationKind::ProtectedRegionSeam,
         RiskConfirmationFactAvailability::Known, seam, "protected_region_seam",
         std::move(source_version)},
    };
}

RiskConfirmationMappingInput mapping_input(const ApplyExpectedContext& context,
                                            bool support_contact = true, bool seam = true)
{
    RiskConfirmationMappingInput input;
    input.publication_identity = publication_identity(context);
    input.published_evidence =
        *context.session.recommendation.goal_result(context.goal).evidence;
    input.facts = known_facts(support_contact, seam);
    return input;
}

RiskConfirmationMappingResult publish(ApplyExpectedContext& context, bool support_contact = true,
                                      bool seam = true,
                                      std::string source_version =
                                          "protected-region-owner/v1")
{
    RiskConfirmationMappingInput input = mapping_input(context, support_contact, seam);
    input.facts = known_facts(support_contact, seam, std::move(source_version));
    RiskConfirmationMappingResult result = RiskConfirmationMapper{}.map(input);
    if (result.accepted())
        context.session.recommendation.goal_result(context.goal).risk_confirmation_contract =
            result.contract;
    return result;
}

RiskConfirmationToken issue_token(ApplyService& service, const ReadyApplyBinding& binding,
                                  const ApplyExpectedContext& context,
                                  std::vector<RiskConfirmationKind> confirmations)
{
    const RiskConfirmationTokenResult result = service.issue_confirmation_token(
        binding, context, {std::move(confirmations)});
    REQUIRE(result.accepted());
    return *result.token;
}

ApplyCommand command_for(const char* id, const ReadyApplyBinding& binding,
                         const RiskConfirmationToken& token)
{
    ApplyCommand command;
    command.command_id = id;
    command.binding = binding;
    command.confirmation_token = token;
    return command;
}

} // namespace

TEST_CASE("Owner risk policy derives the exact protected-region confirmation set",
          "[AI][SmartSlicing][D6T2][RiskConfirmation]")
{
    struct Scenario
    {
        bool support_contact;
        bool seam;
        std::vector<RiskConfirmationKind> expected;
    };
    const std::array<Scenario, 4> scenarios{{
        {false, false, {}},
        {true, false, {RiskConfirmationKind::ProtectedRegionSupportContact}},
        {false, true, {RiskConfirmationKind::ProtectedRegionSeam}},
        {true, true,
         {RiskConfirmationKind::ProtectedRegionSupportContact,
          RiskConfirmationKind::ProtectedRegionSeam}},
    }};

    const ApplyExpectedContext context = base_context();
    for (const Scenario& scenario : scenarios) {
        DYNAMIC_SECTION("support=" << scenario.support_contact << " seam=" << scenario.seam)
        {
            const RiskConfirmationMappingResult result = RiskConfirmationMapper{}.map(
                mapping_input(context, scenario.support_contact, scenario.seam));
            REQUIRE(result.accepted());
            CHECK(result.code == RiskConfirmationMappingCode::None);
            CHECK(result.contract->policy_version() == RISK_CONFIRMATION_POLICY_VERSION);
            CHECK(result.contract->publication_identity() == publication_identity(context));
            CHECK(result.contract->required_confirmations() == scenario.expected);
            CHECK_FALSE(result.contract->source_evidence_digest().empty());
        }
    }
}

TEST_CASE("Owner risk policy canonicalizes source fact order and binds source evidence",
          "[AI][SmartSlicing][D6T2][RiskConfirmation]")
{
    const ApplyExpectedContext context = base_context();
    RiskConfirmationMappingInput ordered = mapping_input(context);
    RiskConfirmationMappingInput reversed = ordered;
    std::reverse(reversed.facts.begin(), reversed.facts.end());
    RiskConfirmationMappingInput changed = ordered;
    changed.facts.front().source_version = "protected-region-owner/v2";

    const auto first = RiskConfirmationMapper{}.map(ordered);
    const auto second = RiskConfirmationMapper{}.map(reversed);
    const auto third = RiskConfirmationMapper{}.map(changed);
    REQUIRE(first.accepted());
    REQUIRE(second.accepted());
    REQUIRE(third.accepted());
    CHECK(first.contract->source_evidence_digest() == second.contract->source_evidence_digest());
    CHECK(first.contract->required_confirmations() == second.contract->required_confirmations());
    CHECK(first.contract->source_evidence_digest() != third.contract->source_evidence_digest());
}

TEST_CASE("Owner risk policy fails closed for incomplete unavailable and invalid evidence",
          "[AI][SmartSlicing][D6T2][RiskConfirmation]")
{
    const ApplyExpectedContext context = base_context();
    const auto rejected_code = [&](RiskConfirmationMappingInput input) {
        const RiskConfirmationMappingResult result = RiskConfirmationMapper{}.map(input);
        CHECK_FALSE(result.accepted());
        return result.code;
    };

    SECTION("unknown policy")
    {
        auto input = mapping_input(context);
        input.policy_version = "risk-confirmation-policy/v2";
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::UnknownPolicy);
    }
    SECTION("missing evidence")
    {
        auto input = mapping_input(context);
        input.facts.pop_back();
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::MissingEvidence);
    }
    SECTION("unknown evidence")
    {
        auto input = mapping_input(context);
        input.facts.front().availability = RiskConfirmationFactAvailability::Unknown;
        input.facts.front().unavoidable.reset();
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::UnknownEvidence);
    }
    SECTION("unavailable evidence")
    {
        auto input = mapping_input(context);
        input.facts.front().availability = RiskConfirmationFactAvailability::Unavailable;
        input.facts.front().unavoidable.reset();
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::UnavailableEvidence);
    }
    SECTION("invalid kind")
    {
        auto input = mapping_input(context);
        input.facts.front().kind = static_cast<RiskConfirmationKind>(99);
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::InvalidEvidence);
    }
    SECTION("invalid availability")
    {
        auto input = mapping_input(context);
        input.facts.front().availability = static_cast<RiskConfirmationFactAvailability>(99);
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::InvalidEvidence);
    }
    SECTION("known evidence without a value")
    {
        auto input = mapping_input(context);
        input.facts.front().unavoidable.reset();
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::InvalidEvidence);
    }
    SECTION("known evidence without source details")
    {
        auto input = mapping_input(context);
        input.facts.front().evidence_code.clear();
        CHECK(rejected_code(input) == RiskConfirmationMappingCode::InvalidEvidence);
        input = mapping_input(context);
        input.facts.front().source_version.clear();
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::InvalidEvidence);
    }
    SECTION("duplicate evidence")
    {
        auto input = mapping_input(context);
        input.facts.push_back(input.facts.front());
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::InvalidEvidence);
    }
    SECTION("conflicting evidence")
    {
        auto input = mapping_input(context);
        RiskConfirmationFact conflict = input.facts.front();
        conflict.unavoidable = false;
        input.facts.push_back(std::move(conflict));
        CHECK(rejected_code(std::move(input)) == RiskConfirmationMappingCode::ConflictingEvidence);
    }
    SECTION("invalid publication identity")
    {
        auto input = mapping_input(context);
        input.publication_identity.workflow_id = 0;
        CHECK(rejected_code(std::move(input)) ==
              RiskConfirmationMappingCode::InvalidPublicationIdentity);
    }
    SECTION("invalid published recommendation evidence")
    {
        auto input = mapping_input(context);
        input.published_evidence.selection_policy_version.clear();
        CHECK(rejected_code(std::move(input)) ==
              RiskConfirmationMappingCode::InvalidRecommendationEvidence);
    }
}

TEST_CASE("Apply consumes only the owner contract frozen into the published goal",
          "[AI][SmartSlicing][D6T2][RiskConfirmation]")
{
    ApplyExpectedContext context = base_context();
    ApplyService service;
    const ReadyApplyBindingResult missing = service.capture_ready_binding(context);
    CHECK_FALSE(missing.accepted());
    CHECK(missing.rejection == ApplyRejectionCode::RequiredConfirmationsChanged);
    CHECK(service.consumed_command_count() == 0);

    const RiskConfirmationMappingResult mapped = publish(context, true, true);
    REQUIRE(mapped.accepted());
    const ReadyApplyBindingResult binding = service.capture_ready_binding(context);
    REQUIRE(binding.accepted());
    CHECK(binding.binding->required_confirmations ==
          std::vector<RiskConfirmationKind>{RiskConfirmationKind::ProtectedRegionSupportContact,
                                            RiskConfirmationKind::ProtectedRegionSeam});

    const RiskConfirmationTokenResult bypass =
        service.issue_confirmation_token(*binding.binding, context, {});
    CHECK_FALSE(bypass.accepted());
    CHECK(bypass.rejection == ApplyRejectionCode::RiskConfirmationRequired);
    CHECK(service.consumed_command_count() == 0);
}

TEST_CASE("Apply rejects stale evidence and contracts from another owner publication",
          "[AI][SmartSlicing][D6T2][RiskConfirmation]")
{
    ApplyExpectedContext context = base_context();
    REQUIRE(publish(context).accepted());
    ApplyService issuer;
    const ReadyApplyBindingResult binding = issuer.capture_ready_binding(context);
    REQUIRE(binding.accepted());
    const std::vector<RiskConfirmationKind> required = binding.binding->required_confirmations;
    const RiskConfirmationToken token = issue_token(issuer, *binding.binding, context, required);

    ApplyExpectedContext changed_evidence = context;
    changed_evidence.session.recommendation.goal_result(context.goal)
        .evidence->protected_region_risk.value = 0.4;
    ApplyService stale_service;
    const ApplyGuardResult stale = stale_service.make_atomic_plan(
        command_for("stale-evidence", *binding.binding, token), *binding.binding,
        changed_evidence);
    CHECK_FALSE(stale.accepted());
    CHECK(stale.rejection == ApplyRejectionCode::RiskEvidenceChanged);
    CHECK(stale_service.consumed_command_count() == 1);

    const auto check_cross_publication = [&](const char* name,
                                             const std::function<void(ApplyExpectedContext&)>& mutate,
                                             ApplyRejectionCode expected) {
        DYNAMIC_SECTION(name)
        {
            ApplyExpectedContext other = base_context();
            mutate(other);
            REQUIRE(publish(other).accepted());
            ApplyExpectedContext current = base_context();
            current.session.recommendation.goal_result(current.goal).risk_confirmation_contract =
                other.session.recommendation.goal_result(other.goal).risk_confirmation_contract;
            ApplyService cross_publication_service;
            const ReadyApplyBindingResult cross =
                cross_publication_service.capture_ready_binding(current);
            CHECK_FALSE(cross.accepted());
            CHECK(cross.rejection == expected);
            CHECK(cross_publication_service.consumed_command_count() == 0);
        }
    };
    check_cross_publication("workflow", [](auto& value) { ++value.session.workflow_id; },
                            ApplyRejectionCode::WorkflowChanged);
    check_cross_publication("attempt", [](auto& value) { ++value.session.attempt_id; },
                            ApplyRejectionCode::AttemptChanged);
    check_cross_publication("selected candidate", [](auto& value) {
        GoalResult& goal = value.session.recommendation.goal_result(value.goal);
        goal.selected_candidate_id = "other-selected";
        value.selected_candidate.id = goal.selected_candidate_id;
    }, ApplyRejectionCode::SelectedCandidateChanged);
}

TEST_CASE("Confirmation binding and token include canonical owner source evidence",
          "[AI][SmartSlicing][D6T2][RiskConfirmation]")
{
    ApplyExpectedContext first = base_context();
    ApplyExpectedContext reordered = base_context();
    ApplyExpectedContext changed = base_context();
    REQUIRE(publish(first).accepted());

    RiskConfirmationMappingInput reordered_input = mapping_input(reordered);
    std::reverse(reordered_input.facts.begin(), reordered_input.facts.end());
    const auto reordered_contract = RiskConfirmationMapper{}.map(reordered_input);
    REQUIRE(reordered_contract.accepted());
    reordered.session.recommendation.goal_result(reordered.goal).risk_confirmation_contract =
        reordered_contract.contract;
    REQUIRE(publish(changed, true, true, "protected-region-owner/v2").accepted());

    const auto token_id = [](const ApplyExpectedContext& value) {
        ApplyService service;
        const ReadyApplyBindingResult binding = service.capture_ready_binding(value);
        REQUIRE(binding.accepted());
        return issue_token(service, *binding.binding, value,
                           binding.binding->required_confirmations)
            .token_id();
    };

    CHECK(token_id(first) == token_id(reordered));
    CHECK(token_id(first) != token_id(changed));
}

TEST_CASE("A complete owner no-risk publication permits an empty confirmation submission",
          "[AI][SmartSlicing][D6T2][RiskConfirmation]")
{
    ApplyExpectedContext context = base_context();
    REQUIRE(publish(context, false, false).accepted());
    ApplyService service;
    const ReadyApplyBindingResult binding = service.capture_ready_binding(context);
    REQUIRE(binding.accepted());
    CHECK(binding.binding->required_confirmations.empty());
    CHECK(service.issue_confirmation_token(*binding.binding, context, {}).accepted());
}
