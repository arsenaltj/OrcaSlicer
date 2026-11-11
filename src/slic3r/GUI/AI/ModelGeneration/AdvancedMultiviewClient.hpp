#pragma once

#include "AdvancedCreationTypes.hpp"
#include "slic3r/Utils/Http.hpp"
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <atomic>

namespace Slic3r::GUI {

// Dedicated client: requests and cancellation never share the smart client's handles.
class AdvancedMultiviewClient {
public:
    using Json = nlohmann::json;
    using Complete = std::function<void(Json)>;
    using Error = std::function<void(std::string)>;
    explicit AdvancedMultiviewClient(std::string endpoint);
    ~AdvancedMultiviewClient();
    void request(const std::string& method, const std::string& route, const Json& payload,
                 Complete complete, Error error);
    void create_group(const std::string& request_id, const AdvancedCreation::InputDraft& draft,
                      Complete complete, Error error);
    void update_group(const std::string& group_id, std::uint64_t expected_revision,
                      const AdvancedCreation::InputDraft& draft, Complete complete, Error error);
    void download_asset(const std::string& group_id, const std::string& asset_id,
                        const std::string& expected_sha256, const boost::filesystem::path& destination,
                        std::function<void()> complete, Error error);
    void cancel();
    static Json design_json(const AdvancedCreation::DesignDraft& draft);
    static std::optional<AdvancedCreation::GroupSnapshot> parse_group(const Json& group,
                                                                     const boost::filesystem::path& cache_root);
    static boost::filesystem::path asset_path(const boost::filesystem::path& cache_root,
                                              const std::string& group, const std::string& asset);
    static bool valid_id(const std::string& value);
    static bool cached_asset_matches(const boost::filesystem::path& path, const std::string& expected);

private:
    bool available(Error error) const;
    void perform(Http http, Complete complete, Error error);
    std::string m_endpoint;
    struct PendingRequest { std::shared_ptr<Http> http; std::shared_ptr<std::atomic<bool>> done; };
    std::vector<PendingRequest> m_requests;
    void track(Http http, std::shared_ptr<std::atomic<bool>> done);
};

} // namespace Slic3r::GUI
