#pragma once

#include "AITextClient.hpp"

#include <memory>
#include <string>

namespace Slic3r::GUI {

class AITextFeatureHost final
{
public:
    explicit AITextFeatureHost(std::string endpoint);

    std::shared_ptr<AITextClient::RequestHandle> request_image_prompt_polish_availability(
        AITextClient::AvailabilityFn on_complete, AITextClient::ErrorFn on_error) const;
    std::shared_ptr<AITextClient::RequestHandle> polish_image_prompt(
        const AITextClient::PolishRequest& request,
        AITextClient::ResultFn on_complete,
        AITextClient::ErrorFn on_error) const;

private:
    AITextClient m_client;
};

} // namespace Slic3r::GUI
