#pragma once
#include "ModelLibraryApplication.hpp"
#include "ModelLibraryLoader.hpp"
#include <wx/panel.h>
#include <wx/timer.h>

class wxListBox;
class wxTextCtrl;
class wxChoice;
class wxStaticText;

namespace Slic3r::GUI {
// A replaceable view over the application. It never creates the generation UI.
class ModelLibraryPanel final : public wxPanel {
public:
    ModelLibraryPanel(wxWindow* parent, boost::filesystem::path root, ModelLibraryApplication::Ports ports);
    ModelLibraryPanel(wxWindow* parent, std::shared_ptr<ModelLibraryApplication> application);
    ~ModelLibraryPanel() override;
    void refresh_assets();
    void shutdown();
private:
    void rebuild();
    void select();
    void command(ModelLibraryCommand command);
    void export_selected();
    std::shared_ptr<ModelLibraryApplication> m_application_owner;
    ModelLibraryApplication& m_application;
    std::unique_ptr<ModelLibraryLoader> m_loader;
    wxTimer m_timer;
    uint64_t m_request_revision {0};
    wxListBox* m_list;
    wxTextCtrl* m_search;
    wxChoice* m_category;
    wxStaticText* m_status;
    wxStaticText* m_details;
    std::vector<size_t> m_visible;
    std::string m_selected_job_id;
    bool m_stopped {false};
};
}
