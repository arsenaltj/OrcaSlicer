#pragma once

#include <wx/panel.h>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r::GUI {

// A local image library, independent of the model/workbench library.
class ImageHistorySidebar final : public wxPanel
{
public:
    using Open = std::function<bool(const std::string&)>;
    ImageHistorySidebar(wxWindow* parent, Open open, std::function<void()> layout_changed);
    ~ImageHistorySidebar() override;
    void refresh_history();
    void set_busy(bool busy);
    void set_selected(const std::string& job_id);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
