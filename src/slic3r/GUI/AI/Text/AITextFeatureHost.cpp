#include "AITextFeatureHost.hpp"

#include <utility>

namespace Slic3r::GUI {

AITextFeatureHost::AITextFeatureHost(std::string endpoint) : m_client(std::move(endpoint)) {}

std::shared_ptr<AITextClient::RequestHandle> AITextFeatureHost::request_image_prompt_polish_availability(
    AITextClient::AvailabilityFn on_complete, AITextClient::ErrorFn on_error) const
{
    return m_client.get_image_prompt_polish_availability(std::move(on_complete), std::move(on_error));
}

std::shared_ptr<AITextClient::RequestHandle> AITextFeatureHost::polish_image_prompt(
    const AITextClient::PolishRequest& request,
    AITextClient::ResultFn on_complete,
    AITextClient::ErrorFn on_error) const
{
    return m_client.polish_image_prompt(request, std::move(on_complete), std::move(on_error));
}

} // namespace Slic3r::GUI
