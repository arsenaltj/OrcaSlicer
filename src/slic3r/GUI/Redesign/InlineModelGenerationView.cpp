#include "InlineModelGenerationView.hpp"
#include <boost/log/trivial.hpp>
#include "RedesignPresentationDeadline.hpp"
#include "RedesignWidgets.hpp"
#include "libslic3r/Utils.hpp"
#include "SmartCreationControls.hpp"
#include "ModelGenerationStarfieldRenderer.hpp"
#include "../AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "../AI/ModelGeneration/ModelPreview3D.hpp"
#include "../Presentation/UiRuntime.hpp"
#include "../GUI_App.hpp"
#include "../wxExtensions.hpp"
#include "../Widgets/Label.hpp"
#include <wx/dcbuffer.h>
#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/toplevel.h>
#include <wx/wupdlock.h>
#include <wx/weakref.h>
#include <atomic>

namespace Slic3r::GUI {
using namespace RedesignTheme;
namespace {
wxString t(const char* value) { return wxString::FromUTF8(value); }
double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class GenerationFeedback final : public RedesignPaintPanel {
public:
    explicit GenerationFeedback(wxWindow* parent) : RedesignPaintPanel(parent) {
        SetBackgroundColour(background_colour()); SetBackgroundStyle(wxBG_STYLE_PAINT);
        paint_owns_background();
        SetWindowStyleFlag(GetWindowStyleFlag() | wxCLIP_CHILDREN);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    }
    wxString title, detail;
    int progress {-1};
    bool animate {false};
    double elapsed {0};
private:
    void paint() {
        wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(background_colour())); dc.Clear();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        const auto size = GetClientSize();
        const int width = std::max(1, std::min(FromDIP(334), size.x - FromDIP(32)));
        wxString heading, description;
        dc.SetFont(smart_creation_font(16, wxFONTWEIGHT_MEDIUM));
        const auto title_size = Label::split_lines(dc, width, title, heading);
        dc.SetFont(smart_creation_font(14));
        const auto detail_size = Label::split_lines(dc, width, detail, description);
        const int row_height = FromDIP(24);
        const int height = title_size.y + FromDIP(8) + detail_size.y + FromDIP(16) + (animate ? row_height : 0);
        const int top = std::max(FromDIP(8), (size.y - height) / 2);
        const int left = (size.x - width) / 2;
        if (animate) draw_model_generation_starfield(*gc, m_stars, elapsed, size,
            wxRect(left - FromDIP(12), top - FromDIP(12), width + FromDIP(24), height + FromDIP(24)),
            double(FromDIP(100)) / 100.0);
        dc.SetFont(smart_creation_font(16, wxFONTWEIGHT_MEDIUM));
        dc.SetTextForeground(smart_creation_opaque_colour(wxColour(255,255,255,230), background_colour()));
        dc.DrawLabel(heading, wxRect(left, top, width, title_size.y), wxALIGN_CENTER_HORIZONTAL);
        dc.SetFont(smart_creation_font(14));
        dc.SetTextForeground(smart_creation_opaque_colour(wxColour(255,255,255,153), background_colour()));
        dc.DrawLabel(description, wxRect(left, top + title_size.y + FromDIP(8), width, detail_size.y), wxALIGN_CENTER_HORIZONTAL);
        if (!animate) return;
        const int bar_width = std::max(1, width - FromDIP(46));
        const int bar_y = top + height - row_height + FromDIP(8);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(smart_creation_opaque_colour(wxColour(255,255,255,26), background_colour())));
        gc->DrawRoundedRectangle(left, bar_y, bar_width, FromDIP(8), FromDIP(4));
        gc->SetBrush(wxBrush(wxColour(255,194,39)));
        const double fill = progress >= 0 ? bar_width * std::clamp(progress, 0, 100) / 100.0 : bar_width / 4.0;
        const double offset = progress >= 0 ? 0 : (bar_width - fill) * (1.0 - std::cos(elapsed * 2.0)) / 2.0;
        if (fill > 0) gc->DrawRoundedRectangle(left + offset, bar_y, fill, FromDIP(8), std::min(fill / 2, double(FromDIP(4))));
        dc.SetFont(smart_creation_font(16, wxFONTWEIGHT_MEDIUM));
        dc.SetTextForeground(smart_creation_opaque_colour(wxColour(255,255,255,219), background_colour()));
        dc.DrawLabel(progress >= 0 ? wxString::Format("%d%%", progress) : t("…"),
            wxRect(left + bar_width + FromDIP(10), top + height - row_height, FromDIP(36), row_height), wxALIGN_CENTER_VERTICAL);
    }
    ModelGenerationStarfield m_stars;
};
}

struct InlineModelGenerationView::Impl {
    struct Preparation {
        std::atomic<bool> canceled {false}, done {false};
        std::uint64_t generation {0};
        WorkbenchOpenRequest request;
        ModelPreview3D::PreparedModel prepared;
        std::string error;
    };
    InlineModelGenerationView& view;
    InlineModelCallbacks callbacks;
    InlineModelGenerationState state;
    GenerationFeedback* feedback;
    wxFrame* first_frame_cover {nullptr};
    GenerationFeedback* retained_feedback {nullptr};
    bool cover_requested {false};
    RoundedActionButton* recover;
    ModelPreview3D* preview {nullptr};
    std::shared_ptr<Preparation> preparation;
    Presentation::TaskScope preparation_scope;
    Presentation::RefreshScheduler refresh;
    std::shared_ptr<int> lifetime {std::make_shared<int>(0)};
    wxTimer timer;
    RedesignPresentationDeadline deadline;
    ModelGenerationAnimationClock animation;
    ModelGenerationPresentation::DesignGenerationWait design_wait;
    std::string error;
    bool presented {false}, closing {false};

    Impl(InlineModelGenerationView& owner, InlineModelCallbacks cb)
        : view(owner), callbacks(std::move(cb)), feedback(new GenerationFeedback(&owner)),
          recover(new RoundedActionButton(feedback, t("恢复当前任务"), false, 36)), timer(&owner) {
        view.SetMinSize(wxSize(0,0)); view.SetBackgroundColour(background_colour());
        refresh.configure([this] { return visible(); }, [this](Presentation::Dirty) { render(); tick(); }, "inline-generation");
        view.SetName("inline-model-generation");
        recover->SetName("inline-generation-recover"); recover->Hide();
        recover->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (state.phase() == InlinePreviewPhase::Failed) view.retry_preview();
            else if (callbacks.recover) callbacks.recover();
        });
        view.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); }, timer.GetId());
        view.Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { layout(); event.Skip(); });
        wxWeakRef<InlineModelGenerationView> weak(&view);
        auto visibility = [weak] {
            if (weak) weak->CallAfter([weak] { if (weak && !weak->m_impl->closing) weak->m_impl->tick(); });
        };
        for (auto* ancestor = static_cast<wxWindow*>(&view); ancestor; ancestor = ancestor->GetParent())
            ancestor->Bind(wxEVT_SHOW, [visibility](wxShowEvent& event) { event.Skip(); visibility(); });
        if (auto* top = wxDynamicCast(wxGetTopLevelParent(&view), wxTopLevelWindow))
            top->Bind(wxEVT_ICONIZE, [visibility](wxIconizeEvent& event) { event.Skip(); visibility(); });
    }
    bool visible() const {
        auto* top = wxDynamicCast(wxGetTopLevelParent(&view), wxTopLevelWindow);
        return presented && view.IsShownOnScreen() && (!top || !top->IsIconized());
    }
    void shutdown() {
        if (closing) return;
        closing = true; timer.Stop(); refresh.stop(); lifetime.reset();
        preparation_scope.cancel();
        if (preparation) preparation->canceled = true;
        if (preview) preview->observe_presentation_frame({});
        if (first_frame_cover) { first_frame_cover->Hide(); first_frame_cover->Destroy(); first_frame_cover = nullptr; }
        preparation.reset(); callbacks = {};
    }
    void changed() { if (callbacks.changed) callbacks.changed(); }
    void sync_first_frame_cover() {
        // Like the workbench's retained source, a separate non-activating native
        // window keeps the feedback visible without clipping WM_PAINT on GL.
        // It owns no task state and exists only for this local first-frame handoff.
        const bool show = cover_requested && state.phase() != InlinePreviewPhase::Failed && visible();
        if (!show) {
            if (first_frame_cover) first_frame_cover->Hide();
            return;
        }
        if (!first_frame_cover) {
            first_frame_cover = new wxFrame(wxGetTopLevelParent(&view), wxID_ANY, wxEmptyString,
                wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT);
            first_frame_cover->SetName("inline-first-frame-feedback");
            first_frame_cover->SetBackgroundColour(background_colour());
            auto* sizer = new wxBoxSizer(wxVERTICAL);
            first_frame_cover->SetSizer(sizer);
            retained_feedback = new GenerationFeedback(first_frame_cover);
            sizer->Add(retained_feedback, 1, wxEXPAND);
            first_frame_cover->Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent& event) { event.Veto(); });
        }
        const wxRect bounds(view.ClientToScreen(wxPoint()), view.GetClientSize());
        if (bounds.IsEmpty()) return;
        if (first_frame_cover->GetRect() != bounds) {
            first_frame_cover->SetSize(bounds); first_frame_cover->Layout();
        }
        retained_feedback->title = feedback->title; retained_feedback->detail = feedback->detail;
        retained_feedback->progress = feedback->progress; retained_feedback->animate = feedback->animate;
        retained_feedback->elapsed = feedback->elapsed;
        const bool reveal = !first_frame_cover->IsShown();
        if (reveal) first_frame_cover->ShowWithoutActivating();
        retained_feedback->Refresh(false);
        if (reveal) retained_feedback->Update(); // One handoff barrier, never an ordinary progress update.
    }
    void layout() {
        const auto size = view.GetClientSize();
        if (preview && preview->GetRect() != view.GetClientRect()) preview->SetSize(view.GetClientRect());
        const bool installed = state.phase() == InlinePreviewPhase::AwaitingFirstFrame || state.ready();
        // The GL viewport keeps its final size while the small feedback overlays it.
        const auto bounds = installed ? wxRect(0, std::max(0, size.y - view.FromDIP(110)), size.x, view.FromDIP(100)) : view.GetClientRect();
        if (feedback->GetRect() != bounds) feedback->SetSize(bounds);
        // Keep the action inside the painted surface so its buffered redraws
        // cannot cover a sibling recovery button on Windows.
        const auto feedback_size = feedback->GetClientSize();
        const wxRect action(std::max(0, (feedback_size.x - view.FromDIP(190)) / 2),
            std::max(0, feedback_size.y / 2 + view.FromDIP(110)), view.FromDIP(190), view.FromDIP(36));
        if (recover->GetRect() != action) recover->SetSize(action);
        sync_first_frame_cover();
    }
    void render() {
        const auto& snapshot = state.snapshot();
        if (snapshot.stage != ModelGenerationUIStage::GeneratingDesign) design_wait.clear();
        feedback->animate = inline_generation_active(snapshot) || inline_generation_handoff(snapshot) ||
            (state.artifact() && !state.ready() && state.phase() != InlinePreviewPhase::Failed);
        feedback->progress = -1;
        feedback->detail = t(snapshot.status_text.c_str());
        if (state.phase() == InlinePreviewPhase::Failed) {
            feedback->title = t("3D模型预览加载失败");
            feedback->detail = state.binding().source == CreationSource::Advanced ?
                t("模型文件暂时无法预览，请保留任务信息并按原入口重新加载。") : t(error.c_str());
        } else if (state.artifact() && !state.ready()) {
            feedback->title = t("正在加载3D模型");
            feedback->detail = state.phase() == InlinePreviewPhase::AwaitingFirstFrame ?
                t("正在显示模型预览") : t("正在解析本地模型文件");
        } else switch (snapshot.stage) {
        case ModelGenerationUIStage::DesignReady:
        case ModelGenerationUIStage::Saving3DOptions:
        case ModelGenerationUIStage::GeneratingDesign:
            feedback->title = t(snapshot.smart_mode ? "正在准备建模参考图…" : "正在准备模型输入");
            if (snapshot.smart_mode)
                feedback->detail = t("完成参考图准备与校验后，将自动生成3D模型。");
            design_wait.synchronize(inline_generation_task(snapshot), snapshot.design_elapsed_seconds,
                snapshot.design_estimated_seconds, now_seconds());
            feedback->detail += "\n" + design_wait.message(now_seconds()); break;
        case ModelGenerationUIStage::GeneratingModel:
            feedback->title = t("正在生成3D模型...");
            feedback->progress = ModelGenerationPresentation::ModelGenerationProgress::viewport_progress(snapshot.progress, false);
            if (snapshot.model_progress_estimated) feedback->detail += wxString::Format(t(" · 估算进度（已等待 %d 秒）"), snapshot.model_elapsed_seconds);
            break;
        case ModelGenerationUIStage::LoadingModel: feedback->title = t("正在获取并加载3D模型"); break;
        case ModelGenerationUIStage::Stopping: feedback->title = t("正在停止生成"); break;
        case ModelGenerationUIStage::Stopped: feedback->title = t("生成已停止"); break;
        case ModelGenerationUIStage::Failed: feedback->title = t("模型生成未完成"); break;
        case ModelGenerationUIStage::ModelReady: feedback->title = state.ready() ? t("3D模型已生成") : t("模型预览尚未就绪"); break;
        default: feedback->title = t("等待生成3D模型"); break;
        }
        if ((snapshot.stage == ModelGenerationUIStage::Failed || snapshot.stage == ModelGenerationUIStage::Stopped) &&
            state.phase() != InlinePreviewPhase::Failed) {
            if (!snapshot.summary_text.empty() && snapshot.summary_text != snapshot.status_text)
                feedback->detail += "\n" + t(snapshot.summary_text.c_str());
            if (snapshot.provider_error_ambiguous) feedback->detail += "\n" + t("提交结果待核对，请先恢复原任务，避免重复生成。");
        }
        if (!snapshot.model_ready && !snapshot.busy && !snapshot.model_path.empty() && !state.artifact()) {
            feedback->title = t("3D模型文件不可用");
            feedback->detail = t("模型文件缺失或无法读取，请恢复原文件或从历史中重新打开。");
        }
        const bool retry_preview = state.phase() == InlinePreviewPhase::Failed;
        const auto action = retry_preview ? t("重新加载预览") : snapshot.can_retry_service ? t("重新检测服务") :
            snapshot.can_retry_model ? t("恢复当前任务") : t("恢复上次任务");
        if (recover->GetLabel() != action) recover->SetLabel(action);
        const bool recovery_shown = retry_preview || (!state.ready() && !snapshot.busy &&
            (snapshot.can_retry_model || snapshot.can_retry_service || snapshot.can_restore_latest));
        if (recover->IsShown() != recovery_shown) recover->Show(recovery_shown);
        bool stacking_changed = false;
        if (feedback->IsShown() != !state.ready()) { feedback->Show(!state.ready()); stacking_changed = true; }
        const bool preview_shown = state.phase() == InlinePreviewPhase::AwaitingFirstFrame || state.ready();
        if (preview && preview->IsShown() != preview_shown) { preview->Show(preview_shown); stacking_changed = true; }
        if (stacking_changed && feedback->IsShown()) feedback->Raise();
        view.SetName(feedback->title + (feedback->detail.empty() ? wxString() : " · " + feedback->detail));
        layout(); feedback->Refresh(false); recover->Refresh(false);
    }
    void start() {
        if (!visible() || !state.artifact() || state.phase() != InlinePreviewPhase::Empty || preparation || closing) return;
        preparation = std::make_shared<Preparation>();
        preparation->generation = state.generation(); preparation->request = state.artifact()->request;
        state.advance(state.generation(), InlinePreviewPhase::Preparing);
        auto request = preparation;
        try {
            const auto task = preparation_scope.begin(request->request.entry.job_id);
            auto* target = &view;
            const auto alive = std::weak_ptr<int>(lifetime);
            const bool accepted = Presentation::background_tasks().submit(task, [request](const Presentation::TaskContext& context) {
                try {
                    const auto& open = request->request;
                    const auto stopped = [&context, request] { return context.canceled() || request->canceled.load(); };
                    if (!open.sha256.empty() && AI::model_artifact_sha256(open.entry.model_path) != open.sha256)
                        request->error = "模型文件摘要已变化，请恢复原文件后重新加载。";
                    else if (!ModelPreview3D::prepare_model(open.entry.model_path, request->prepared, request->error,
                        {}, ModelGenerationPresentation::library_metadata_path(open.entry.job_id), true,
                        stopped) && request->error.empty())
                        request->error = "模型文件无法解析。";
                    if (request->error.empty() && !open.sha256.empty() && request->prepared.artifact_sha256 != open.sha256)
                        request->error = "模型在加载期间发生变化。";
                    if (request->error.empty() && !stopped()) request->prepared.prepare_render_geometry(stopped);
                } catch (const std::exception& exception) { request->error = exception.what(); }
                return true;
            }, [target, alive, task, request](Presentation::TaskResult<bool> result) {
                if (alive.expired()) return;
                auto& self = *target->m_impl;
                if (!self.preparation_scope.matches(task) || self.preparation != request || result.canceled) return;
                if (result.error || !result.value) request->error = "模型预览准备未完成，请重新加载。";
                request->done.store(true);
                self.refresh.request(Presentation::Dirty::Content);
            }, Presentation::TaskPriority::Visible);
            if (!accepted) throw std::runtime_error("模型预览任务暂时繁忙，请重新加载。");
        } catch (const std::exception& exception) {
            error = exception.what();
            if (state.binding().source == CreationSource::Advanced)
                BOOST_LOG_TRIVIAL(warning) << "[InlineModelPreview] job=" << state.snapshot().job_id << " reason=" << error;
            preparation.reset(); state.advance(state.generation(), InlinePreviewPhase::Failed); changed();
        }
        render();
    }
    void install() {
        if (!preparation || !preparation->done.load()) return;
        if (preparation->canceled || preparation->generation != state.generation()) { preparation.reset(); return; }
        if (!visible()) return; // Retain prepared CPU data until this source is shown.
        auto request = std::move(preparation);
        error = request->error;
        if (error.empty()) {
            cover_requested = true;
            sync_first_frame_cover();
            if (!preview) {
                preview = new ModelPreview3D(&view); preview->set_preview_background(background_colour());
                preview->Hide();
                preview->set_selection_enabled(false); preview->set_color_controls_visible(false);
            }
            size_t triangles = 0, colors = 0; Vec3d dimensions = Vec3d::Zero();
            if (!preview->load_prepared_model(std::move(request->prepared), request->request.entry.palette,
                triangles, dimensions, colors, error) && error.empty()) error = "模型预览无法加载。";
        }
        if (!error.empty()) {
            if (state.binding().source == CreationSource::Advanced)
                BOOST_LOG_TRIVIAL(warning) << "[InlineModelPreview] job=" << state.snapshot().job_id << " reason=" << error;
            state.advance(request->generation, InlinePreviewPhase::Failed); render(); changed(); return;
        }
        state.advance(request->generation, InlinePreviewPhase::AwaitingFirstFrame);
        render(); // Show the final-size GL canvas before requesting its first frame.
        deadline.begin(RedesignPresentationDeadline::Clock::now());
        wxWeakRef<InlineModelGenerationView> weak(&view);
        preview->observe_presentation_frame([weak, generation = request->generation] {
            if (!weak || weak->m_impl->closing) return;
            auto& self = *weak->m_impl;
            if (!self.visible() || !self.state.advance(generation, InlinePreviewPhase::Ready)) return;
            self.preview->observe_presentation_frame({});
            self.render(); self.changed(); self.finish_first_frame_handoff(); self.notify();
        });
    }
    void finish_first_frame_handoff() {
        if (!cover_requested || !state.ready() || !visible()) return;
        // Completion actions can relayout the parent. Submit the resulting GL
        // frame while feedback still covers native invalidation, then repair
        // the exposed canvas synchronously as the local cover is withdrawn.
        preview->refresh();
        cover_requested = false;
        sync_first_frame_cover();
        preview->refresh();
    }
    void notify() {
        if (!cover_requested && state.consume_completion(visible()) && callbacks.completed) callbacks.completed();
    }
    void tick() {
        if (closing) return;
        install(); start();
        const bool shown = visible();
        animation.synchronize(feedback->animate, shown, now_seconds());
        feedback->elapsed = animation.elapsed(now_seconds());
        sync_first_frame_cover();
        finish_first_frame_handoff();
        if (state.phase() == InlinePreviewPhase::AwaitingFirstFrame &&
            deadline.observe(RedesignPresentationDeadline::Clock::now(), !shown)) {
            preview->observe_presentation_frame({}); error = "模型预览未能显示，请重新加载预览。";
            state.advance(state.generation(), InlinePreviewPhase::Failed); render(); changed();
        }
        if (shown && feedback->animate) feedback->Refresh(false);
        notify();
        const bool needs_timer = shown && (feedback->animate || state.phase() == InlinePreviewPhase::AwaitingFirstFrame);
        if (needs_timer && !timer.IsRunning()) timer.Start(42);
        else if (!needs_timer) timer.Stop();
    }
};

InlineModelGenerationView::InlineModelGenerationView(wxWindow* parent, InlineModelCallbacks callbacks)
    : wxPanel(parent), m_impl(std::make_unique<Impl>(*this, std::move(callbacks))) {}
InlineModelGenerationView::~InlineModelGenerationView() { m_impl->shutdown(); }
void InlineModelGenerationView::shutdown() { m_impl->shutdown(); }
void InlineModelGenerationView::apply_snapshot(const GenerationBinding& binding, const ModelGenerationUIState& snapshot,
    std::optional<WorkbenchOpenRequest> artifact) {
    if (m_impl->closing) return;
    const auto generation = m_impl->state.generation();
    const bool ready_before = ready();
    const auto stage_before = m_impl->state.snapshot().stage;
    const bool content_before = has_content();
    std::optional<InlineModelArtifact> model;
    if (artifact) model = InlineModelArtifact {std::move(*artifact)};
    if (!m_impl->state.apply(binding, snapshot, std::move(model))) return;
    std::unique_ptr<wxWindowUpdateLocker> content_handoff;
    if (generation != m_impl->state.generation() && m_impl->visible())
        content_handoff = std::make_unique<wxWindowUpdateLocker>(this);
    if (generation != m_impl->state.generation()) {
        m_impl->cover_requested = false;
        m_impl->sync_first_frame_cover();
        m_impl->design_wait.clear();
        if (m_impl->preparation) m_impl->preparation->canceled = true;
        m_impl->preparation_scope.cancel(); m_impl->preparation.reset();
        m_impl->error.clear();
        if (m_impl->preview) {
            // Retire a completed preview only after the new feedback is drawn.
            // Clearing the still-visible GL canvas exposes an empty frame.
            m_impl->preview->observe_presentation_frame({});
            if (m_impl->visible()) {
                m_impl->render();
                if (m_impl->feedback->IsShown()) m_impl->feedback->Update();
            }
            m_impl->preview->clear();
        }
    }
    if (generation != m_impl->state.generation() || stage_before != snapshot.stage || content_before != has_content()) {
        // A newly shown child must already have its current feedback. Ordinary
        // progress still uses the existing coalesced refresh path.
        m_impl->render();
        wxString trace;
        if (wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &trace) && trace == "1")
            BOOST_LOG_TRIVIAL(info) << "[InlineModelPresentation] source=" << int(binding.source)
                << " stage=" << int(snapshot.stage) << " content=" << has_content()
                << " generation=" << m_impl->state.generation();
    }
    m_impl->refresh.request(Presentation::Dirty::Content);
    if (ready_before != ready()) m_impl->changed();
    if (content_handoff) {
        // Hide/Show on native siblings can expose their parent before buffered
        // feedback is painted. Complete this local handoff before returning.
        content_handoff.reset();
        if (m_impl->feedback->IsShown()) m_impl->feedback->Update();
    }
}
void InlineModelGenerationView::set_presented(bool active) {
    if (m_impl->closing) return;
    const bool reveal = active && !m_impl->presented;
    if (reveal) {
        // A previously hidden view may still have its initial zero-size bounds.
        // Commit its local slot before the first visible feedback paint.
        GetParent()->Layout();
        m_impl->render();
        if (m_impl->feedback->IsShown()) m_impl->feedback->Update();
    }
    if (reveal && m_impl->preview) m_impl->preview->refresh();
    m_impl->presented = active; m_impl->tick();
    if (active) m_impl->refresh.resume();
    if (reveal && m_impl->visible() && m_impl->feedback->IsShown()) {
        m_impl->render();
        m_impl->feedback->Update(); // One initial display handoff, never each progress snapshot.
    }
}
void InlineModelGenerationView::retry_preview() {
    if (m_impl->closing || !m_impl->state.artifact()) return;
    if (m_impl->preparation) m_impl->preparation->canceled = true;
    m_impl->preparation_scope.cancel(); m_impl->preparation.reset();
    if (m_impl->preview) { m_impl->preview->observe_presentation_frame({}); m_impl->preview->clear(); }
    m_impl->state.retry(); m_impl->error.clear(); m_impl->render(); m_impl->changed(); m_impl->tick();
}
void InlineModelGenerationView::generation_started() { m_impl->state.generation_started(); }
void InlineModelGenerationView::invalidate_context() {
    m_impl->cover_requested = false;
    m_impl->sync_first_frame_cover();
    if (m_impl->preparation) m_impl->preparation->canceled = true;
    m_impl->preparation_scope.cancel(); m_impl->preparation.reset();
    if (m_impl->preview) { m_impl->preview->observe_presentation_frame({}); m_impl->preview->clear(); }
    m_impl->state.invalidate(); m_impl->render(); m_impl->changed();
}
bool InlineModelGenerationView::has_content() const { return m_impl->state.has_content(); }
bool InlineModelGenerationView::ready() const { return m_impl->state.ready(); }
std::optional<WorkbenchOpenRequest> InlineModelGenerationView::displayed_artifact() const {
    return ready() ? std::optional<WorkbenchOpenRequest>(m_impl->state.artifact()->request) : std::nullopt;
}

GenerationCompletionActions::GenerationCompletionActions(wxWindow* parent, std::function<void()> regenerate,
    std::function<void()> open_workbench) : wxPanel(parent) {
    SetBackgroundColour(parent->GetBackgroundColour()); SetName("generation-completion-actions");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_glow_source.LoadFile(wxString::FromUTF8(Slic3r::resources_dir() + "/images/inline_generation_workbench_glow.png"));
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    auto* column = new wxBoxSizer(wxVERTICAL); SetSizer(column);
    auto* row = new wxBoxSizer(wxHORIZONTAL); column->Add(row, 0, wxEXPAND);
    auto* again = new RoundedActionButton(this, t("重新生成"), false, 44); m_regenerate = again;
    auto* workbench = new RoundedActionButton(this, t("进入工作台"), true, 44); m_workbench = workbench;
    again->SetName("regenerate-3d-model"); workbench->SetName("enter-model-workbench");
    again->SetMinSize(FromDIP(wxSize(120,44))); workbench->SetMinSize(FromDIP(wxSize(186,44)));
    again->set_corner_radius(32); workbench->set_corner_radius(32); workbench->set_generation_gradient(true);
    again->set_generation_completion_style(false); workbench->set_generation_completion_style(true);
    again->set_icon(create_scaled_bitmap("inline_generation_regenerate.png",again,20)); again->set_icon_on_left(true);
    workbench->set_icon(create_scaled_bitmap("inline_generation_workbench",workbench,20)); workbench->set_icon_on_left(true);
    again->set_secondary_face(smart_creation_opaque_colour(wxColour(255,255,255,23), GetBackgroundColour()));
    style_smart_creation_text(again,16,smart_creation_opaque_colour(wxColour(255,255,255,166),GetBackgroundColour()),wxFONTWEIGHT_MEDIUM);
    style_smart_creation_text(workbench,16,*wxWHITE,wxFONTWEIGHT_MEDIUM);
    again->set_text_colour(again->GetForegroundColour()); workbench->set_text_colour(*wxWHITE);
    again->Bind(wxEVT_BUTTON, [regenerate](wxCommandEvent&) { if (regenerate) regenerate(); });
    workbench->Bind(wxEVT_BUTTON, [open_workbench](wxCommandEvent&) { if (open_workbench) open_workbench(); });
    row->Add(again,120,wxEXPAND); row->AddSpacer(FromDIP(6)); row->Add(workbench,186,wxEXPAND);
    column->AddSpacer(FromDIP(16)); // The glow occupies the original bottom inset.
    SetMinSize(FromDIP(wxSize(312,60))); Hide();
}
void GenerationCompletionActions::paint() {
    wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
    const auto size = wxSize(m_workbench->GetSize().x, FromDIP(60));
    if (!m_glow_source.IsOk() || size.x <= 0 || size.y <= 0) return;
    if (!m_glow.IsOk() || size != m_glow_size) {
        m_glow = wxBitmap(m_glow_source.Scale(size.x, size.y, wxIMAGE_QUALITY_HIGH));
        m_glow_size = size;
    }
    dc.DrawBitmap(m_glow, m_workbench->GetPosition().x, 0, true);
}
void GenerationCompletionActions::set_enabled(bool regenerate, bool open_workbench) {
    if (m_regenerate->IsEnabled() != regenerate) { m_regenerate->Enable(regenerate); m_regenerate->Refresh(false); }
    if (m_workbench->IsEnabled() != open_workbench) { m_workbench->Enable(open_workbench); m_workbench->Refresh(false); }
}
} // namespace Slic3r::GUI
