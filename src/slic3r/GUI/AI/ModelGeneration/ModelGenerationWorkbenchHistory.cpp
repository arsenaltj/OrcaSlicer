#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "WorkbenchStyle.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/scrolwin.h>
#include <wx/notebook.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <cstring>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

wxWindow* ModelGenerationPanel::build_workbench_history(wxWindow* parent)
{
    auto* panel = m_workbench_history_panel = new WorkbenchPanel(parent);
    panel->SetMinSize(FromDIP(wxSize(260, 200)));
    panel->SetBackgroundColour(wxColour(32, 32, 34));
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(panel, wxID_ANY, _L("选择模型"));
    title->SetForegroundColour(wxColour(235, 235, 235));
    root->Add(title, 0, wxEXPAND | wxALL, FromDIP(12));
    auto* search = new TextInput(panel, wxEmptyString, wxEmptyString, "workbench_search");
    search->SetName("ai_content_color");
    search->SetBackgroundColor(StateColor(wxColour(49, 49, 54)));
    search->SetTextColor(StateColor(wxColour(235, 235, 235)));
    search->SetMinSize(FromDIP(wxSize(160, 32)));
    m_workbench_history_search = search->GetTextCtrl();
    m_workbench_history_search->SetForegroundColour(wxColour(235, 235, 235));
    m_workbench_history_search->SetHint(_L("搜索模型"));
    m_workbench_history_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refresh_workbench_history(); });
    auto* search_row = new wxBoxSizer(wxHORIZONTAL);
    search_row->Add(search, 1, wxALIGN_CENTER_VERTICAL);
    m_workbench_history_upload = workbench_button(panel, wxEmptyString);
    m_workbench_history_upload->SetIcon("workbench_upload");
    m_workbench_history_upload->SetName(_L("上传模型"));
    m_workbench_history_upload->SetToolTip(_L("上传模型"));
    m_workbench_history_upload->SetMinSize(FromDIP(wxSize(32, 32)));
    m_workbench_history_upload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (post_generation_ui_state().can_switch_version) choose_local_model();
    });
    search_row->Add(m_workbench_history_upload, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    root->Add(search_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    auto* tools = new wxBoxSizer(wxHORIZONTAL);
    const std::array<wxString, 4> labels {_L("全部模型"), _L("单色模型"), _L("多色模型"), _L("原始模型")};
    const std::array<wxString, 4> icons {"workbench_history_all", "workbench_history_monochrome",
        "workbench_history_multicolor", "workbench_history_original"};
    for (size_t index = 0; index < labels.size(); ++index) {
        auto* filter = m_workbench_history_filters[index] = workbench_button(panel, wxEmptyString);
        filter->SetName(labels[index]);
        filter->SetToolTip(labels[index]);
        filter->SetIcon(icons[index]);
        filter->SetMinSize(FromDIP(wxSize(32, 32)));
        filter->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            m_workbench_history_filter = int(index);
            m_workbench_history_page = 0;
            refresh_workbench_history();
        });
        tools->Add(filter, 0, wxRIGHT, FromDIP(6));
    }
    tools->AddStretchSpacer();
    auto* refresh = workbench_button(panel, _L("刷新"));
    refresh->SetIcon("refresh");
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load_library_entries(true); });
    refresh->SetLabel(wxEmptyString);
    refresh->SetToolTip(_L("刷新模型"));
    refresh->SetMinSize(FromDIP(wxSize(32, 32)));
    tools->Add(refresh, 0);
    root->Add(tools, 0, wxEXPAND | wxALL, FromDIP(12));
    m_workbench_history_status = new wxStaticText(panel, wxID_ANY, wxEmptyString);
    m_workbench_history_status->SetForegroundColour(wxColour(165, 165, 170));
    root->Add(m_workbench_history_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_workbench_history_scroller = new WorkbenchScrolledWindow(panel, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_workbench_history_scroller->SetScrollRate(0, FromDIP(12));
    m_workbench_history_scroller->SetBackgroundColour(panel->GetBackgroundColour());
    m_workbench_history_sizer = new wxBoxSizer(wxVERTICAL);
    m_workbench_history_scroller->SetSizer(m_workbench_history_sizer);
    root->Add(m_workbench_history_scroller, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    auto* pages = new wxBoxSizer(wxHORIZONTAL);
    pages->AddStretchSpacer();
    const auto page_button = [&](const wxString& icon, const wxString& label, auto action) {
        auto* button = workbench_button(panel, wxEmptyString);
        button->SetIcon(icon);
        button->SetName(label);
        button->SetToolTip(label);
        button->SetMinSize(FromDIP(wxSize(28, 28)));
        button->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent&) { action(); refresh_workbench_history(); });
        pages->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        return button;
    };
    m_workbench_history_first = page_button("workbench_page_edge", _L("首页"),
        [this] { m_workbench_history_page = 0; });
    m_workbench_history_previous = page_button("workbench_page_step", _L("上一页"),
        [this] { if (m_workbench_history_page) --m_workbench_history_page; });
    m_workbench_history_page_label = new wxStaticText(panel, wxID_ANY, "1 / 1");
    m_workbench_history_page_label->SetForegroundColour(wxColour(165, 165, 170));
    pages->Add(m_workbench_history_page_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
    m_workbench_history_next = page_button("workbench_page_step", _L("下一页"),
        [this] { ++m_workbench_history_page; });
    m_workbench_history_last = page_button("workbench_page_edge", _L("末页"),
        [this] { m_workbench_history_page = m_library_entries.size() / 10; });
    for (auto* previous : {m_workbench_history_previous, m_workbench_history_first}) {
        const wxString icon = previous == m_workbench_history_previous ? "workbench_page_step" : "workbench_page_edge";
        auto bitmap = create_scaled_bitmap(icon.ToStdString(), previous, 16).ConvertToImage();
        previous->SetIcon(wxBitmap(bitmap.Mirror()));
    }
    pages->AddStretchSpacer();
    root->Add(pages, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto* assets = workbench_button(panel, _L("打开历史资产"));
    assets->SetIcon("workbench_assets");
    assets->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_preview_book || m_preview_book->GetPageCount() < 2) return;
        auto* library = m_preview_book->GetPage(1);
        const auto label = m_preview_book->GetPageText(1);
        wxDialog dialog(m_workbench_shell, wxID_ANY, _L("历史资产"), wxDefaultPosition, wxDefaultSize,
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        dialog.SetName("ai_content_color");
        dialog.SetBackgroundColour(wxColour(32, 32, 35));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        dialog.SetSizer(layout);
        m_preview_book->RemovePage(1);
        library->Reparent(&dialog);
        layout->Add(library, 1, wxEXPAND | wxALL, FromDIP(12));
        auto* close = workbench_button(&dialog, _L("关闭"));
        close->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
        layout->Add(close, 0, wxALIGN_RIGHT | wxALL, FromDIP(12));
        WorkbenchAppearanceScope appearance(library);
        m_library_appearance_handler = [&appearance](wxWindow* card) { appearance.include(card); };
        dialog.SetClientSize(FromDIP(wxSize(900, 650)));
        dialog.CentreOnParent();
        library->Show();
        dialog.CallAfter([this] { load_library_entries(); });
        dialog.ShowModal();
        m_library_appearance_handler = {};
        layout->Detach(library);
        library->Reparent(m_preview_book);
        m_preview_book->InsertPage(1, library, label, false);
        refresh_workbench_history();
    });
    root->Add(assets, 0, wxEXPAND | wxALL, FromDIP(12));
    panel->SetSizer(root);
    panel->Hide();
    return panel;
}

void ModelGenerationPanel::refresh_workbench_history()
{
    if (!m_workbench_history_sizer || m_shutdown) return;
    m_workbench_history_sizer->Clear(true);
    const int scroll_y = m_workbench_history_scroller->GetViewStart().y;
    const bool can_switch = post_generation_ui_state().can_switch_version;
    const auto query = m_workbench_history_search->GetValue().Lower();
    std::vector<const GeneratedModelEntry*> entries;
    for (const auto& entry : m_library_entries) {
        if (entry.design_only || entry.model_path.empty() || !is_nonempty_model(entry.model_path) ||
            (!query.empty() && !entry.title.Lower().Contains(query))) continue;
        if ((m_workbench_history_filter == 1 && entry.palette.size() != 1) ||
            (m_workbench_history_filter == 2 && entry.palette.size() < 2) ||
            (m_workbench_history_filter == 3 && !entry.palette.empty())) continue;
        entries.push_back(&entry);
    }
    constexpr size_t page_size = 10;
    const size_t pages = std::max<size_t>(1, (entries.size() + page_size - 1) / page_size);
    m_workbench_history_page = std::min(m_workbench_history_page, pages - 1);
    m_workbench_history_page_label->SetLabel(wxString::Format("%llu / %llu",
        static_cast<unsigned long long>(m_workbench_history_page + 1), static_cast<unsigned long long>(pages)));
    m_workbench_history_first->Enable(m_workbench_history_page > 0);
    m_workbench_history_previous->Enable(m_workbench_history_page > 0);
    m_workbench_history_next->Enable(m_workbench_history_page + 1 < pages);
    m_workbench_history_last->Enable(m_workbench_history_page + 1 < pages);
    for (size_t index = 0; index < m_workbench_history_filters.size(); ++index)
        m_workbench_history_filters[index]->SetBackgroundColor(index == size_t(m_workbench_history_filter)
            ? wxColour(77, 77, 79) : wxColour(22, 22, 25));
    auto* grid = new wxGridSizer(2, FromDIP(8), FromDIP(8));
    const size_t begin = m_workbench_history_page * page_size;
    for (size_t index = begin; index < std::min(entries.size(), begin + page_size); ++index) {
        const auto& entry = *entries[index];
        const bool current = entry.model_path == m_displayed_model_path;
        auto* card = new WorkbenchPanel(m_workbench_history_scroller, true);
        card->set_selected(current);
        card->SetBackgroundColour(wxColour(22, 22, 25));
        card->SetMinSize(FromDIP(wxSize(112, 162)));
        auto* row = new wxBoxSizer(wxVERTICAL);
        wxBitmap thumbnail;
        const auto found = std::find_if(m_ui_history_entries.begin(), m_ui_history_entries.end(),
            [&](const ModelGenerationUIHistoryEntry& value) { return value.job_id == entry.job_id; });
        if (found != m_ui_history_entries.end() && found->thumbnail_width > 0 && found->thumbnail_height > 0 &&
            found->thumbnail_rgb.size() == size_t(found->thumbnail_width) * found->thumbnail_height * 3) {
            wxImage image(found->thumbnail_width, found->thumbnail_height);
            std::memcpy(image.GetData(), found->thumbnail_rgb.data(), found->thumbnail_rgb.size());
            if (found->thumbnail_alpha.size() == size_t(found->thumbnail_width) * found->thumbnail_height) {
                image.InitAlpha();
                std::memcpy(image.GetAlpha(), found->thumbnail_alpha.data(), found->thumbnail_alpha.size());
            }
            thumbnail = wxBitmap(image.Scale(FromDIP(96), FromDIP(96), wxIMAGE_QUALITY_HIGH));
        } else thumbnail = create_scaled_bitmap("workbench_object", card, 40);
        auto* visual = new wxStaticBitmap(card, wxID_ANY, thumbnail);
        visual->SetMinSize(FromDIP(wxSize(96, 96)));
        row->Add(visual, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(6));
        auto* name = new wxStaticText(card, wxID_ANY, entry.title, wxDefaultPosition, wxDefaultSize,
            wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        name->SetMinSize(FromDIP(wxSize(96, 18)));
        name->SetForegroundColour(wxColour(235, 235, 235));
        name->SetToolTip(entry.title + "\n" + entry.details);
        row->Add(name, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        auto* state = new wxStaticText(card, wxID_ANY, current ? _L("当前模型") :
            entry.accepted_finishing ? _L("已接受版本") : _L("历史模型"),
            wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        state->SetMinSize(FromDIP(wxSize(96, 22)));
        state->SetForegroundColour(current ? wxColour(255, 194, 39) : wxColour(165, 165, 170));
        row->Add(state, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        card->SetToolTip(entry.title + "\n" + entry.details);
        visual->SetToolTip(card->GetToolTipText());
        state->SetToolTip(card->GetToolTipText());
        card->SetSizer(row);
        const auto open = [this, entry](wxMouseEvent&) {
            if (!post_generation_ui_state().can_switch_version || entry.model_path == m_displayed_model_path) return;
            load_library_entry(entry.model_path, entry.reference_image_path, entry.ai_image_path,
                entry.palette, entry.palette_roles, entry.use_printable_colors, entry.color_intent_path,
                entry.color_intent_schema, entry.color_intent_sha256, entry.job_id, entry.title);
        };
        card->Bind(wxEVT_LEFT_UP, open);
        visual->Bind(wxEVT_LEFT_UP, open);
        name->Bind(wxEVT_LEFT_UP, open);
        state->Bind(wxEVT_LEFT_UP, open);
        card->Enable(can_switch || current);
        grid->Add(card, 0, wxEXPAND);
    }
    m_workbench_history_sizer->Add(grid, 0, wxEXPAND);
    m_workbench_history_status->SetLabel(m_ui_history_loading ? _L("正在读取模型…") :
        !m_ui_history_error.empty() ? from_u8(m_ui_history_error) :
        wxString::Format(_L("%llu 个模型"), static_cast<unsigned long long>(entries.size())));
    m_workbench_history_scroller->Layout();
    m_workbench_history_scroller->FitInside();
    m_workbench_history_scroller->Scroll(0, scroll_y);
}

} // namespace Slic3r::GUI
