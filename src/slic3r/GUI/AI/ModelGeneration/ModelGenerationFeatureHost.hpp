#pragma once

#include <functional>
#include <memory>
#include <string>

class wxWindow;

namespace Slic3r::GUI {

class ModelGenerationPanel;
class Plater;

class ModelGenerationFeatureHost final
{
public:
    using NavigateAfterImportFn = std::function<void(bool slice)>;
    using RetryServiceFn = std::function<void()>;

    ModelGenerationFeatureHost(wxWindow* parent, Plater* plater, NavigateAfterImportFn navigate_after_import,
                               RetryServiceFn retry_service);
    ~ModelGenerationFeatureHost();

    ModelGenerationFeatureHost(const ModelGenerationFeatureHost&) = delete;
    ModelGenerationFeatureHost& operator=(const ModelGenerationFeatureHost&) = delete;

    wxWindow* panel() const;
    void set_service_availability(bool available, const std::string& message);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
