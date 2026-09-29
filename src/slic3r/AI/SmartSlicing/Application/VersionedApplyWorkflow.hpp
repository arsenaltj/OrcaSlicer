#pragma once

#include "ApplyService.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"

#include <optional>
#include <string>

namespace Slic3r::AI::SmartSlicing {

struct VersionedApplyRequest
{
    std::string command_id;
    RiskConfirmationSubmission confirmation;
};

class VersionedApplyWorkflow
{
public:
    explicit VersionedApplyWorkflow(IOfficialSliceGateway& gateway) : m_gateway(gateway) {}

    OfficialSliceResult start(const VersionedApplyRequest& request,
                              const ApplyExpectedContext& current);
    OfficialSliceResult retry(const OfficialApplyTransactionIdentity& transaction);
    OfficialSliceResult poll();
    OfficialSliceResult notify_slice_completed(bool success, std::string diagnostic_code = {});
    OfficialSliceResult undo(const OfficialApplyTransactionIdentity& transaction);

    const std::optional<OfficialApplyTransactionIdentity>& active_transaction() const
    {
        return m_active_transaction;
    }

private:
    static OfficialSliceResult rejected(std::string diagnostic_code);
    bool matches_active(const OfficialApplyTransactionIdentity& transaction) const;

    IOfficialSliceGateway& m_gateway;
    ApplyService m_apply_service;
    std::optional<OfficialApplyTransactionIdentity> m_active_transaction;
};

} // namespace Slic3r::AI::SmartSlicing
