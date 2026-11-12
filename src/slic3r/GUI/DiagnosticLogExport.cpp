#include "DiagnosticLogExport.hpp"
#include "MsgDialog.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "slic3r/Utils/DiagnosticLogs.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include <wx/filedlg.h>
#include <wx/datetime.h>
#include <future>
#include <mutex>

namespace Slic3r::GUI {
void export_diagnostic_logs(wxWindow* parent) {
    // Pulse processes user events. Guard the entire picker/export lifecycle against reentry.
    static bool exporting=false;if(exporting) return;
    struct Guard {bool& value;Guard(bool& v):value(v){value=true;}~Guard(){value=false;}} guard(exporting);
    wxFileDialog picker(parent,wxString::FromUTF8("导出日志"),wxEmptyString,
        "HonorForge_Logs_"+wxDateTime::Now().Format("%Y%m%d_%H%M%S")+".zip",
        "ZIP (*.zip)|*.zip",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
    if(picker.ShowModal()!=wxID_OK) return;
    Diagnostics::LogExportResult result;
    {
        std::atomic_bool cancelled {false};std::mutex mutex;std::string stage="正在读取日志…";
        ProgressDialog progress(wxString::FromUTF8("导出日志"),wxString::FromUTF8(stage),100,parent,
            wxPD_APP_MODAL|wxPD_CAN_ABORT|wxPD_ELAPSED_TIME,true);
        if(!progress.Pulse()) cancelled=true;
#ifdef _WIN32
        const boost::filesystem::path target(picker.GetPath().ToStdWstring());
        const boost::filesystem::path data(wxString::FromUTF8(Slic3r::data_dir()).ToStdWstring());
#else
        const boost::filesystem::path target(picker.GetPath().ToUTF8().data()),data(Slic3r::data_dir());
#endif
        auto job=std::async(std::launch::async,[&] {
            return Diagnostics::export_logs(data,target,
                std::string(HONOR_PRODUCT_VERSION)+" (engine "+SLIC3R_VERSION+")",cancelled,
                [&](const std::string& value){std::lock_guard<std::mutex> lock(mutex);stage=value;});
        });
        while(job.wait_for(std::chrono::milliseconds(50))!=std::future_status::ready) {
            std::string message;{std::lock_guard<std::mutex> lock(mutex);message=stage;}
            if(!progress.Pulse(wxString::FromUTF8(cancelled?"正在取消导出…":message))) cancelled=true;
        }
        result=job.get();progress.Hide();
    }
    if(result.cancelled) return;
    std::string message=result.empty?"当前没有可导出的日志。":result.success?"日志导出成功。":result.error;
    if(result.success && !result.omitted.empty()) {
        message+="\n以下日志未能完整读取，已记录在 ZIP 清单中：";
        for(std::size_t i=0;i<result.omitted.size() && i<5;++i) message+="\n"+result.omitted[i];
        if(result.omitted.size()>5) message+="\n其余遗漏请查看 ZIP 内清单。";
    }
    MessageDialog(parent,wxString::FromUTF8(message),wxString::FromUTF8("导出日志"),
        wxOK|(result.success && result.omitted.empty()?wxICON_INFORMATION:wxICON_WARNING)).ShowModal();
}
}
