#pragma once

#include "slic3r/AI/SmartSlicing/Application/RecommendationSessionCoordinator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ApplyContract.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/RiskConfirmationPolicy.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceContext.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct ApplyActivitySnapshot
{
    bool model_tool_active{false};
    bool official_transaction_active{false};
    std::string source_version;
};

struct ApplyExpectedContext
{
    RecommendationSessionSnapshot session;
    RecommendationGoal goal{RecommendationGoal::Balanced};
    SliceCandidate selected_candidate;
    WorkspaceContext workspace;
    ParameterValidationContext parameter_validation;
    ApplyActivitySnapshot activity;
};

struct RiskConfirmationSubmission
{
    std::vector<RiskConfirmationKind> confirmed_risks;
};

struct ReadyApplyBindingResult
{
    std::optional<ReadyApplyBinding> binding;
    ApplyRejectionCode rejection{ApplyRejectionCode::None};
    std::vector<std::string> diagnostic_details;

    bool accepted() const { return binding.has_value(); }
};

struct RiskConfirmationTokenResult
{
    std::optional<RiskConfirmationToken> token;
    ApplyRejectionCode rejection{ApplyRejectionCode::None};
    std::vector<std::string> diagnostic_details;

    bool accepted() const { return token.has_value(); }
};

struct ApplyGuardResult
{
    std::optional<AtomicApplyPlan> plan;
    ApplyRejectionCode rejection{ApplyRejectionCode::None};
    std::vector<std::string> diagnostic_details;

    bool accepted() const { return plan.has_value(); }
};

class ApplyService
{
public:
    ReadyApplyBindingResult capture_ready_binding(const ApplyExpectedContext& context) const;
    RiskConfirmationTokenResult issue_confirmation_token(
        const ReadyApplyBinding& binding, const ApplyExpectedContext& current,
        const RiskConfirmationSubmission& submission) const;
    ApplyGuardResult make_atomic_plan(const ApplyCommand& command, const ReadyApplyBinding& binding,
                                      const ApplyExpectedContext& current);

    size_t consumed_command_count() const { return m_consumed_command_ids.size(); }

private:
    std::unordered_set<std::string> m_consumed_command_ids;
};

} // namespace Slic3r::AI::SmartSlicing
