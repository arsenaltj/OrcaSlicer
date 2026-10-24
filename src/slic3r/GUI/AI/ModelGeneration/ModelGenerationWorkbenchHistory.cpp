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
#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/wupdlock.h>
#include <cstring>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
namespace {
class WorkbenchHistoryThumbnail final : public wxPanel {
public:
    explicit WorkbenchHistoryThumbnail(wxWindow* parent) : wxPanel(parent)
    {
        SetBackgroundColour(wxColour(22, 22, 25));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
            const wxBitmap& bitmap = m_bitmap.IsOk() ? m_bitmap : m_fallback;
            if (bitmap.IsOk())
                dc.DrawBitmap(bitmap, (GetClientSize().x - bitmap.GetWidth()) / 2,
                              (GetClientSize().y - bitmap.GetHeight()) / 2, true);
        });
        rescale();
    }

    void set_image(const wxImage& image)
    {
        m_source = image.IsOk() ? image.Copy() : wxImage();
        rebuild_bitmap();
    }

    void rescale()
    {
        const wxSize extent = FromDIP(wxSize(96, 96));
        SetMinSize(extent);
        SetMaxSize(extent);
        m_fallback = create_scaled_bitmap("workbench_object", this, 40);
        rebuild_bitmap();
    }

private:
    void rebuild_bitmap()
    {
        m_bitmap = wxBitmap();
        if (m_source.IsOk()) {
            const wxSize target = FromDIP(wxSize(96, 96));
            const wxSize fitted = fit_workbench_thumbnail_size(
                m_source.GetWidth(), m_source.GetHeight(), target.x, target.y);
            if (fitted.x > 0 && fitted.y > 0)
                m_bitmap = wxBitmap(m_source.Scale(fitted.x, fitted.y, wxIMAGE_QUALITY_BILINEAR));
        }
        Refresh(false);
    }

    wxBitmap m_bitmap, m_fallback;
    wxImage m_source;
};

class WorkbenchHistoryCard final : public wxPanel {
public:
    WorkbenchHistoryCard(wxWindow* parent, std::function<void(const std::string&)> open,
                         std::function<void(const std::string&, bool)> action)
        : wxPanel(parent)
    {
        SetCanFocus(true);
        SetBackgroundColour(wxColour(22, 22, 25));
        m_surface = new WorkbenchPanel(this, true);
        m_surface->SetBackgroundColour(GetBackgroundColour());
        auto* container = new wxBoxSizer(wxVERTICAL);
        container->Add(m_surface, 1, wxEXPAND);
        SetSizer(container);
        auto* row = new wxBoxSizer(wxVERTICAL);
        m_visual = new WorkbenchHistoryThumbnail(m_surface);
        row->Add(m_visual, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(6));
        m_name = new wxStaticText(m_surface, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
            wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        m_name->SetForegroundColour(wxColour(235, 235, 235));
        row->Add(m_name, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        m_state = new wxStaticText(m_surface, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
            wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        row->Add(m_state, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        m_surface->SetSizer(row);
        for (wxWindow* window : std::array<wxWindow*, 5>{{this, m_surface, m_visual, m_name, m_state}}) {
            window->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
                SetFocus();
                event.Skip();
            });
            window->Bind(wxEVT_LEFT_UP, [this, open](wxMouseEvent&) { open(m_key); });
            window->Bind(wxEVT_CONTEXT_MENU, [this, action](wxContextMenuEvent& event) {
                const std::string key = m_key;
                if (key.empty()) return;
                wxMenu menu;
                const int export_id = wxWindow::NewControlId();
                const int remove_id = wxWindow::NewControlId();
                menu.Append(export_id, _L("导出模型"));
                menu.AppendSeparator();
                menu.Append(remove_id, _L("删除模型"));
                menu.Bind(wxEVT_MENU, [action, key, remove_id](wxCommandEvent& command) {
                    action(key, command.GetId() == remove_id);
                });
                wxPoint position = event.GetPosition();
                position = position == wxDefaultPosition ? wxPoint(GetClientSize().x / 2, GetClientSize().y / 2)
                                                        : ScreenToClient(position);
                PopupMenu(&menu, position);
            });
        }
    }

    void update(const ModelLibraryEntry& entry, const ModelGenerationUIHistoryEntry* image,
                bool current, bool can_switch)
    {
        m_entry = entry;
        m_key = workbench_history_key(entry);
        for (wxWindow* window : std::array<wxWindow*, 5>{{this, m_surface, m_visual, m_name, m_state}})
            if (window->GetBackgroundColour() != wxColour(22, 22, 25)) window->SetBackgroundColour(wxColour(22, 22, 25));
        const auto size = FromDIP(wxSize(112, 162));
        const bool resized = GetMinSize() != size;
        if (resized) {
            SetMinSize(size);
            m_visual->rescale();
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
                m_visual->set_image(pixels);
            } else m_visual->set_image(wxImage());
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
    std::string m_key;
    WorkbenchPanel* m_surface;
    WorkbenchHistoryThumbnail* m_visual;
    wxStaticText *m_name, *m_state;
    uint64_t m_thumbnail_revision {0};
    bool m_initialized {false}, m_current {false};
};

std::vector<const ModelLibraryEntry*> filtered_workbench_history_entries(
    const std::vector<ModelLibraryEntry>& library, const wxString& query, int filter)
{
    std::vector<const ModelLibraryEntry*> entries;
    for (const auto& entry : library) {
        if (entry.design_only || entry.model_path.empty() || !is_nonempty_model(entry.model_path) ||
            (!query.empty() && !entry.title.Lower().Contains(query))) continue;
        if ((filter == 1 && entry.palette.size() != 1) ||
            (filter == 2 && entry.palette.size() < 2) ||
            (filter == 3 && !entry.palette.empty())) continue;
        entries.push_back(&entry);
    }
    return entries;
}
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
    root->AddSpacer(FromDIP(12));
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
    const auto entries = filtered_workbench_history_entries(
        m_library_entries, query, m_workbench_history_filter);
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
            cached = new WorkbenchHistoryCard(m_workbench_history_scroller,
                [this](const auto& selected_key) { open_workbench_history_entry(selected_key); },
                [this](const auto& selected_key, bool remove) {
                    if (remove) delete_workbench_history_entry(selected_key);
                    else export_workbench_history_entry(selected_key);
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
        !m_ui_history_error.empty() ? wxString::FromUTF8(m_ui_history_error.c_str()) :
        wxString::Format(_L("%llu 个模型"), static_cast<unsigned long long>(entries.size()));
    if (m_workbench_history_status->GetLabel() != status) m_workbench_history_status->SetLabel(status);
}

void ModelGenerationPanel::open_workbench_history_entry(const std::string& key)
{
    if (m_workbench_import_feedback || !can_replace_model_asset()) return;
    const auto found = std::find_if(m_library_entries.begin(), m_library_entries.end(),
        [&](const auto& entry) { return workbench_history_key(entry) == key; });
    if (found == m_library_entries.end()) {
        m_workbench_history_status->SetLabel(_L("模型列表已变化，请刷新后重试。"));
        return;
    }
    if (found->model_path == m_displayed_model_path) return;
    load_library_entry(found->model_path, found->reference_image_path, found->ai_image_path,
        found->palette, found->palette_roles, found->use_printable_colors, found->color_intent_path,
        found->color_intent_schema, found->color_intent_sha256, found->job_id, found->title);
}

void ModelGenerationPanel::export_workbench_history_entry(const std::string& key)
{
    if (m_workbench_import_feedback || !can_replace_model_asset()) return;
    const auto found = std::find_if(m_library_entries.begin(), m_library_entries.end(),
        [&](const auto& entry) { return workbench_history_key(entry) == key; });
    if (found == m_library_entries.end()) {
        m_workbench_history_status->SetLabel(_L("模型列表已变化，旧菜单操作已取消。"));
        return;
    }
    export_library_entry(*found);
}

void ModelGenerationPanel::delete_workbench_history_entry(const std::string& key)
{
    if (m_workbench_import_feedback || !can_replace_model_asset()) return;
    const auto found = std::find_if(m_library_entries.begin(), m_library_entries.end(),
        [&](const auto& entry) { return workbench_history_key(entry) == key; });
    if (found == m_library_entries.end()) {
        m_workbench_history_status->SetLabel(_L("模型列表已变化，旧菜单操作已取消。"));
        return;
    }

    const bool current = found->model_path == m_displayed_model_path;
    std::optional<GeneratedModelEntry> fallback;
    if (current) {
        const auto filtered = filtered_workbench_history_entries(
            m_library_entries, m_workbench_history_search->GetValue().Lower(), m_workbench_history_filter);
        std::vector<std::string> ordered_keys;
        ordered_keys.reserve(filtered.size());
        for (const auto* entry : filtered) ordered_keys.push_back(workbench_history_key(*entry));
        if (const auto fallback_key = workbench_history_fallback_key(ordered_keys, key)) {
            const auto neighbor = std::find_if(m_library_entries.begin(), m_library_entries.end(),
                [&](const auto& entry) { return workbench_history_key(entry) == *fallback_key; });
            if (neighbor != m_library_entries.end()) fallback = *neighbor;
        }
    }

    const GeneratedModelEntry selected = *found;
    if (!delete_library_entry(selected) || !current || !fallback) return;
    load_library_entry(fallback->model_path, fallback->reference_image_path, fallback->ai_image_path,
        fallback->palette, fallback->palette_roles, fallback->use_printable_colors, fallback->color_intent_path,
        fallback->color_intent_schema, fallback->color_intent_sha256, fallback->job_id, fallback->title);
}

} // namespace Slic3r::GUI
