#pragma once

#include <wx/panel.h>

class wxBoxSizer;
class wxWindow;

namespace Slic3r::GUI {

class RedesignShell final : public wxPanel
{
public:
    explicit RedesignShell(wxWindow* parent);

    void attach_legacy_content(wxWindow* content);
    bool has_legacy_content() const;

private:
    wxBoxSizer* m_sizer { nullptr };
    wxWindow* m_legacy_content { nullptr };
};

}
