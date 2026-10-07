#include "SmartSlicingPresenter.hpp"

#include <utility>

namespace Slic3r::GUI {

SmartSlicingPresenter::SmartSlicingPresenter(AI::SmartSlicing::SmartSlicingCoordinator& coordinator, DispatchFn dispatch)
    : m_coordinator(coordinator), m_dispatch(std::move(dispatch))
{
    m_coordinator.set_observer([this](const AI::SmartSlicing::WorkflowSnapshot& snapshot) {
        SmartSlicingViewModel view = SmartSlicingViewModel::from_snapshot(snapshot);
        view.mode = m_view_model.mode;
        view.mode_id = m_view_model.mode_id;
        view.purpose = m_view_model.purpose;
        view.purpose_id = m_view_model.purpose_id;
        auto publish = m_callback_gate.guard([this, view = std::move(view)]() mutable {
            m_view_model = std::move(view);
            if (m_view_changed)
                m_view_changed(m_view_model);
        });
        if (m_dispatch)
            m_dispatch(std::move(publish));
        else
            publish();
    });
}

SmartSlicingPresenter::~SmartSlicingPresenter()
{
    m_callback_gate.close();
    m_coordinator.set_observer({});
}

void SmartSlicingPresenter::set_view_changed(ViewChangedFn view_changed)
{
    m_callback_gate.assert_owner_thread();
    m_view_changed = std::move(view_changed);
    if (m_view_changed)
        m_view_changed(m_view_model);
}

void SmartSlicingPresenter::publish_recommendation_snapshot(
    const AI::SmartSlicing::RecommendationSnapshot& recommendation)
{
    m_callback_gate.assert_owner_thread();
    AI::SmartSlicing::WorkflowSnapshot merged_snapshot = m_coordinator.snapshot();
    merged_snapshot.recommendation = recommendation;
    SmartSlicingViewModel view = SmartSlicingViewModel::from_snapshot(merged_snapshot);
    view.mode = m_view_model.mode;
    view.mode_id = m_view_model.mode_id;
    view.purpose = m_view_model.purpose;
    view.purpose_id = m_view_model.purpose_id;
    m_view_model = std::move(view);
    if (m_view_changed)
        m_view_changed(m_view_model);
}

void SmartSlicingPresenter::set_mode(SmartSlicingMode mode)
{
    m_callback_gate.assert_owner_thread();
    m_view_model.mode = mode;
    m_view_model.mode_id = mode == SmartSlicingMode::Orca ? "orca" : "ai";
    if (m_view_changed)
        m_view_changed(m_view_model);
}

void SmartSlicingPresenter::set_purpose(SmartSlicingPurpose purpose)
{
    m_callback_gate.assert_owner_thread();
    m_view_model.purpose = purpose;
    switch (purpose) {
    case SmartSlicingPurpose::Decoration: m_view_model.purpose_id = "decoration"; break;
    case SmartSlicingPurpose::Functional: m_view_model.purpose_id = "functional"; break;
    case SmartSlicingPurpose::General: m_view_model.purpose_id = "general"; break;
    }
    if (m_view_changed)
        m_view_changed(m_view_model);
}

} // namespace Slic3r::GUI
