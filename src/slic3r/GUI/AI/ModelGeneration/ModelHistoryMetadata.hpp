#pragma once

#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "ModelHistoryRecord.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <functional>
#include <map>
#include <string>
#include <utility>

namespace Slic3r::GUI {

// One history load owns this record until its GUI callback consumes it. It is
// neither a list-summary cache nor an independent writer. Large accepted and draft
// fields are frozen; the existing ordered writer restores the complete document.
class ModelHistoryMetadata
{
public:
    void clear()
    {
        m_path.clear();
        m_fingerprint.clear();
        m_fields = nullptr;
        m_encoded.clear();
    }

    bool read(const boost::filesystem::path& path, const std::function<bool()>& canceled = {})
    {
        clear();
        const auto stopped = [&] { return canceled && canceled(); };
        if (stopped()) return false;
        boost::system::error_code error;
        const auto bytes = boost::filesystem::file_size(path, error);
        // Keep the same bound as saved preview-state restoration.
        if (error || bytes == 0 || bytes > 128ULL * 1024 * 1024) return false;
        boost::filesystem::ifstream input(path, std::ios::binary);
        if (!input) return false;
        std::string contents(static_cast<size_t>(bytes), '\0');
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (input.gcount() != static_cast<std::streamsize>(contents.size()) ||
            input.bad() || input.peek() != std::char_traits<char>::eof() || stopped())
            return false;
        const auto fingerprint = digest(contents);
        // The hash belongs to the exact bytes parsed, even if a writer changed
        // the file while this worker was reading it.
        if (fingerprint.empty() || AI::model_artifact_sha256(path) != fingerprint) return false;
        nlohmann::json fields;
        std::map<std::string, std::string> encoded;
        if (!ModelHistoryRecord::parse(contents, fields, encoded, canceled) || stopped()) return false;
        m_path = path;
        m_fingerprint = fingerprint;
        m_fields = std::move(fields);
        m_encoded = std::move(encoded);
        return true;
    }

    const nlohmann::json& restoration_fields() const { return m_fields; }

    // A changed/missing record takes the existing fresh-read path. Failed
    // handoffs leave the outputs untouched, and every handoff is single-use.
    bool take_if_current(const boost::filesystem::path& path, nlohmann::json& fields,
                         std::map<std::string, std::string>& encoded)
    {
        const bool current = path == m_path && m_fields.is_object() && !m_fingerprint.empty() &&
                             AI::model_artifact_sha256(path) == m_fingerprint;
        if (current) {
            fields = std::move(m_fields);
            encoded = std::move(m_encoded);
        }
        clear();
        return current;
    }

private:
    static std::string digest(const std::string& contents)
    {
        unsigned char bytes[EVP_MAX_MD_SIZE];
        unsigned length = 0;
        if (EVP_Digest(contents.data(), contents.size(), bytes, &length, EVP_sha256(), nullptr) != 1)
            return {};
        constexpr char hex[] = "0123456789abcdef";
        std::string value(length * 2, '\0');
        for (unsigned i = 0; i < length; ++i) {
            value[2 * i] = hex[bytes[i] >> 4];
            value[2 * i + 1] = hex[bytes[i] & 15];
        }
        return value;
    }

    boost::filesystem::path m_path;
    std::string m_fingerprint;
    nlohmann::json m_fields;
    std::map<std::string, std::string> m_encoded;
};

} // namespace Slic3r::GUI
