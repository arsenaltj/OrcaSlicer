#include "AsyncPreviewImage.hpp"
#include <wx/weakref.h>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Slic3r::GUI::Presentation {
wxImage prepare_preview_image(const PreviewImageRequest& key, const std::function<bool()>& canceled)
{
    const auto started = std::chrono::steady_clock::now();
    const auto stopped = [&] { return canceled && canceled(); };
    if (stopped() || key.path.empty() || key.bounds.x <= 0 || key.bounds.y <= 0) return {};
    wxImage source;
    if (!source.LoadFile(wxString(boost::filesystem::path(key.path).wstring())) || stopped()) return {};
    const double sx = double(key.bounds.x) / source.GetWidth(), sy = double(key.bounds.y) / source.GetHeight();
    const double scale = key.cover ? std::max(sx, sy) : std::min(sx, sy);
    const int width = key.cover ? std::max(1, int(std::ceil(source.GetWidth() * scale))) :
        std::clamp(int(std::lround(source.GetWidth() * scale)), 1, key.bounds.x);
    const int height = key.cover ? std::max(1, int(std::ceil(source.GetHeight() * scale))) :
        std::clamp(int(std::lround(source.GetHeight() * scale)), 1, key.bounds.y);
    auto image = source.Scale(width, height, wxIMAGE_QUALITY_HIGH);
    if (!image.IsOk() || stopped()) return {};
    if (key.cover) {
        image = image.GetSubImage(wxRect(std::max(0, (width-key.bounds.x)/2),
            std::max(0, int(std::lround((height-key.bounds.y)*0.1))), key.bounds.x, key.bounds.y));
        if (!image.HasAlpha()) image.InitAlpha();
        const int radius = std::clamp(key.radius, 0, std::min(key.bounds.x, key.bounds.y)/2);
        auto* alpha = image.GetAlpha();
        for (int y=0; y<key.bounds.y; ++y) {
            if ((y & 63) == 0 && stopped()) return {};
            for (int x=0; x<key.bounds.x; ++x) {
                double dx=0, dy=0;
                if (x<radius) dx=radius-0.5-x;
                else if (x>=key.bounds.x-radius) dx=x-(key.bounds.x-radius-0.5);
                if (y<radius) dy=radius-0.5-y;
                else if (y>=key.bounds.y-radius) dy=y-(key.bounds.y-radius-0.5);
                if (dx>0 && dy>0 && dx*dx+dy*dy>radius*radius) alpha[y*key.bounds.x+x]=0;
            }
        }
    }
    const char* trace = std::getenv("ORCASLICER_UI_LATENCY_TRACE");
    if (trace && std::string(trace)=="1")
        BOOST_LOG_TRIVIAL(info) << "[UiImage] prepared width=" << key.bounds.x << " height=" << key.bounds.y
            << " elapsed_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    return image;
}

struct AsyncPreviewImage::State {
    wxWeakRef<wxWindow> owner;
    TaskRunner* tasks;
    TaskScope scope;
    std::optional<PreviewImageRequest> requested, installed;
    wxBitmap bitmap;
    bool active {true}, pending {false}, stopped {false};
};
AsyncPreviewImage::AsyncPreviewImage(wxWindow* owner, TaskRunner& tasks) : m_state(std::make_shared<State>())
{
    m_state->owner=owner; m_state->tasks=&tasks;
    std::weak_ptr<State> weak=m_state;
    owner->Bind(wxEVT_SHOW,[weak](wxShowEvent& event) {
        event.Skip();
        if (auto state=weak.lock()) {
            if (!event.IsShown()) { state->scope.cancel(); state->pending=false; return; }
            state->owner->CallAfter([weak] { if (auto current=weak.lock()) launch(current); });
        }
    });
}
AsyncPreviewImage::~AsyncPreviewImage() { m_state->stopped=true; m_state->scope.cancel(); }
const wxBitmap& AsyncPreviewImage::bitmap() const { return m_state->bitmap; }
void AsyncPreviewImage::request(PreviewImageRequest key)
{
    assert_gui_thread();
    auto& state=*m_state;
    if (state.requested && *state.requested==key && (state.pending || (state.installed && *state.installed==key))) return;
    state.scope.cancel(); state.pending=false; state.requested=std::move(key);
    if (state.requested->path.empty()) { state.bitmap={}; state.installed.reset(); return; }
    launch(m_state);
}
void AsyncPreviewImage::set_active(bool active)
{
    assert_gui_thread();
    if (m_state->active==active) { if (active) launch(m_state); return; }
    m_state->active=active;
    if (!active) { m_state->scope.cancel(); m_state->pending=false; }
    else launch(m_state);
}
void AsyncPreviewImage::launch(const std::shared_ptr<State>& state)
{
    if (state->stopped || !state->active || state->pending || !state->owner ||
        !state->owner->IsShownOnScreen() || !state->requested || state->requested->path.empty() ||
        state->requested->bounds.x<=0 || state->requested->bounds.y<=0 ||
        (state->installed && *state->installed==*state->requested)) return;
    const auto key=*state->requested;
    const auto task=state->scope.begin(key.path+key.revision);
    state->pending=true;
    const bool accepted=state->tasks->submit(task, [key](const TaskContext& context) {
        return prepare_preview_image(key, [&context] { return context.canceled(); });
    }, [state,task,key](TaskResult<wxImage> result) {
        if (state->stopped || !state->scope.owns(task)) return;
        state->pending=false;
        if (result.canceled || result.error || !result.value || !result.value->IsOk() ||
            !state->active || !state->owner || !state->owner->IsShownOnScreen() ||
            !state->requested || !(*state->requested==key)) return;
        wxBitmap bitmap(*result.value);
        if (!bitmap.IsOk()) {
            BOOST_LOG_TRIVIAL(warning) << "[UiImage] bitmap installation failed; retained previous bitmap";
            return;
        }
        state->bitmap=std::move(bitmap);
        state->installed=key;
        state->owner->Refresh(false);
    }, TaskPriority::Low);
    if (!accepted) { state->pending=false; BOOST_LOG_TRIVIAL(warning) << "[UiImage] queue full; retained previous bitmap"; }
}
}
