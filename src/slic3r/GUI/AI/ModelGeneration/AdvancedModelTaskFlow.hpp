#pragma once

#include "ModelGenerationHost.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelLibraryEntry.hpp"
#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include <memory>
#include <optional>

namespace Slic3r::GUI {

// Local observation lifecycle only. Submission and resume belong to the advanced Session.
// Ports return on the owning thread; their cancellation need not suppress late callbacks.
class AdvancedModelTaskFlow {
public:
    using Status = AIModelGenerationClient::JobStatus;
    using Artifact = AI::GeneratedModelArtifact;
    using Error = AIModelGenerationClient::ErrorFn;
    struct Ports {
        std::function<void(const std::string&, AIModelGenerationClient::StatusFn, Error)> status;
        std::function<void(const Status&, std::function<void(Artifact)>, Error,
            std::function<void(size_t, size_t)>)> acquire;
        std::function<void(Artifact, const Status&, const boost::filesystem::path&,
            std::function<void(ModelLibraryEntry)>, Error)> prepare;
        std::function<void(int)> schedule;
        std::function<void()> cancel;
    };

    explicit AdvancedModelTaskFlow(Ports ports) : m_ports(std::move(ports)) {}
    ~AdvancedModelTaskFlow() { m_alive.reset(); m_ports.cancel(); }
    AdvancedModelTaskFlow(const AdvancedModelTaskFlow&) = delete;
    AdvancedModelTaskFlow& operator=(const AdvancedModelTaskFlow&) = delete;

    void set_listener(ModelGenerationUIStateListener listener) { m_listener = std::move(listener); publish(); }
    const ModelGenerationUIState& snapshot() const { return m_state; }
    const std::optional<ModelLibraryEntry>& artifact() const { return m_artifact; }
    void reset() {
        ++m_epoch; m_ports.cancel();
        const auto revision = m_state.revision + 1, session = m_state.model_generation_session + 1;
        m_state = {}; m_state.revision = revision; m_state.model_generation_session = session;
        m_status = {}; m_artifact.reset(); m_pending = m_local = m_terminal = false;
    }
    void set_available(bool available) {
        m_available = available;
        publish();
        if (available) poll(); else m_ports.schedule(0);
    }
    void observe(const std::string& job, boost::filesystem::path reference, bool restart = false) {
        if (job.empty()) return;
        if (m_state.job_id == job && !restart) { publish(); return; }
        reset(); m_state.job_id = job; m_state.model_generation_context = true;
        m_state.busy = true; m_state.stage = ModelGenerationUIStage::GeneratingModel;
        m_reference = std::move(reference); m_progress.begin(job); publish(); poll();
    }
    void poll() {
        if (!m_available || m_pending || m_local || m_terminal || m_state.job_id.empty()) return;
        m_pending = true;
        const auto query = ++m_query;
        const auto valid = callback_guard();
        m_ports.status(m_state.job_id, [this, valid, query](Status status) {
            if (!valid() || !m_pending || query != m_query) return;
            m_pending = false;
            if (status.id != m_state.job_id) { reconnect("任务响应与当前任务不匹配。"); return; }
            if (status.updated_at > 0 && m_status.updated_at > status.updated_at) {
                m_ports.schedule(1200); return;
            }
            received(std::move(status));
        }, [this, valid, query](std::string message) {
            if (!valid() || !m_pending || query != m_query) return;
            m_pending = false; reconnect(std::move(message));
        });
    }
    void retry_local() { if (!m_state.busy && m_status.state == "ready" && m_status.artifact_ready) acquire(); }
    void stop_local() {
        if (!m_local) return;
        ++m_epoch; m_ports.cancel(); m_local = m_pending = false;
        m_state.busy = false; m_state.can_stop = false; m_state.can_retry_model = true;
        m_state.stage = ModelGenerationUIStage::Stopped; m_state.status_text = "模型本地加载已取消";
        m_state.summary_text = "远端模型已保留，可重新加载同一产物。"; publish();
    }

private:
    std::function<bool()> callback_guard() const {
        return [this, alive = std::weak_ptr<int>(m_alive), epoch = m_epoch] {
            return !alive.expired() && epoch == m_epoch;
        };
    }
    void publish() {
        ++m_state.revision; m_state.service_available = m_available;
        if (m_listener) m_listener(m_state);
    }
    void reconnect(std::string message) {
        m_state.status_text = "正在重新连接当前任务"; m_state.summary_text = std::move(message);
        publish(); if (m_available) m_ports.schedule(1200);
    }
    void received(Status status) {
        m_status = std::move(status);
        m_state.job_state = m_status.state; m_state.job_phase = m_status.phase;
        m_state.status_text = m_status.message; m_state.summary_text.clear();
        m_state.provider_name = m_status.provider_name; m_state.provider_task_id = m_status.provider_task_id;
        m_state.provider_conversion_task_id = m_status.provider_conversion_task_id;
        m_state.provider_error_code = m_status.provider_error_code;
        m_state.provider_error_category = m_status.provider_error_category;
        m_state.provider_error_retryable = m_status.provider_error_retryable;
        m_state.provider_error_ambiguous = m_status.provider_error_ambiguous;
        m_state.progress = m_progress.synchronize(m_status);
        m_state.model_progress_estimated = m_progress.estimated();
        m_state.model_elapsed_seconds = m_progress.elapsed_seconds();
        m_state.stage = m_status.state == "failed" ? ModelGenerationUIStage::Failed :
            (m_status.state == "stopped" || m_status.state == "cancelled") ? ModelGenerationUIStage::Stopped :
            m_status.state == "stopping" ? ModelGenerationUIStage::Stopping : ModelGenerationUIStage::GeneratingModel;
        m_terminal = m_state.stage == ModelGenerationUIStage::Failed || m_state.stage == ModelGenerationUIStage::Stopped;
        m_state.busy = !m_terminal; publish();
        if (m_status.artifact_ready && m_status.state == "ready") acquire();
        else if (!m_terminal) m_ports.schedule(1200);
    }
    void fail(std::string error) {
        m_local = false; m_terminal = true; m_state.busy = false; m_state.can_stop = false;
        m_state.stage = ModelGenerationUIStage::Failed; m_state.status_text = "模型本地加载未完成";
        m_state.summary_text = std::move(error); m_state.can_retry_model = m_status.artifact_ready; publish();
    }
    void acquire() {
        if (m_local) return;
        ++m_epoch; m_preparing = false;
        m_local = m_terminal = true; m_state.busy = m_state.can_stop = true; m_state.can_retry_model = false;
        m_state.stage = ModelGenerationUIStage::LoadingModel;
        m_state.status_text = "正在下载并校验生成的模型"; m_state.summary_text.clear(); publish();
        const auto valid = callback_guard();
        const auto failed = [this, valid](std::string error) { if (valid() && m_local) fail(std::move(error)); };
        m_ports.acquire(m_status, [this, valid, failed](Artifact artifact) {
            if (!valid() || !m_local || m_preparing) return;
            if (artifact.job_id != m_state.job_id) { fail("下载产物与当前任务不匹配。"); return; }
            m_preparing = true;
            m_state.progress = m_progress.loading(); m_state.status_text = "正在解析本地模型文件"; publish();
            m_ports.prepare(std::move(artifact), m_status, m_reference, [this, valid](ModelLibraryEntry entry) {
                if (!valid() || !m_local) return;
                if (entry.job_id != m_state.job_id || entry.model_path.empty()) { fail("本地模型身份无效。"); return; }
                m_artifact = std::move(entry); m_local = false;
                m_state.busy = m_state.can_stop = false;
                m_state.model_ready = m_state.can_open_workbench = true;
                m_state.model_asset_id = m_artifact->job_id; m_state.model_path = m_artifact->model_path.string();
                m_state.original_image_path = m_artifact->reference_image_path.string();
                m_state.design_image_path = m_artifact->ai_image_path.string();
                m_state.stage = ModelGenerationUIStage::ModelReady; m_state.progress = m_progress.complete();
                m_state.status_text = "3D 模型已生成，可打开工作台查看与检查。"; publish();
            }, failed);
        }, failed, [this, valid](size_t received, size_t total) {
            if (!valid() || !m_local || m_preparing) return;
            m_state.progress = m_progress.download(received, total); publish();
        });
    }
    Ports m_ports;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    ModelGenerationUIState m_state;
    ModelGenerationPresentation::ModelGenerationProgress m_progress;
    Status m_status;
    std::optional<ModelLibraryEntry> m_artifact;
    boost::filesystem::path m_reference;
    ModelGenerationUIStateListener m_listener;
    std::uint64_t m_epoch {0};
    std::uint64_t m_query {0};
    bool m_available {false}, m_pending {false}, m_local {false}, m_terminal {false}, m_preparing {false};
};
} // namespace Slic3r::GUI
