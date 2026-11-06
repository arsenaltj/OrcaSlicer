#include "WebMediaController.hpp"
#include "IPrinterAgent.hpp"

#include <wx/filename.h>
#include <wx/filesys.h>
#include <wx/gdicmn.h>
#include <wx/uri.h>
#include <wx/webview.h>

namespace Slic3r { namespace GUI {

WebMediaController::WebMediaController(wxWebView* webview)
    : m_webview(webview)
{
    if (m_webview)
        m_webview->SetBackgroundColour(*wxBLACK);
    show_idle_image();
}

void WebMediaController::Load(wxURI url)
{
    m_url = url.BuildURI().ToStdString();
}

void WebMediaController::set_mode(CameraStreamMode mode)
{
    m_stream_mode = mode;
}

void WebMediaController::set_idle_image(const wxString& image)
{
    m_idle_image = image;
    show_idle_image();
}

void WebMediaController::show_idle_image()
{
    if (!m_webview)
        return;

    wxString html = "<html><head><style>"
                    "html,body{margin:0;width:100%;height:100%;background:#19191b;overflow:hidden;}"
                    "body{display:flex;align-items:center;justify-content:center;}"
                    "img{width:100%;height:100%;object-fit:contain;display:block;}"
                    "</style></head><body>";
    if (!m_idle_image.empty()) {
        const wxString image_url = wxFileSystem::FileNameToURL(wxFileName(m_idle_image));
        html += "<img alt=\"Honor Forge\" src=\"" + image_url + "\">";
    }
    html += "</body></html>";
    m_webview->SetPage(html, "about:blank");
}

void WebMediaController::Play()
{
    if (!m_webview)
        return;

    wxString url  = wxString::FromUTF8(m_url);
    wxString html = "<html><head><style>"
                    "html,body{margin:0;height:100%;background:#000;overflow:hidden;}"
                    "img{width:100%;height:100%;object-fit:contain;display:block;}"
                    "</style></head><body><img id=\"camera-frame\"";
    if (m_stream_mode == CameraStreamMode::http_snapshot) {
        html += " data-camera-url=\"" + url +
                "\"><script>"
                "const cameraFrame=document.getElementById('camera-frame');"
                "const cameraUrl=cameraFrame.dataset.cameraUrl;"
                "let cameraFrameLoading=false;"
                "function refreshCameraFrame(){"
                "if(cameraFrameLoading)return;"
                "cameraFrameLoading=true;"
                "const nextFrame=new Image();"
                "nextFrame.onload=function(){cameraFrame.src=nextFrame.src;cameraFrameLoading=false;};"
                "nextFrame.onerror=function(){cameraFrameLoading=false;};"
                "nextFrame.src=cameraUrl+(cameraUrl.indexOf('?')>=0?'&':'?')+'_orca_frame='+Date.now();"
                "}"
                "let cameraRefreshInterval=null;"
                "function stopCameraRefresh(){"
                "if(cameraRefreshInterval!==null){"
                "clearInterval(cameraRefreshInterval);"
                "cameraRefreshInterval=null;"
                "}"
                "}"
                "refreshCameraFrame();"
                "cameraRefreshInterval = setInterval(refreshCameraFrame,200);"
                "</script></body></html>";
        m_webview->SetPage(html, url);
    } else {
        // Load MJPEG streams as the top-level document.  Some embedded WebView
        // backends buffer a multipart stream when it is used as an <img> resource,
        // which introduces noticeable live-view latency.
        m_webview->LoadURL(url);
    }
}

void WebMediaController::Stop()
{
    if (m_webview) {
        m_webview->RunScript("if(typeof stopCameraRefresh==='function') stopCameraRefresh();");
        m_webview->Stop();
        m_webview->ClearHistory();
    }
    m_url.clear();
    show_idle_image();
}

}} // namespace Slic3r::GUI
