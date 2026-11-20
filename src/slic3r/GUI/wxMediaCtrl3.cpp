#include "wxMediaCtrl3.h"
#include "AVVideoDecoder.hpp"
#include <algorithm>
#include <boost/log/trivial.hpp>
#include "slic3r/GUI/Printer/BambuTunnel.h"
#include <cassert>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <wx/event.h>
#include <wx/uri.h>
#include <wx/gdicmn.h>
#include <wx/mediactrl.h>
#include <wx/string.h>
#include <wx/image.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>

class wxWindow;
}
#ifdef __WIN32__
#include <versionhelpers.h>
#include <wx/msw/registry.h>
#include <shellapi.h>
#endif

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

BEGIN_EVENT_TABLE(wxMediaCtrl3, wxWindow)

// catch paint events
EVT_PAINT(wxMediaCtrl3::paintEvent)

END_EVENT_TABLE()

struct StaticBambuLib : BambuLib
{
    static StaticBambuLib &get(BambuLib *);
};

wxMediaCtrl3::wxMediaCtrl3(wxWindow *parent)
    : wxWindow(parent, wxID_ANY)
    , BambuLib(StaticBambuLib::get(this))
    , m_thread([this] { PlayThread(); })
{
    SetBackgroundColour("#000001ff");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
}

wxMediaCtrl3::~wxMediaCtrl3()
{
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_url.reset(new wxURI);
        m_frame = wxImage(m_idle_image);
        m_cond.notify_all();
    }
    m_thread.join();
}

static void adjust_frame_size(wxSize& frame, wxSize const& video, wxSize const& window);

void wxMediaCtrl3::Load(wxURI url)
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_external)
        return;
    m_video_size = wxDefaultSize;
    m_state = wxMEDIASTATE_STOPPED;
    m_frame_is_video = false;
    m_error = 0;
    m_url.reset(new wxURI(url));
    m_cond.notify_all();
}

void wxMediaCtrl3::Play()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_external)
        return;
    if (m_state != wxMEDIASTATE_PLAYING) {
        m_state = wxMEDIASTATE_PLAYING;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

void wxMediaCtrl3::Stop()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_url.reset();
    m_frame = wxImage(m_idle_image);
    m_frame_is_video = false;
    NotifyStopped();
    m_cond.notify_all();
    QueueRefresh();
}

void wxMediaCtrl3::QueueRefresh()
{
    if (!m_refresh_pending.exchange(true)) {
        CallAfter([this] {
            m_refresh_pending.store(false);
            Refresh(false);
        });
    }
}

void wxMediaCtrl3::SetExternalFrame(const wxImage& frame, wxSize videoSize)
{
    if (!frame.IsOk())
        return;
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (!m_external)
            return;
        m_frame = frame;
        m_frame_is_video = true;
        m_state = wxMEDIASTATE_PLAYING;
        m_video_size = videoSize.IsFullySpecified() ? videoSize : frame.GetSize();
        adjust_frame_size(m_frame_size, m_video_size, GetSize());
    }
    QueueRefresh();
}

#ifdef _WIN32
void wxMediaCtrl3::SetExternalFrame(const wxBitmap& frame, wxSize videoSize)
{
    if (!frame.IsOk())
        return;
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (!m_external)
            return;
        m_frame = frame;
        m_frame_is_video = true;
        m_state = wxMEDIASTATE_PLAYING;
        m_video_size = videoSize.IsFullySpecified() ? videoSize : frame.GetSize();
        adjust_frame_size(m_frame_size, m_video_size, GetSize());
    }
    QueueRefresh();
}
#endif

void wxMediaCtrl3::BeginExternalStream()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_external = true;
    m_url.reset();
    m_active_url.reset();
    m_video_size = wxDefaultSize;
    m_state = wxMEDIASTATE_STOPPED;
    m_frame_is_video = false;
    m_frame = wxImage(m_idle_image);
    m_cond.notify_all();
    Refresh();
}

void wxMediaCtrl3::EndExternalStream()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_external = false;
    m_url.reset();
    m_active_url.reset();
    m_video_size = wxDefaultSize;
    m_state = wxMEDIASTATE_STOPPED;
    m_frame_is_video = false;
    m_frame = wxImage(m_idle_image);
    m_cond.notify_all();
    Refresh();
}

void wxMediaCtrl3::SetIdleImage(wxString const &image)
{
    if (m_idle_image == image)
        return;
    m_idle_image = image;
    if (m_url == nullptr) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_frame = wxImage(m_idle_image);
        m_frame_is_video = false;
        assert(m_frame.IsOk());
        Refresh();
    }
}

wxMediaState wxMediaCtrl3::GetState()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_state;
}

int wxMediaCtrl3::GetLastError() const
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_error;
}

wxSize wxMediaCtrl3::GetVideoSize() const
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_video_size;
}

wxImage wxMediaCtrl3::GetCurrentFrame()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_state != wxMEDIASTATE_PLAYING || !m_frame_is_video || !m_frame.IsOk())
        return {};
#ifdef _WIN32
    return m_frame.ConvertToImage();
#else
    return m_frame.Copy();
#endif
}

wxSize wxMediaCtrl3::DoGetBestSize() const
{
    return {-1, -1};
}

static void adjust_frame_size(wxSize & frame, wxSize const & video, wxSize const & window)
{
    if (video.x * window.y < video.y * window.x)
        frame = { video.x * window.y / video.y, window.y };
    else
        frame = { window.x, video.y * window.x / video.x };
}

void wxMediaCtrl3::paintEvent(wxPaintEvent &evt)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();
    auto      size = GetSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    std::unique_lock<std::mutex> lk(m_mutex);
    if (!m_frame.IsOk())
        return;
    auto size2 = m_frame.GetSize();
    if (size2.x != m_frame_size.x && size2.y == m_frame_size.y)
        size2.x = m_frame_size.x;
    auto size3 = (size - size2) / 2;
    if (size2.x != size.x && size2.y != size.y) {
        double scale = 1.;
        if (size.x * size2.y > size.y * size2.x) {
            size3 = {size.x * size2.y / size.y, size2.y};
            scale = double(size.y) / size2.y;
        } else {
            size3 = {size2.x, size.y * size2.x / size.x};
            scale = double(size.x) / size2.x;
        }
        dc.SetUserScale(scale, scale);
        size3 = (size3 - size2) / 2;
    }
    dc.DrawBitmap(m_frame, size3.x, size3.y);
}

void wxMediaCtrl3::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (sizeFlags == wxSIZE_USE_EXISTING) return;
    wxMediaCtrl_OnSize(this, m_video_size, width, height);
    std::unique_lock<std::mutex> lk(m_mutex);
    adjust_frame_size(m_frame_size, m_video_size, GetSize());
    Refresh();
}

void wxMediaCtrl3::bambu_log(void *ctx, int level, tchar const *msg2)
{
#ifdef _WIN32
    wxString msg(msg2);
#else
    wxString msg = wxString::FromUTF8(msg2);
#endif
    if (level == 1) {
        if (msg.EndsWith("]")) {
            int n = msg.find_last_of('[');
            if (n != wxString::npos) {
                long val = 0;
                wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
                if (msg.SubString(n + 1, msg.Length() - 2).ToLong(&val)) {
                    std::unique_lock<std::mutex> lk(ctrl->m_mutex);
                    ctrl->m_error = (int) val;
                }
            }
        } else if (msg.Contains("stat_log")) {
            wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
            wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
            evt.SetEventObject(ctrl);
            evt.SetString(msg.Mid(msg.Find(' ') + 1));
            wxPostEvent(ctrl, evt);
        }
    }
    BOOST_LOG_TRIVIAL(info) << msg.ToUTF8().data();
}

// FFmpeg's own diagnostics (HTTP status, "Invalid data found", demuxer choice,
// missing stream dimensions, ...) are otherwise swallowed: a failed camera open
// only surfaces as wxMediaCtrl3's generic error code, which MediaPlayCtrl maps to
// the misleading "Player is malfunctioning" string. Forward them to the Orca log
// instead. Verbosity defaults to AV_LOG_VERBOSE and can be raised at runtime with
// ORCA_FFMPEG_LOG_LEVEL=debug|trace|... (or lowered to warning/error/quiet).
static int ffmpeg_log_level_from_env()
{
    const char *env = std::getenv("ORCA_FFMPEG_LOG_LEVEL");
    if (env == nullptr || *env == '\0')
        return AV_LOG_VERBOSE;
    const wxString v = wxString(env).Lower();
    if (v == "quiet")   return AV_LOG_QUIET;
    if (v == "panic")   return AV_LOG_PANIC;
    if (v == "fatal")   return AV_LOG_FATAL;
    if (v == "error")   return AV_LOG_ERROR;
    if (v == "warning") return AV_LOG_WARNING;
    if (v == "info")    return AV_LOG_INFO;
    if (v == "verbose") return AV_LOG_VERBOSE;
    if (v == "debug")   return AV_LOG_DEBUG;
    if (v == "trace")   return AV_LOG_TRACE;
    return AV_LOG_VERBOSE;
}

static void ffmpeg_log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;
    thread_local int print_prefix = 1;
    char line[1024];
    av_log_format_line2(avcl, level, fmt, vl, line, (int) sizeof(line), &print_prefix);
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
        line[--len] = '\0';
    if (len == 0)
        return;
    if (level <= AV_LOG_ERROR)
        BOOST_LOG_TRIVIAL(error) << "ffmpeg: " << line;
    else if (level <= AV_LOG_WARNING)
        BOOST_LOG_TRIVIAL(warning) << "ffmpeg: " << line;
    else if (level <= AV_LOG_INFO)
        BOOST_LOG_TRIVIAL(info) << "ffmpeg: " << line;
    else
        BOOST_LOG_TRIVIAL(debug) << "ffmpeg: " << line;
}

static void install_ffmpeg_logger()
{
    av_log_set_level(ffmpeg_log_level_from_env());
    av_log_set_callback(&ffmpeg_log_callback);
}

int wxMediaCtrl3::ffmpeg_interrupt_callback(void *opaque)
{
    auto *ctrl = static_cast<wxMediaCtrl3 *>(opaque);
    std::lock_guard<std::mutex> lock(ctrl->m_mutex);
    return ctrl->m_url != ctrl->m_active_url || std::chrono::steady_clock::now() >= ctrl->m_io_deadline;
}

int wxMediaCtrl3::PlayFfmpeg(std::shared_ptr<wxURI> const &url, std::unique_lock<std::mutex> &lock)
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    static std::once_flag logger_once;
    std::call_once(logger_once, install_ffmpeg_logger);

    if (avformat_network_init() < 0)
        return 2;

    AVFormatContext *format_context = avformat_alloc_context();
    if (!format_context) {
        avformat_network_deinit();
        return 2;
    }

    format_context->interrupt_callback = {&wxMediaCtrl3::ffmpeg_interrupt_callback, this};
    m_active_url = url;

    auto finish = [&](int error) {
        lock.unlock();
        avformat_close_input(&format_context);
        avformat_network_deinit();
        lock.lock();
        m_active_url.reset();
        return error;
    };

    const std::string uri = url->BuildURI().ToUTF8().data();
    const wxString scheme = url->GetScheme();
    const bool http_stream = scheme.CmpNoCase("http") == 0 || scheme.CmpNoCase("https") == 0;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "rw_timeout", "5000000", 0);
    av_dict_set(&options, "timeout", "5000000", 0);
    av_dict_set(&options, "tls_verify", "1", 0);
    av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls,rtp,udp", 0);
    if (http_stream) {
        av_dict_set(&options, "probesize", "5000000", 0);
        av_dict_set(&options, "analyzeduration", "1000000", 0);
    } else {
        av_dict_set(&options, "rtsp_transport", "udp+tcp", 0);
        av_dict_set(&options, "rtsp_flags", "prefer_tcp", 0);
    }
    m_io_deadline = Clock::now() + 12s;
    lock.unlock();
    int error = avformat_open_input(&format_context, uri.c_str(), nullptr, &options);
    av_dict_free(&options);
    lock.lock();
    if (error < 0 || m_url != url)
        return finish(2);

    const bool segmented_stream = std::strcmp(format_context->iformat->name, "hls") == 0;
    if (std::strcmp(format_context->iformat->name, "mpjpeg") == 0)
        format_context->flags |= AVFMT_FLAG_NOBUFFER;
    m_io_deadline = Clock::now() + 12s;
    lock.unlock();
    error = avformat_find_stream_info(format_context, nullptr);
    lock.lock();
    if (error < 0 || m_url != url)
        return finish(2);

    const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream < 0)
        return finish(2);

    AVVideoDecoder decoder;
    if (decoder.open(*format_context->streams[video_stream]->codecpar) < 0)
        return finish(2);

    // Prefer the dimensions the container reported. A small probe budget, or a
    // camera that doesn't announce a size up front, can leave these unset - in
    // that case fill them in from the first frame that decodes (below) rather
    // than failing the open outright.
    auto apply_video_size = [&](wxSize size) {
        if (!size.IsFullySpecified() || size.x <= 0 || size.y <= 0)
            return false;
        m_video_size = size;
        adjust_frame_size(m_frame_size, m_video_size, GetSize());
        return true;
    };
    bool have_size = apply_video_size({format_context->streams[video_stream]->codecpar->width,
                                      format_context->streams[video_stream]->codecpar->height});
    int size_probe_frames = 0; // frames spent still waiting for a usable size

    AVPacket *packet = av_packet_alloc();
    if (!packet)
        return finish(2);

    Clock::time_point playback_start;
    std::int64_t start_timestamp = AV_NOPTS_VALUE;
    const auto time_base = format_context->streams[video_stream]->time_base;
    while (m_url == url) {
        m_io_deadline = Clock::now() + 12s;
        lock.unlock();
        error = av_read_frame(format_context, packet);
        lock.lock();
        if (m_url != url)
            break;
        if (error < 0)
            break;
        if (packet->stream_index == video_stream) {
            lock.unlock();
            const int decode_error = decoder.decode(*packet);
            lock.lock();
            if (m_url != url)
                break;
            if (decode_error == 0) {
                if (!have_size) {
                    have_size = apply_video_size(decoder.decoded_frame_size());
                    if (!have_size) {
                        av_packet_unref(packet);
                        // MJPEG yields a sized frame on the first full packet; if
                        // several seconds of frames never do, treat it as a bad
                        // stream instead of sitting in "Loading..." forever.
                        if (++size_probe_frames > 120)
                            break;
                        continue;
                    }
                }
                auto frame_size = m_frame_size;
                lock.unlock();
#ifdef _WIN32
                wxBitmap frame;
                decoder.toWxBitmap(frame, frame_size);
#else
                wxImage frame;
                decoder.toWxImage(frame, frame_size);
#endif
                lock.lock();
                if (m_url != url)
                    break;
                if (!frame.IsOk()) {
                    av_packet_unref(packet);
                    continue;
                }
                if (segmented_stream && decoder.frame_timestamp() != AV_NOPTS_VALUE) {
                    const auto timestamp = av_rescale_q(decoder.frame_timestamp(), time_base, AVRational{1, 1000});
                    const auto now = Clock::now();
                    auto due = start_timestamp == AV_NOPTS_VALUE ? now : playback_start + std::chrono::milliseconds(timestamp - start_timestamp);
                    if (start_timestamp == AV_NOPTS_VALUE || due < now - 1s || due > now + 3s) {
                        start_timestamp = timestamp;
                        playback_start = due = now;
                    }
                    if (m_cond.wait_until(lock, due, [this, &url] { return m_url != url; }))
                        break;
                }
                m_frame = frame;
                if (!m_frame_is_video) {
                    m_frame_is_video = true;
                    m_state = wxMEDIASTATE_PLAYING;
                    BOOST_LOG_TRIVIAL(info) << "[PrinterCamera] first_stream_frame width=" << m_video_size.GetWidth()
                                           << " height=" << m_video_size.GetHeight();
                    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED, GetId());
                    event.SetEventObject(this);
                    wxPostEvent(this, event);
                }
                QueueRefresh();
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return finish(m_url == url ? 2 : 1);
}

void wxMediaCtrl3::PlayThread()
{
    using namespace std::chrono_literals;
    std::shared_ptr<wxURI> url;
    std::unique_lock<std::mutex> lk(m_mutex);
    while (true) {
        m_cond.wait(lk, [this, &url] { return m_url != url; });
        url = m_url;
        if (url == nullptr)
            continue;
        if (!url->HasScheme())
            break;
        const wxString scheme = url->GetScheme();
        const bool generic_ffmpeg = scheme.CmpNoCase("http") == 0 || scheme.CmpNoCase("https") == 0 ||
                                    scheme.CmpNoCase("rtsp") == 0 || scheme.CmpNoCase("rtsps") == 0;
        int error = 0;
        if (generic_ffmpeg) {
            error = PlayFfmpeg(url, lk);
        } else {
            lk.unlock();
            Bambu_Tunnel tunnel = nullptr;
            error = Bambu_Create(&tunnel, url->BuildURI().ToUTF8());
            if (error == 0) {
                Bambu_SetLogger(tunnel, &wxMediaCtrl3::bambu_log, this);
                error = Bambu_Open(tunnel);
                if (error == 0)
                    error = Bambu_would_block;
            }
            lk.lock();
            while (error == int(Bambu_would_block)) {
                m_cond.wait_for(lk, 100ms);
                if (m_url != url) {
                    error = 1;
                    break;
                }
                lk.unlock();
                error = Bambu_StartStream(tunnel, true);
                lk.lock();
            }
            Bambu_StreamInfo info;
            if (error == 0)
                error = Bambu_GetStreamInfo(tunnel, 0, &info);
            AVVideoDecoder decoder;
            int minFrameDuration = 0;
            if (error == 0) {
                decoder.open(info);
                m_video_size = { info.format.video.width, info.format.video.height };
                adjust_frame_size(m_frame_size, m_video_size, GetSize());
                minFrameDuration = 800 / info.format.video.frame_rate; // 80%
                NotifyStopped();
            }
            Bambu_Sample sample;
            while (error == 0) {
                lk.unlock();
                error = Bambu_ReadSample(tunnel, &sample);
                lk.lock();
                while (error == int(Bambu_would_block)) {
                    m_cond.wait_for(lk, 100ms);
                    if (m_url != url) {
                        error = 1;
                        break;
                    }
                    lk.unlock();
                    error = Bambu_ReadSample(tunnel, &sample);
                    lk.lock();
                }
                if (error == 0) {
                    auto frame_size = m_frame_size;
                    lk.unlock();
                    decoder.decode(sample);
#ifdef _WIN32
                    wxBitmap bm;
                    decoder.toWxBitmap(bm, frame_size);
#else
                    wxImage bm;
                    decoder.toWxImage(bm, frame_size);
#endif
                    lk.lock();
                    if (m_url != url) {
                        error = 1;
                        break;
                    }
                    if (bm.IsOk()) {
                        auto now = std::chrono::system_clock::now();
                        if (m_last_PTS && (sample.decode_time - m_last_PTS) < 30000000ULL) { // 3s
                            auto next_PTS_expected = m_last_PTS_expected + std::chrono::milliseconds((sample.decode_time - m_last_PTS) / 10000ULL);
                            auto next_PTS_practical = m_last_PTS_practical + std::chrono::milliseconds(minFrameDuration);
                            auto next_PTS = std::max(next_PTS_expected, next_PTS_practical);
                            if (now < next_PTS)
                                std::this_thread::sleep_until(next_PTS);
                            else
                                next_PTS = now;
                            m_last_PTS = sample.decode_time;
                            m_last_PTS_expected = next_PTS_expected;
                            m_last_PTS_practical = next_PTS;
                        } else {
                            m_last_PTS = sample.decode_time;
                            m_last_PTS_expected = now;
                            m_last_PTS_practical = now;
                        }
                        m_frame = bm;
                        m_frame_is_video = true;
                    }
                    QueueRefresh();
                }
            }
            if (tunnel) {
                lk.unlock();
                Bambu_Close(tunnel);
                Bambu_Destroy(tunnel);
                tunnel = nullptr;
                lk.lock();
            }
        }
        if (m_url != url)
            continue;
        m_error = error;
        m_frame_size = wxDefaultSize;
        m_video_size = wxDefaultSize;
        NotifyStopped();
    }

}

void wxMediaCtrl3::NotifyStopped()
{
    m_frame_is_video = false;
    m_state = wxMEDIASTATE_STOPPED;
    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
    event.SetId(GetId());
    event.SetEventObject(this);
    wxPostEvent(this, event);
}
