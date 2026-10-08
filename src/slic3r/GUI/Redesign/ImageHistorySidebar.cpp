#include "ImageHistorySidebar.hpp"
#include "ImageHistoryPagination.hpp"
#include "RedesignWidgets.hpp"
#include "../AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "../AI/ModelGeneration/ModelLibraryThumbnail.hpp"
#include "RedesignMessageDialog.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace Slic3r::GUI {
namespace {
using namespace ModelGenerationPresentation;
using namespace RedesignTheme;
wxString tr(const char* value) { return wxString::FromUTF8(value); }

// The paged canvas paints only the current page, and the worker decodes only
// requested thumbnails. Library size never determines bitmap memory usage.
struct ImageHistoryWorker {
    struct Pixels {
        std::string id;
        int width {0}, height {0};
        std::vector<unsigned char> rgb, alpha;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stop {false};
    bool scan {false}, scanned {false}, failed {false};
    boost::filesystem::path root;
    std::vector<DesignHistoryEntry> entries;
    std::deque<DesignHistoryEntry> pending;
    std::vector<Pixels> ready;
    std::thread thread;

    explicit ImageHistoryWorker(boost::filesystem::path directory) : root(std::move(directory))
    {
        thread = std::thread([this] {
            ModelLibraryThumbnailCache cache;
            while (!stop) {
                bool scanning = false;
                DesignHistoryEntry entry;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock, [&] { return stop || scan || !pending.empty(); });
                    if (stop) return;
                    scanning = scan;
                    scan = false;
                    if (!scanning) { entry = std::move(pending.front()); pending.pop_front(); }
                }
                if (scanning) {
                    std::vector<DesignHistoryEntry> result;
                    bool error = false;
                    try { result = read_image_history(root); } catch (...) { error = true; }
                    std::lock_guard<std::mutex> lock(mutex);
                    entries = std::move(result); failed = error; scanned = true;
                } else {
                    Pixels pixels;
                    pixels.id = entry.job_id;
                    try {
                        const auto image = cache.load(entry.preview_path, {}, 320, stop);
                        if (image.IsOk()) {
                            pixels.width = image.GetWidth(); pixels.height = image.GetHeight();
                            const size_t count = size_t(pixels.width) * pixels.height;
                            pixels.rgb.assign(image.GetData(), image.GetData() + count * 3);
                            if (image.HasAlpha()) pixels.alpha.assign(image.GetAlpha(), image.GetAlpha() + count);
                        }
                    } catch (...) { /* Keep an unavailable tile; opening reports the failure. */ }
                    std::lock_guard<std::mutex> lock(mutex);
                    ready.push_back(std::move(pixels));
                }
            }
        });
    }
    ~ImageHistoryWorker()
    {
        stop = true; wake.notify_one();
        if (thread.joinable()) thread.join();
    }
};
}

struct ImageHistorySidebar::Impl {
    ImageHistorySidebar* owner;
    Open open;
    std::function<void()> layout_changed;
    ImageHistoryWorker worker {generated_models_root()};
    wxTimer timer;
    wxPanel* panel;
    RoundedActionButton* toggle;
    wxTextCtrl* search;
    wxPanel* canvas;
    wxPanel* page_label;
    RoundedActionButton *first_page, *previous_page, *next_page, *last_page;
    ImageHistoryPagination pagination;
    wxStaticText* status;
    std::vector<DesignHistoryEntry> entries;
    std::vector<size_t> filtered;
    std::map<std::string, wxBitmap> bitmaps;
    std::set<std::string> requested, hidden;
    std::string selected;
    int hovered {-1}, focused {-1};
    bool expanded {true}, busy {false}, loading {false};

    int dip(int value) const { return owner->FromDIP(value); }
    ImageHistoryGrid grid() const {
        const auto size = canvas->GetClientSize();
        return ImageHistoryGrid::fit(size.x, size.y, dip(12));
    }
    int edge() const { return grid().edge; }
    int grid_left() const { return std::max(0, (canvas->GetClientSize().x - 2 * edge() - dip(12)) / 2); }
    wxRect tile(size_t index) const {
        const int e = edge();
        const size_t local = index - pagination.begin();
        return {grid_left() + int(local % 2) * (e + dip(12)), int(local / 2) * (e + dip(12)), e, e};
    }
    wxRect trash(const wxRect& tile) const {
        return {tile.GetRight() - dip(32), tile.GetBottom() - dip(32), dip(28), dip(28)};
    }
    int hit(const wxPoint& position) const {
        const wxPoint point(position.x - grid_left(), position.y);
        const int e = edge() + dip(12);
        const int col = point.x / e, row = point.y / e;
        const size_t index = pagination.begin() + size_t(row) * 2 + col;
        return point.x >= 0 && point.y >= 0 && col < 2 && pagination.contains(index) && tile(index).Contains(position)
            ? int(index) : -1;
    }

    Impl(ImageHistorySidebar* parent, Open callback, std::function<void()> changed)
        : owner(parent), open(std::move(callback)), layout_changed(std::move(changed)), timer(parent)
    {
        owner->SetName("my-images-sidebar");
        owner->SetBackgroundColour(background_colour());
        auto* outer = new wxBoxSizer(wxHORIZONTAL);
        owner->SetSizer(outer);
        auto* rail = new wxBoxSizer(wxVERTICAL);
        rail->AddStretchSpacer();
        toggle = new RoundedActionButton(owner, tr("›"), false, 64);
        toggle->SetMinSize({dip(24), dip(64)});
        toggle->SetMaxSize({dip(24), dip(64)});
        toggle->SetName("my-images-collapse");
        toggle->SetToolTip(tr("收起我的图片"));
        toggle->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(toggle); dc.SetBackground(wxBrush(background_colour())); dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            const double w = toggle->GetClientSize().x, h = toggle->GetClientSize().y;
            auto shape = gc->CreatePath();
            shape.MoveToPoint(w, 0); shape.AddLineToPoint(w * .25, h * .28);
            shape.AddQuadCurveToPoint(0, h * .5, w * .25, h * .72);
            shape.AddLineToPoint(w, h); shape.CloseSubpath();
            gc->SetPen(*wxTRANSPARENT_PEN); gc->SetBrush(wxBrush(panel_colour())); gc->FillPath(shape);
            gc->SetPen(wxPen(secondary_text_colour(), std::max(1, dip(1))));
            for (int step = 0; step < 2; ++step) {
                const double x = w * .4 + dip(5) * step, direction = expanded ? 1 : -1;
                gc->StrokeLine(x, h / 2 - dip(4), x + direction * dip(3), h / 2);
                gc->StrokeLine(x + direction * dip(3), h / 2, x, h / 2 + dip(4));
            }
        });
        rail->Add(toggle);
        rail->AddStretchSpacer();
        outer->Add(rail, 0, wxEXPAND);
        panel = new RoundedPanel(owner, {dip(352), -1}, panel_colour(), background_colour(), 12);
        panel->SetMinSize({dip(352), -1});
        panel->SetBackgroundColour(panel_colour());
        outer->Add(panel, 1, wxEXPAND);
        auto* column = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(column);
        auto* title = new wxStaticText(panel, wxID_ANY, tr("我的图片"));
        style_text(title, primary_text_colour(), 14, true);
        column->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, dip(20));
        auto* search_box = new RoundedPanel(panel, {-1, dip(40)}, control_colour(), panel_colour(), 8);
        search_box->SetBackgroundColour(control_colour());
        auto* search_row = new wxBoxSizer(wxHORIZONTAL);
        search_box->SetSizer(search_row);
        auto* icon = new wxPanel(search_box, wxID_ANY, wxDefaultPosition, {dip(20), dip(22)});
        icon->SetBackgroundStyle(wxBG_STYLE_PAINT);
        icon->Bind(wxEVT_PAINT, [this, icon](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(icon); dc.SetBackground(wxBrush(control_colour())); dc.Clear();
            dc.SetPen(wxPen(secondary_text_colour(), dip(1))); dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawCircle(dip(8), dip(9), dip(5)); dc.DrawLine(dip(12), dip(13), dip(17), dip(18));
        });
        search_row->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, dip(12));
        search = new wxTextCtrl(search_box, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        search->SetName("my-images-search"); search->SetHint(tr("搜索图片"));
        search->SetBackgroundColour(control_colour()); style_text(search, primary_text_colour(), 10);
        search_row->Add(search, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, dip(8));
        column->Add(search_box, 0, wxEXPAND | wxALL, dip(20));
        status = new wxStaticText(panel, wxID_ANY, tr("正在读取历史图片…"));
        style_text(status, secondary_text_colour(), 10);
        column->Add(status, 0, wxLEFT | wxRIGHT | wxBOTTOM, dip(20));
        canvas = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE);
        canvas->SetName("my-images-grid");
        canvas->SetBackgroundColour(panel_colour()); canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
        canvas->SetMinSize({0, 0}); canvas->SetCanFocus(true);
        column->Add(canvas, 1, wxEXPAND | wxLEFT | wxRIGHT, dip(20));
        auto* footer = new wxPanel(panel, wxID_ANY);
        footer->SetBackgroundColour(panel_colour());
        auto* pager = new wxBoxSizer(wxHORIZONTAL);
        footer->SetSizer(pager);
        pager->AddStretchSpacer();
        auto add_page_button = [&](const char* name, const char* tooltip, int direction, bool double_arrow,
                                   std::function<void()> action) {
            auto* button = new RoundedActionButton(footer, tr(tooltip), false, 32);
            button->SetName(name); button->SetToolTip(tr(tooltip));
            button->SetMinSize({dip(32), dip(32)});
            button->SetMaxSize({dip(32), dip(32)});
            button->Bind(wxEVT_PAINT, [this, button, direction, double_arrow](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(button); dc.SetBackground(wxBrush(panel_colour())); dc.Clear();
                dc.SetPen(wxPen(button->IsEnabled() ? secondary_text_colour() : wxColour(75, 75, 80),
                                std::max(1, dip(1))));
                const auto size = button->GetClientSize();
                for (int i = 0; i < (double_arrow ? 2 : 1); ++i) {
                    const int x = size.x / 2 + (double_arrow ? dip(i == 0 ? -3 : 3) : 0);
                    dc.DrawLine(x - direction * dip(2), size.y / 2 - dip(4), x + direction * dip(2), size.y / 2);
                    dc.DrawLine(x + direction * dip(2), size.y / 2, x - direction * dip(2), size.y / 2 + dip(4));
                }
                if (button->HasFocus()) {
                    dc.SetBrush(*wxTRANSPARENT_BRUSH); dc.SetPen(wxPen(accent_colour(), std::max(1, dip(1))));
                    dc.DrawRoundedRectangle(dip(1), dip(1), size.x - dip(2), size.y - dip(2), dip(5));
                }
            });
            button->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
            pager->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, dip(4));
            return button;
        };
        first_page = add_page_button("my-images-first-page", "首页", -1, true, [this] { go_to_page(0); });
        previous_page = add_page_button("my-images-previous-page", "上一页", -1, false,
            [this] { if (pagination.page() > 0) go_to_page(pagination.page() - 1); });
        page_label = new wxPanel(footer, wxID_ANY, wxDefaultPosition, {dip(64), dip(32)});
        page_label->SetName("my-images-page-number");
        page_label->SetBackgroundStyle(wxBG_STYLE_PAINT);
        style_text(page_label, primary_text_colour(), 10);
        page_label->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(page_label); dc.SetBackground(wxBrush(panel_colour())); dc.Clear();
            dc.SetPen(wxPen(secondary_text_colour(), std::max(1, dip(1)))); dc.SetBrush(*wxTRANSPARENT_BRUSH);
            const auto size = page_label->GetClientSize();
            dc.DrawRoundedRectangle(dip(1), dip(1), size.x - dip(2), size.y - dip(2), dip(8));
            dc.SetFont(page_label->GetFont()); dc.SetTextForeground(primary_text_colour());
            dc.DrawLabel(page_label->GetLabel(), wxRect({0, 0}, size), wxALIGN_CENTER);
        });
        pager->Add(page_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, dip(8));
        next_page = add_page_button("my-images-next-page", "下一页", 1, false,
            [this] { go_to_page(pagination.page() + 1); });
        last_page = add_page_button("my-images-last-page", "末页", 1, true,
            [this] { go_to_page(pagination.pages() == 0 ? 0 : pagination.pages() - 1); });
        pager->AddStretchSpacer();
        column->Add(footer, 0, wxEXPAND | wxALL, dip(20));
        update_pager();
        toggle->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            expanded = !expanded; panel->Show(expanded);
            toggle->SetLabel(expanded ? tr("›") : tr("‹"));
            toggle->SetName(expanded ? "my-images-collapse" : "my-images-expand");
            toggle->SetToolTip(expanded ? tr("收起我的图片") : tr("展开我的图片"));
            owner->SetMinSize({dip(expanded ? 376 : 24), -1});
            owner->SetMaxSize({dip(expanded ? 376 : 24), -1});
            owner->GetParent()->Layout(); layout_changed();
            if (expanded) refresh();
        });
        search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { filter(true); });
        canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { resize(); event.Skip(); });
        canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        canvas->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            const int index = hit(event.GetPosition());
            if (index != hovered) {
                hovered = index; canvas->Refresh(false);
                if (index >= 0) {
                    const auto& entry = entries[filtered[index]];
                    canvas->SetToolTip(tr(entry.prompt.c_str()) + "\n" + style_label(entry.style));
                } else canvas->UnsetToolTip();
            }
        });
        canvas->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hovered = -1; canvas->Refresh(false); });
        canvas->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            const int index = hit(event.GetPosition());
            if (index < 0 || busy) return;
            focused = index; canvas->SetFocus();
            const bool remove = trash(tile(index)).Contains(event.GetPosition());
            activate(index, remove);
        });
        canvas->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (filtered.empty() || busy) { event.Skip(); return; }
            const int key = event.GetKeyCode();
            if (key == WXK_RETURN || key == WXK_SPACE || key == WXK_DELETE) {
                if (focused >= 0 && pagination.contains(size_t(focused))) activate(focused, key == WXK_DELETE);
            } else if (key == WXK_PAGEUP || key == WXK_PAGEDOWN || key == WXK_HOME || key == WXK_END) {
                if (key == WXK_HOME) go_to_page(0);
                else if (key == WXK_END) go_to_page(pagination.pages() - 1);
                else if (key == WXK_PAGEUP) go_to_page(pagination.page() == 0 ? 0 : pagination.page() - 1);
                else go_to_page(pagination.page() + 1);
            } else if (key == WXK_LEFT || key == WXK_RIGHT || key == WXK_UP || key == WXK_DOWN) {
                focused = focused < 0 ? int(pagination.begin()) :
                    std::clamp(focused + (key == WXK_LEFT ? -1 : key == WXK_RIGHT ? 1 : key == WXK_UP ? -2 : 2),
                    0, int(filtered.size()) - 1);
                pagination.go_to(size_t(focused) / pagination.capacity());
                hovered = -1; canvas->UnsetToolTip(); update_pager();
                canvas->Refresh(false);
            } else event.Skip();
        });
        owner->Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); }, timer.GetId());
        timer.Start(100);
        owner->SetMinSize({dip(376), -1});
        owner->SetMaxSize({dip(376), -1});
        refresh();
    }
    ~Impl() { timer.Stop(); }

    void refresh() {
        loading = true;
        { std::lock_guard<std::mutex> lock(worker.mutex); worker.scan = true; }
        worker.wake.notify_one();
    }
    void update_pager() {
        const size_t pages = pagination.pages();
        page_label->SetLabel(wxString::Format("%llu/%llu",
            static_cast<unsigned long long>(pages == 0 ? 0 : pagination.page() + 1),
            static_cast<unsigned long long>(pages)));
        page_label->Refresh(false);
        first_page->Enable(pagination.page() > 0); previous_page->Enable(pagination.page() > 0);
        next_page->Enable(pagination.page() + 1 < pages); last_page->Enable(pagination.page() + 1 < pages);
        for (auto* button : {first_page, previous_page, next_page, last_page}) button->Refresh(false);
    }
    void go_to_page(size_t page) {
        pagination.go_to(page);
        focused = pagination.begin() < pagination.end() ? int(pagination.begin()) : -1;
        hovered = -1; canvas->UnsetToolTip();
        update_pager(); canvas->Refresh(false);
    }
    void resize() {
        // Collapsing the drawer must not change its page through a zero size.
        if (!expanded || canvas->GetClientSize().x <= 0 || canvas->GetClientSize().y <= 0) return;
        pagination.set_capacity(size_t(grid().rows) * 2);
        if (focused >= 0 && !pagination.contains(size_t(focused))) focused = -1;
        hovered = -1; canvas->UnsetToolTip(); update_pager();
        canvas->Refresh(false);
    }
    void filter(bool reset_page) {
        filtered.clear();
        for (size_t i = 0; i < entries.size(); ++i)
            if (image_history_matches(entries[i], search->GetValue())) filtered.push_back(i);
        hovered = -1; focused = -1;
        pagination.set_count(filtered.size(), reset_page);
        status->SetLabel(entries.empty() ? tr("还没有保存的设计图") : tr("没有匹配的图片"));
        status->Show(filtered.empty()); panel->Layout(); resize();
        for (size_t i = pagination.begin(); i < pagination.end(); ++i)
            if (entries[filtered[i]].job_id == selected) focused = int(i);
        update_pager(); canvas->Refresh(false);
    }
    void poll() {
        std::vector<ImageHistoryWorker::Pixels> ready;
        bool scanned = false, failed = false;
        std::vector<DesignHistoryEntry> result;
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            ready.swap(worker.ready); scanned = worker.scanned; failed = worker.failed;
            if (scanned) { result = std::move(worker.entries); worker.scanned = false; }
        }
        if (scanned) {
            loading = false;
            if (failed) { status->SetLabel(tr("历史图片读取失败，收起后展开可重试")); status->Show(); panel->Layout(); }
            else {
                // A scan started before removal must not resurrect its old row.
                result.erase(std::remove_if(result.begin(), result.end(), [&](const auto& entry) {
                    return hidden.count(entry.job_id) > 0;
                }), result.end());
                entries = std::move(result);
                // Reopening the drawer can retry a thumbnail repaired on disk.
                for (auto it = bitmaps.begin(); it != bitmaps.end();) {
                    if (!it->second.IsOk()) { requested.erase(it->first); it = bitmaps.erase(it); }
                    else ++it;
                }
                filter(false);
            }
        }
        for (const auto& pixels : ready) {
            wxImage image;
            if (pixels.width > 0 && pixels.height > 0 && image.Create(pixels.width, pixels.height)) {
                std::memcpy(image.GetData(), pixels.rgb.data(), pixels.rgb.size());
                if (!pixels.alpha.empty()) { image.InitAlpha(); std::memcpy(image.GetAlpha(), pixels.alpha.data(), pixels.alpha.size()); }
            }
            bitmaps[pixels.id] = image.IsOk() ? wxBitmap(image) : wxBitmap();
        }
        if (!ready.empty()) canvas->Refresh(false);
    }
    void activate(int index, bool remove) {
        if (busy || index < 0 || size_t(index) >= filtered.size()) return;
        const auto id = entries[filtered[index]].job_id;
        if (remove) {
            RedesignMessageDialog confirm(owner, tr("从“我的图片”中移除此记录？\n原始图片、关联的 3D 模型和工程仍会保留。"),
                tr("移除图片记录"), wxYES_NO | wxICON_QUESTION);
            if (confirm.ShowModal() != wxID_YES) return;
            if (!hide_image_history_entry(worker.root, id)) {
                status->SetLabel(tr("移除失败，记录已保留，请稍后重试")); status->Show(); panel->Layout(); return;
            }
            hidden.insert(id);
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.job_id == id; }), entries.end());
            filter(false);
        } else if (open(id)) { selected = id; canvas->Refresh(false); }
        else { status->SetLabel(tr("图片无法打开，当前内容已保留")); status->Show(); panel->Layout(); }
    }
    void paint() {
        wxAutoBufferedPaintDC dc(canvas);
        dc.SetBackground(wxBrush(panel_colour())); dc.Clear();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        std::set<std::string> visible;
        for (size_t i = pagination.begin(); i < pagination.end(); ++i) {
            const auto rect = tile(i);
            const auto& entry = entries[filtered[i]];
            visible.insert(entry.job_id);
            gc->SetPen(wxPen(entry.job_id == selected ? accent_colour() : control_colour(), dip(1)));
            gc->SetBrush(wxBrush(control_colour()));
            gc->DrawRoundedRectangle(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2, dip(8));
            const auto bitmap = bitmaps.find(entry.job_id);
            if (bitmap != bitmaps.end() && bitmap->second.IsOk()) {
                const auto& image = bitmap->second;
                const double ratio = std::min(double(rect.width - dip(12)) / image.GetWidth(), double(rect.height - dip(12)) / image.GetHeight());
                const double w = image.GetWidth() * ratio, h = image.GetHeight() * ratio;
                gc->DrawBitmap(image, rect.x + (rect.width - w) / 2, rect.y + (rect.height - h) / 2, w, h);
            } else {
                dc.SetTextForeground(secondary_text_colour()); dc.SetFont(search->GetFont());
                dc.DrawLabel(bitmap == bitmaps.end() ? tr("加载中…") : tr("图片不可用"), rect, wxALIGN_CENTER);
            }
            if (requested.insert(entry.job_id).second) {
                std::lock_guard<std::mutex> lock(worker.mutex);
                worker.pending.push_back(entry); worker.wake.notify_one();
            }
            if (int(i) == hovered || (int(i) == focused && canvas->HasFocus())) {
                const auto box = trash(rect);
                gc->SetPen(*wxTRANSPARENT_PEN); gc->SetBrush(wxBrush(wxColour(45, 45, 49, 230)));
                gc->DrawRoundedRectangle(box.x, box.y, box.width, box.height, dip(5));
                gc->SetPen(wxPen(primary_text_colour(), dip(1))); gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRectangle(box.x + dip(9), box.y + dip(10), dip(10), dip(12));
                gc->StrokeLine(box.x + dip(7), box.y + dip(8), box.x + dip(21), box.y + dip(8));
                gc->StrokeLine(box.x + dip(11), box.y + dip(5), box.x + dip(17), box.y + dip(5));
                gc->StrokeLine(box.x + dip(12), box.y + dip(12), box.x + dip(12), box.y + dip(19));
                gc->StrokeLine(box.x + dip(16), box.y + dip(12), box.x + dip(16), box.y + dip(19));
            }
        }
        // Retain nearby bitmap results, but evict offscreen tiles in large libraries.
        for (auto it = bitmaps.begin(); bitmaps.size() > 64 && it != bitmaps.end();) {
            if (!visible.count(it->first)) { requested.erase(it->first); it = bitmaps.erase(it); }
            else ++it;
        }
    }
};

ImageHistorySidebar::ImageHistorySidebar(wxWindow* parent, Open open, std::function<void()> changed)
    : wxPanel(parent), m_impl(std::make_unique<Impl>(this, std::move(open), std::move(changed))) {}
ImageHistorySidebar::~ImageHistorySidebar() = default;
void ImageHistorySidebar::refresh_history() { m_impl->refresh(); }
void ImageHistorySidebar::set_busy(bool busy) { m_impl->busy = busy; }
void ImageHistorySidebar::set_selected(const std::string& id)
{
    if (m_impl->selected != id) { m_impl->selected = id; m_impl->canvas->Refresh(false); }
}

}
