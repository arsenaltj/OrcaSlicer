#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::GUI {
// Presentation identities only; business requests remain owned by the service.
class LibraryPresentationChannel {
public:
    using Json = nlohmann::json;
    enum class Acceptance { Rejected, Ready, Command };
    Json begin(std::string nonce) {
        m_nonce = std::move(nonce); ++m_document; m_ready = false;
        m_account.clear(); m_account_generation = m_request = 0; m_pending.clear();
        return {{"type","init"},{"nonce",m_nonce},{"documentGeneration",m_document}};
    }
    Acceptance accept(const Json& value) {
        if (!value.is_object() || value.value("nonce",std::string()) != m_nonce ||
            !value.contains("documentGeneration") || !value["documentGeneration"].is_number_unsigned() ||
            value["documentGeneration"].get<std::uint64_t>() != m_document) return Acceptance::Rejected;
        if (value.value("command",std::string()) == "ready") {
            if (m_ready) return Acceptance::Rejected;
            m_ready = true; return Acceptance::Ready;
        }
        if (!m_ready || value.value("accountTag",std::string()) != m_account ||
            value.value("accountGeneration",std::uint64_t(0)) != m_account_generation ||
            !value.contains("requestGeneration") || !value["requestGeneration"].is_number_unsigned())
            return Acceptance::Rejected;
        const auto request = value["requestGeneration"].get<std::uint64_t>();
        if (request <= m_request) return Acceptance::Rejected;
        m_request = request;
        m_pending.erase(std::remove_if(m_pending.begin(),m_pending.end(),[](const Json& item) {
            return item.value("type",std::string()) != "account";
        }),m_pending.end());
        return Acceptance::Command;
    }
    void push(Json value) {
        if (!m_ready) return;
        if (value.value("type",std::string()) == "account") {
            const auto tag = value.value("accountTag",std::string());
            const auto generation = value.value("accountGeneration",std::uint64_t(0));
            if (tag != m_account || generation != m_account_generation) m_pending.clear();
            m_account = tag; m_account_generation = generation;
        }
        value["documentGeneration"] = m_document;
        value["accountTag"] = m_account;
        value["accountGeneration"] = m_account_generation;
        value["requestGeneration"] = m_request;
        const auto type = value.value("type",std::string());
        // A minimized WebView may defer presentation for a long time. Retain
        // only the latest snapshots/progress, never duplicate thumbnail bytes.
        const bool snapshot = type == "account" || type == "local" || type == "loading" ||
            type == "statistics" || type == "taxonomy";
        if (snapshot || type == "image") {
            const auto key = value.value("key",std::string());
            m_pending.erase(std::remove_if(m_pending.begin(),m_pending.end(),[&](const Json& item) {
                return item.value("type",std::string()) == type &&
                    (snapshot || item.value("key",std::string()) == key);
            }),m_pending.end());
        }
        m_pending.push_back(std::move(value));
    }
    void retry(std::vector<Json> values) {
        // RunScript can fail while the native document is being restored.
        // Preserve its batch for the next explicit presentation opportunity.
        for (auto& value : values)
            if (value.value("documentGeneration",std::uint64_t(0)) == m_document &&
                value.value("accountTag",std::string()) == m_account &&
                value.value("accountGeneration",std::uint64_t(0)) == m_account_generation &&
                value.value("requestGeneration",std::uint64_t(0)) == m_request)
                push(std::move(value));
    }
    std::vector<Json> take() { auto result=std::move(m_pending);m_pending.clear();return result; }
    bool ready() const { return m_ready; }
private:
    std::string m_nonce,m_account;
    std::uint64_t m_document {0},m_account_generation {0},m_request {0};
    bool m_ready {false};
    std::vector<Json> m_pending;
};
}
