#include "AIDesktopFeatureHost.hpp"

#include "ModelGeneration/ModelGenerationFeatureHost.hpp"
#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/GUI/AIServiceManager.hpp"

#include <boost/log/trivial.hpp>
#include <wx/event.h>
#include <wx/timer.h>
#include <wx/window.h>

#include <string>
#include <utility>

namespace Slic3r::GUI {

struct AIDesktopFeatureHost::Impl final : wxEvtHandler
{
    Impl(wxWindow* parent, Plater* plater, NavigateAfterImportFn navigate_after_import,
         SmartSlicingAvailableFn smart_slicing_available)
        : model_generation(parent, plater, std::move(navigate_after_import), [this] { retry_now(); })
        , service_manager(AISidecarClient::default_endpoint())
        , retry_timer(this)
        , on_smart_slicing_available(std::move(smart_slicing_available))
    {
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { discover(); }, retry_timer.GetId());
    }

    ~Impl() override
    {
        shutdown();
    }

    void start()
    {
        if (started || shutdown_requested)
            return;
        started = true;
        discover();
    }

    void retry_now()
    {
        if (shutdown_requested)
            return;
        retry_timer.Stop();
        retry_count = 0;
        discover();
    }

    void discover()
    {
        if (discovery_active || shutdown_requested)
            return;
        discovery_active = true;
        service_manager.discover_async(model_generation.panel(), [this](AIServiceAvailability availability) {
            discovery_active = false;
            apply_availability(availability);
            if (availability.compatible) {
                retry_timer.Stop();
                retry_count = 0;
            } else if (availability.transient && retry_count < 20) {
                ++retry_count;
                retry_timer.StartOnce(500);
            }
        });
    }

    void apply_availability(const AIServiceAvailability& availability)
    {
        const std::string message = availability.compatible && !availability.model_generation_available
            ? "Configure the local AI service to enable 3D generation."
            : availability.error;
        model_generation.set_service_availability(
            availability.compatible && availability.model_generation_available, message);

        if (!availability.compatible) {
            BOOST_LOG_TRIVIAL(info) << "AI features unavailable: " << availability.error;
            return;
        }
        if (availability.config_proposal_available && !smart_slicing_announced) {
            smart_slicing_announced = true;
            if (on_smart_slicing_available)
                on_smart_slicing_available();
        }
    }

    void shutdown()
    {
        if (shutdown_requested)
            return;
        shutdown_requested = true;
        retry_timer.Stop();
        service_manager.shutdown();
        model_generation.shutdown();
    }

    ModelGenerationFeatureHost model_generation;
    AIServiceManager service_manager;
    wxTimer retry_timer;
    SmartSlicingAvailableFn on_smart_slicing_available;
    unsigned retry_count { 0 };
    bool discovery_active { false };
    bool smart_slicing_announced { false };
    bool shutdown_requested { false };
    bool started { false };
};

AIDesktopFeatureHost::AIDesktopFeatureHost(wxWindow* parent, Plater* plater,
                                           NavigateAfterImportFn navigate_after_import,
                                           SmartSlicingAvailableFn smart_slicing_available)
    : m_impl(std::make_unique<Impl>(parent, plater, std::move(navigate_after_import),
                                    std::move(smart_slicing_available)))
{}

AIDesktopFeatureHost::~AIDesktopFeatureHost() = default;

wxWindow* AIDesktopFeatureHost::model_generation_panel() const
{
    return m_impl->model_generation.panel();
}

void AIDesktopFeatureHost::start()
{
    m_impl->start();
}

void AIDesktopFeatureHost::shutdown()
{
    m_impl->shutdown();
}

} // namespace Slic3r::GUI
