#pragma once

#include <slic3r/GUI/IMediaController.hpp>
#include <slic3r/Utils/IPrinterAgent.hpp>

#include <string>
#include <wx/string.h>
#include <wx/uri.h>

class wxWebView;

namespace Slic3r { namespace GUI {

class WebMediaController : public IMediaController
{
public:
    explicit WebMediaController(wxWebView* webview);

    void Load(wxURI url) override;

    void set_mode(CameraStreamMode mode);

    void set_idle_image(const wxString& image);

    void Play() override;

    void Stop() override;

private:
    void show_idle_image();

    wxWebView* m_webview = nullptr;
    std::string m_url;
    wxString m_idle_image;
    CameraStreamMode m_stream_mode = CameraStreamMode::http;
};

}} // namespace Slic3r::GUI
