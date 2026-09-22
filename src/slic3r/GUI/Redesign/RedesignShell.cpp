#include "RedesignShell.hpp"

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/window.h>

namespace Slic3r::GUI {

RedesignShell::RedesignShell(wxWindow* parent)
    : wxPanel(parent)
    , m_sizer(new wxBoxSizer(wxVERTICAL))
{
    SetSizer(m_sizer);
}

void RedesignShell::attach_legacy_content(wxWindow* content)
{
    if (content == nullptr || content == m_legacy_content)
        return;

    if (m_legacy_content != nullptr)
        m_sizer->Detach(m_legacy_content);

    content->Reparent(this);
    m_legacy_content = content;
    m_sizer->Add(m_legacy_content, 1, wxEXPAND);
    Layout();
}

bool RedesignShell::has_legacy_content() const
{
    return m_legacy_content != nullptr;
}

}
