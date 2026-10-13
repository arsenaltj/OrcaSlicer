#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "WorkbenchStyle.hpp"
#include "ModelGenerationPresentation.hpp"
#include "WorkbenchHistorySnapshot.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/scrolwin.h>
#include <wx/notebook.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/wupdlock.h>
#include <cstring>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
namespace {
class WorkbenchHistoryCard final : public wxPanel {
public:
    WorkbenchHistoryCard(wxWindow* parent, std::function<void(const ModelLibraryEntry&)> open)
        : wxPanel(parent)
    {
        SetBackgroundColour(wxColour(22, 22, 25));
        m_surface = new WorkbenchPanel(this, true);
        m_surface->SetBackgroundColour(GetBackgroundColour());
        auto* container = new wxBoxSizer(wxVERTICAL);
        container->Add(m_surface, 1, wxEXPAND);
        SetSizer(container);
        auto* row = new wxBoxSizer(wxVERTICAL);
        m_visual = new wxStaticBitmap(m_surface, wxID_ANY, create_scaled_bitmap("workbench_object", this, 40));
        row->Add(m_visual, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(6));
        m_name = new wxStaticText(m_surface, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
            wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        m_name->SetForegroundColour(wxColour(235, 235, 235));
        row->Add(m_name, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        m_state = new wxStaticText(m_surface, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
            wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        row->Add(m_state, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        m_surface->SetSizer(row);
        for (wxWindow* window : std::array<wxWindow*, 5>{{this, m_surface, m_visual, m_name, m_state}})
            window->Bind(wxEVT_LEFT_UP, [this, open](wxMouseEvent&) { open(m_entry); });
    }

    void update(const ModelLibraryEntry& entry, const ModelGenerationUIHistoryEntry* image,
                bool current, bool can_switch)
    {
        m_entry = entry; // Clicks always read the latest authoritative projection.
        for (wxWindow* window : std::array<wxWindow*, 5>{{this, m_surface, m_visual, m_name, m_state}})
            if (window->GetBackgroundColour() != wxColour(22, 22, 25)) window->SetBackgroundColour(wxColour(22, 22, 25));
        const auto size = FromDIP(wxSize(112, 162));
        const bool resized = GetMinSize() != size;
        if (resized) {
            SetMinSize(size);
            m_visual->SetMinSize(FromDIP(wxSize(96, 96)));
            m_name->SetMinSize(FromDIP(wxSize(96, 18)));
            m_state->SetMinSize(FromDIP(wxSize(96, 22)));
        }
        if (m_name->GetLabel() != entry.title) m_name->SetLabel(entry.title);
        const auto label = current ? _L("当前模型") : entry.accepted_finishing ? _L("已接受版本") : _L("历史模型");
        if (m_state->GetLabel() != label) m_state->SetLabel(label);
        const auto state_colour = current ? wxColour(255, 194, 39) : wxColour(165, 165, 170);
        if (!m_initialized || m_current != current || m_state->GetForegroundColour() != state_colour) {
            m_surface->set_selected(current);
            m_state->SetForegroundColour(state_colour);
            m_current = current;
        }
        const bool enabled = can_switch || current;
        if (IsEnabled() != enabled) Enable(enabled);
        const auto tooltip = entry.title + "\n" + entry.details;
        if (GetToolTipText() != tooltip)
            for (wxWindow* window : std::array<wxWindow*, 5>{{this, m_surface, m_visual, m_name, m_state}}) window->SetToolTip(tooltip);
        if (image && image->thumbnail_ready && (!m_initialized || resized || m_thumbnail_revision != image->thumbnail_revision)) {
            if (image->thumbnail_width > 0 && image->thumbnail_height > 0 &&
                image->thumbnail_rgb.size() == size_t(image->thumbnail_width) * image->thumbnail_height * 3) {
                wxImage pixels(image->thumbnail_width, image->thumbnail_height);
                std::memcpy(pixels.GetData(), image->thumbnail_rgb.data(), image->thumbnail_rgb.size());
                if (image->thumbnail_alpha.size() == size_t(image->thumbnail_width) * image->thumbnail_height) {
                    pixels.InitAlpha();
                    std::memcpy(pixels.GetAlpha(), image->thumbnail_alpha.data(), image->thumbnail_alpha.size());
                }
                m_visual->SetBitmap(wxBitmap(pixels.Scale(FromDIP(96), FromDIP(96), wxIMAGE_QUALITY_BILINEAR)));
            } else m_visual->SetBitmap(create_scaled_bitmap("workbench_object", this, 40));
            m_thumbnail_revision = image->thumbnail_revision;
        }
        m_initialized = true;
    }
    void update_actions(bool can_switch, const boost::filesystem::path& displayed)
    {
        const bool current = m_entry.model_path == displayed;
        if (m_current != current) {
            m_surface->set_selected(current);
            m_state->SetLabel(current ? _L("当前模型") :
                m_entry.accepted_finishing ? _L("已接受版本") : _L("历史模型"));
            m_state->SetForegroundColour(current ? wxColour(255, 194, 39) : wxColour(165, 165, 170));
            m_current = current;
        }
        const bool enabled = can_switch || current;
        if (IsEnabled() != enabled) Enable(enabled);
    }
private:
    ModelLibraryEntry m_entry;
    WorkbenchPanel* m_surface;
    wxStaticBitmap* m_visual;
    wxStaticText *m_name, *m_state;
    uint64_t m_thumbnail_revision {0};
    bool m_initialized {false}, m_current {false};
};
}

void ModelGenerationPanel::update_workbench_history_actions()
{
    if (!m_workbench_history_upload) return;
    const bool allowed = can_replace_model_asset();
    if (m_workbench_history_upload->IsEnabled() != allowed) m_workbench_history_upload->Enable(allowed);
    for (const auto& pair : m_workbench_history_cards)
        static_cast<WorkbenchHistoryCard*>(pair.second)->update_actions(allowed, m_displayed_model_path);
}

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
    m_workbench_history_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        m_workbench_history_page = 0; refresh_workbench_history();
    });
    auto* search_row = new wxBoxSizer(wxHORIZONTAL);
    search_row->Add(search, 1, wxALIGN_CENTER_VERTICAL);
    m_workbench_history_upload = workbench_button(panel, wxEmptyString);
    m_workbench_history_upload->SetIcon("workbench_upload");
    m_workbench_history_upload->SetName(_L("上传模型"));
    m_workbench_history_upload->SetToolTip(_L("上传模型"));
    m_workbench_history_upload->SetMinSize(FromDIP(wxSize(32, 32)));
    m_workbench_history_upload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_workbench_import_feedback && can_replace_model_asset()) choose_local_model();
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
        filter->SetBackgroundColor(index == 0 ? wxColour(77, 77, 79) : wxColour(22, 22, 25));
        filter->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            m_workbench_history_filter = int(index);
            for (size_t i = 0; i < m_workbench_history_filters.size(); ++i)
                m_workbench_history_filters[i]->SetBackgroundColor(i == index ? wxColour(77, 77, 79) : wxColour(22, 22, 25));
            m_workbench_history_page = 0;
            refresh_workbench_history();
        });
        tools->Add(filter, 0, wxRIGHT, FromDIP(6));
    }
    tools->AddStretchSpacer();
    auto* refresh = workbench_button(panel, _L("刷新"));
    refresh->SetIcon("refresh");
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_library_loading(); load_library_entries(true); });
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
    m_workbench_history_grid = new wxGridSizer(2, FromDIP(8), FromDIP(8));
    m_workbench_history_sizer->Add(m_workbench_history_grid, 0, wxEXPAND);
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
        [this] { m_workbench_history_page = m_workbench_history_pages - 1; });
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
        if (m_workbench_import_feedback || m_workbench_import_running) return;
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
        request_library_thumbnails(true);
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
    const int scroll_y = m_workbench_history_scroller->GetViewStart().y;
    const bool can_switch = !m_workbench_import_feedback && can_replace_model_asset();
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
    m_workbench_history_pages = pages;
    m_workbench_history_page = std::min(m_workbench_history_page, pages - 1);
    m_workbench_history_page_label->SetLabel(wxString::Format("%llu / %llu",
        static_cast<unsigned long long>(m_workbench_history_page + 1), static_cast<unsigned long long>(pages)));
    m_workbench_history_first->Enable(m_workbench_history_page > 0);
    m_workbench_history_previous->Enable(m_workbench_history_page > 0);
    m_workbench_history_next->Enable(m_workbench_history_page + 1 < pages);
    m_workbench_history_last->Enable(m_workbench_history_page + 1 < pages);
    m_workbench_history_upload->Enable(can_switch);
    std::set<std::string> all_keys;
    for (const auto& entry : m_library_entries) all_keys.insert(workbench_history_key(entry));
    std::vector<std::string> visible;
    bool sizes_changed = false;
    const size_t begin = m_workbench_history_page * page_size;
    for (size_t index = begin; index < std::min(entries.size(), begin + page_size); ++index) {
        const auto& entry = *entries[index];
        const auto key = workbench_history_key(entry);
        visible.push_back(key);
        const bool current = entry.model_path == m_displayed_model_path;
        auto& cached = m_workbench_history_cards[key];
        if (!cached) {
            cached = new WorkbenchHistoryCard(m_workbench_history_scroller, [this](const auto& selected) {
                if (m_workbench_import_feedback || !can_replace_model_asset() || selected.model_path == m_displayed_model_path) return;
                load_library_entry(selected.model_path, selected.reference_image_path, selected.ai_image_path,
                    selected.palette, selected.palette_roles, selected.use_printable_colors, selected.color_intent_path,
                    selected.color_intent_schema, selected.color_intent_sha256, selected.job_id, selected.title);
            });
            cached->Hide();
        }
        const auto found = std::find_if(m_ui_history_entries.begin(), m_ui_history_entries.end(),
            [&](const auto& value) { return value.asset_key == key; });
        sizes_changed = sizes_changed || cached->GetMinSize() != FromDIP(wxSize(112, 162));
        static_cast<WorkbenchHistoryCard*>(cached)->update(entry,
            found == m_ui_history_entries.end() ? nullptr : &*found, current, can_switch);
    }
    if (visible != m_workbench_history_visible_keys || sizes_changed) {
        wxWindowUpdateLocker lock(m_workbench_history_scroller);
        m_workbench_history_grid->Clear(false);
        for (const auto& pair : m_workbench_history_cards) pair.second->Hide();
        for (const auto& key : visible) {
            auto* card = m_workbench_history_cards.at(key);
            card->Show(); m_workbench_history_grid->Add(card, 0, wxEXPAND);
        }
        m_workbench_history_visible_keys = std::move(visible);
        for (size_t i = 0; i < m_workbench_history_filters.size(); ++i)
            m_workbench_history_filters[i]->SetBackgroundColor(i == size_t(m_workbench_history_filter)
                ? wxColour(77, 77, 79) : wxColour(22, 22, 25));
        m_workbench_history_scroller->Layout();
        m_workbench_history_scroller->FitInside();
        m_workbench_history_scroller->Scroll(0, scroll_y);
    }
    for (auto it = m_workbench_history_cards.begin(); it != m_workbench_history_cards.end();) {
        if (!all_keys.count(it->first)) { it->second->Destroy(); it = m_workbench_history_cards.erase(it); }
        else ++it;
    }
    const auto status = m_ui_history_loading ? _L("正在读取模型…") :
        !m_ui_history_error.empty() ? from_u8(m_ui_history_error) :
        wxString::Format(_L("%llu 个模型"), static_cast<unsigned long long>(entries.size()));
    if (m_workbench_history_status->GetLabel() != status) m_workbench_history_status->SetLabel(status);
}

} // namespace Slic3r::GUI
