#include "OrcaBusinessAdapter.hpp"

#include "slic3r/GUI/Plater.hpp"

namespace Slic3r::GUI {

OrcaBusinessAdapter::OrcaBusinessAdapter(Plater* plater)
    : m_plater(plater)
{}

void OrcaBusinessAdapter::set_plater(Plater* plater)
{
    m_plater = plater;
}

bool OrcaBusinessAdapter::attached() const
{
    return m_plater != nullptr;
}

bool OrcaBusinessAdapter::can_start_slice() const
{
    return attached() && !m_plater->model().objects.empty() && !m_plater->is_background_process_slicing();
}

bool OrcaBusinessAdapter::can_export_gcode() const
{
    return attached() && !m_plater->model().objects.empty() &&
           !m_plater->is_background_process_slicing() && !m_plater->is_export_gcode_scheduled();
}

void OrcaBusinessAdapter::register_commands(RedesignCommandRegistry& registry) const
{
    if (!attached())
        return;

    registry.register_command(RedesignCommandId::NewProject, {}, [this] {
        (void)m_plater->new_project();
    });
    registry.register_command(RedesignCommandId::OpenProject, {}, [this] {
        m_plater->load_project();
    });
    registry.register_command(RedesignCommandId::SaveProject, {}, [this] {
        (void)m_plater->save_project(false);
    });
    registry.register_command(RedesignCommandId::SaveProjectAs, {}, [this] {
        (void)m_plater->save_project(true);
    });
    registry.register_command(RedesignCommandId::ImportModel,
                              [this] { return m_plater->can_add_model(); },
                              [this] { m_plater->add_file(); });
    registry.register_command(RedesignCommandId::DeleteSelection,
                              [this] { return m_plater->can_delete(); },
                              [this] { m_plater->remove_selected(); });
    registry.register_command(RedesignCommandId::Undo,
                              [this] { return m_plater->can_undo(); },
                              [this] { m_plater->undo(); });
    registry.register_command(RedesignCommandId::Redo,
                              [this] { return m_plater->can_redo(); },
                              [this] { m_plater->redo(); });
    registry.register_command(RedesignCommandId::StartSlice,
                              [this] { return can_start_slice(); },
                              [this] { m_plater->reslice(); });
    registry.register_command(RedesignCommandId::ExportGcode,
                              [this] { return can_export_gcode(); },
                              [this] { m_plater->export_gcode(false); });
}

RedesignStateSnapshot OrcaBusinessAdapter::snapshot() const
{
    RedesignStateSnapshot state;
    state.project_open = attached();
    if (!attached())
        return state;

    state.project_dirty = m_plater->is_project_dirty();
    state.has_selection = !m_plater->get_selection().is_empty();
    state.can_undo = m_plater->can_undo();
    state.can_redo = m_plater->can_redo();
    if (m_plater->is_background_process_slicing())
        state.slice_status = RedesignSliceStatus::Slicing;
    else if (m_plater->model().objects.empty())
        state.slice_status = RedesignSliceStatus::Idle;
    else if (state.project_dirty)
        state.slice_status = RedesignSliceStatus::Dirty;
    else
        state.slice_status = RedesignSliceStatus::Idle;
    return state;
}

void OrcaBusinessAdapter::publish_state(RedesignStateStore& store) const
{
    store.publish(snapshot());
}

}
