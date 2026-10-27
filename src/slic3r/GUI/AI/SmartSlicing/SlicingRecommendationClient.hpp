#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SmartSliceTypes.hpp"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
class Http;

namespace GUI {

struct SlicingRecommendationRequest
{
    std::string request_id;
    size_t object_count {0};
    AI::SmartSlicing::ModelMetrics metrics;
    AI::SmartSlicing::MachinePreset machine;
    AI::SmartSlicing::FilamentPreset filament;
    AI::SmartSlicing::SmartAnalysisResult local_baselines;
};

struct SlicingRecommendationResult
{
    std::string request_id;
    AI::SmartSlicing::SmartAnalysisResult analysis;
    std::array<std::string, 3> summaries;
    std::vector<std::string> warnings;
    std::string diagnostic_id;
};

class SlicingRecommendationClient final
{
public:
    struct Availability
    {
        bool available {false};
        std::string reason;
        std::string diagnostic_id;
    };

    struct Error
    {
        std::string code;
        std::string message;
        std::string diagnostic_id;
        std::string goal;
        std::string parameter;
        std::string value;
    };

    class RequestHandle final
    {
    public:
        void cancel();
        bool active() const { return m_request != nullptr; }

    private:
        friend class SlicingRecommendationClient;
        explicit RequestHandle(std::shared_ptr<Http> request) : m_request(std::move(request)) {}
        std::shared_ptr<Http> m_request;
    };

    using AvailabilityFn = std::function<void(Availability)>;
    using ResultFn = std::function<void(SlicingRecommendationResult)>;
    using ErrorFn = std::function<void(Error)>;

    explicit SlicingRecommendationClient(std::string endpoint);

    std::shared_ptr<RequestHandle> get_availability(AvailabilityFn on_complete, ErrorFn on_error) const;
    std::shared_ptr<RequestHandle> recommend(
        const SlicingRecommendationRequest& request, ResultFn on_complete, ErrorFn on_error) const;

    static nlohmann::json build_request_json(const SlicingRecommendationRequest& request);
    static bool parse_availability(const std::string& body, Availability& result);
    static bool parse_result(const std::string& body, const SlicingRecommendationRequest& request,
                             SlicingRecommendationResult& result, std::string* diagnostic = nullptr,
                             Error* error = nullptr);
    static Error parse_error(const std::string& body, const std::string& transport_error, unsigned status);

private:
    std::string url() const;
    std::string m_endpoint;
};

} // namespace GUI
} // namespace Slic3r
