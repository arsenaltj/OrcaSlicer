#include "DiagnosticLogExport.hpp"
#include "MsgDialog.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "GUI_App.hpp"
#include "slic3r/Utils/DiagnosticLogs.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include <wx/filedlg.h>
#include <wx/datetime.h>
#include "Presentation/UiRuntime.hpp"
#include <wx/timer.h>
#include <mutex>

namespace Slic3r::GUI {
void export_diagnostic_logs(wxWindow* parent) {
    Presentation::assert_gui_thread();
    // Keep the existing dialog style without a GUI-thread wait/yield loop.
    static bool exporting=false;if(exporting) return;
    struct Guard {bool& value;bool transferred{false};Guard(bool& v):value(v){value=true;}~Guard(){if(!transferred)value=false;}} guard(exporting);
    wxFileDialog picker(parent,wxString::FromUTF8("导出日志"),wxEmptyString,
        "HonorForge_Logs_"+wxDateTime::Now().Format("%Y%m%d_%H%M%S")+".zip",
        "ZIP (*.zip)|*.zip",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
    if(picker.ShowModal()!=wxID_OK) return;
    struct ProgressState {
        std::shared_ptr<std::atomic_bool> canceled {std::make_shared<std::atomic_bool>(false)};
        std::mutex mutex;
        std::string stage {"正在读取日志…"};
    };
    struct Operation {
        std::shared_ptr<ProgressState> state;
        wxWeakRef<ProgressDialog> progress;
        std::unique_ptr<wxTimer> timer;
        Presentation::TaskScope scope;
        ~Operation() { exporting = false; }
    };
    auto operation = std::make_shared<Operation>();
    operation->state = std::make_shared<ProgressState>();
    operation->progress = new ProgressDialog(wxString::FromUTF8("导出日志"),
        wxString::FromUTF8(operation->state->stage),100,parent,wxPD_APP_MODAL|wxPD_CAN_ABORT|wxPD_ELAPSED_TIME,true);
    operation->timer = std::make_unique<wxTimer>(operation->progress.get());
    std::weak_ptr<Operation> pending = operation;
    operation->progress->Bind(wxEVT_TIMER,[pending](wxTimerEvent&) {
        const auto op = pending.lock(); if (!op || !op->progress) return;
        std::string message; { std::lock_guard<std::mutex> lock(op->state->mutex); message = op->state->stage; }
        if (!op->progress->PulseFromEvent(wxString::FromUTF8(*op->state->canceled ? "正在取消导出…" : message)))
            *op->state->canceled = true;
    });
#ifdef _WIN32
    const boost::filesystem::path target(picker.GetPath().ToStdWstring());
    const boost::filesystem::path data(wxString::FromUTF8(Slic3r::data_dir()).ToStdWstring());
#else
    const boost::filesystem::path target(picker.GetPath().ToUTF8().data()),data(Slic3r::data_dir());
#endif
    wxWeakRef<wxWindow> owner(parent);
    const auto task = operation->scope.begin("diagnostic-export",Presentation::TaskLifetime::Transaction);
    operation->state->canceled = task.cancellation;
    const bool accepted = Presentation::background_tasks().submit(task,
        [state=operation->state,data,target](const Presentation::TaskContext&) {
            return Diagnostics::export_logs(data,target,
                std::string(HONOR_PRODUCT_VERSION)+" (engine "+SLIC3R_VERSION+")",*state->canceled,
                [state](const std::string& value) { std::lock_guard<std::mutex> lock(state->mutex); state->stage=value; });
        }, [operation,owner](Presentation::TaskResult<Diagnostics::LogExportResult> outcome) {
            operation->timer->Stop();
            if (operation->progress) { operation->progress->Hide(); operation->progress->Destroy(); }
            auto result = outcome.value.value_or(Diagnostics::LogExportResult{});
            if (outcome.canceled && !outcome.value) result.cancelled=true;
            if (outcome.error) result.error="日志导出失败，请重试。";
            exporting=false;
            if (!owner || wxGetApp().is_closing()) return;
    if(result.cancelled) return;
    std::string message=result.empty?"当前没有可导出的日志。":result.success?"日志导出成功。":result.error;
    if(result.success && !result.omitted.empty()) {
        message+="\n以下日志未能完整读取，已记录在 ZIP 清单中：";
        for(std::size_t i=0;i<result.omitted.size() && i<5;++i) message+="\n"+result.omitted[i];
        if(result.omitted.size()>5) message+="\n其余遗漏请查看 ZIP 内清单。";
    }
    MessageDialog(owner.get(),wxString::FromUTF8(message),wxString::FromUTF8("导出日志"),
        wxOK|(result.success && result.omitted.empty()?wxICON_INFORMATION:wxICON_WARNING)).ShowModal();
        });
    if (!accepted) {
        operation->progress->Hide(); operation->progress->Destroy(); return;
    }
    guard.transferred=true;
    operation->timer->Start(100);
}
}
