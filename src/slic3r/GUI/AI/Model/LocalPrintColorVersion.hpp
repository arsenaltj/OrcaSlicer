#pragma once

#include "ModelArtifact.hpp"
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Slic3r::GUI::LocalPrintColorVersion {
// Own only this attempt's new files. Release them immediately after native
// adoption/paint commits, before fallible UI refresh. Older versions are never
// part of this cleanup and a rejected attempt keeps its in-memory candidate.
class Pending {
public:
    explicit Pending(const boost::filesystem::path& directory)
        : m_id(boost::filesystem::unique_path("orca-color-%%%%%%%%-%%%%%%%%").string()),
          m_asset(directory / (m_id + ".glb")), m_record(directory / (m_id + ".json")),
          m_temporary(directory / (m_id + ".json.tmp"))
    {
        boost::filesystem::create_directories(directory);
    }
    Pending(const Pending&) = delete;
    Pending& operator=(const Pending&) = delete;
    ~Pending() noexcept
    {
        boost::system::error_code error;
        if (m_temporary_owned) boost::filesystem::remove(m_temporary, error);
        if (!m_kept && m_record_owned) boost::filesystem::remove(m_record, error);
        if (!m_kept && m_asset_owned) boost::filesystem::remove(m_asset, error);
    }
    const std::string& id() const noexcept { return m_id; }
    const boost::filesystem::path& asset() const noexcept { return m_asset; }
    const boost::filesystem::path& record() const noexcept { return m_record; }
    const boost::filesystem::path& temporary() const noexcept { return m_temporary; }

    void publish(const indexed_triangle_set& mesh, const std::vector<RGBA>& colors,
                 nlohmann::json record)
    {
        if (m_asset_owned || m_kept || boost::filesystem::exists(m_record) ||
            boost::filesystem::exists(m_temporary))
            throw std::runtime_error("The color version output already exists; retry with a new version.");
        std::string error;
        if (!AI::write_model_artifact(m_asset, mesh, colors, error)) throw std::runtime_error(error);
        m_asset_owned = true;
        record["derived_sha256"] = AI::model_artifact_sha256(m_asset);
        boost::filesystem::ofstream stream(m_temporary);
        if (!stream) throw std::runtime_error("Unable to save the color version.");
        m_temporary_owned = true;
        stream << record.dump(2);
        stream.close();
        if (!stream) throw std::runtime_error("Unable to save the color version.");
        boost::filesystem::rename(m_temporary, m_record);
        m_record_owned = true;
        m_temporary_owned = false;
    }
    void keep() noexcept { m_kept = true; }
private:
    std::string m_id;
    boost::filesystem::path m_asset, m_record, m_temporary;
    bool m_asset_owned {false}, m_record_owned {false}, m_temporary_owned {false}, m_kept {false};
};
} // namespace Slic3r::GUI::LocalPrintColorVersion
