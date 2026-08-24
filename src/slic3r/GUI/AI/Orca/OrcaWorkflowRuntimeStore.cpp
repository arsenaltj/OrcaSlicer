#include "OrcaWorkflowRuntimeStore.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Slic3r::GUI {
namespace {

using namespace AI::SmartSlicing;
using Json = nlohmann::json;

constexpr uintmax_t MAX_JOURNAL_BYTES = 64 * 1024;
constexpr uint64_t FNV1A_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV1A_PRIME = 1099511628211ULL;

std::string instance_token(const std::string& executable_instance)
{
    uint64_t hash = FNV1A_OFFSET_BASIS;
    for (const unsigned char byte : executable_instance) {
        hash ^= byte;
        hash *= FNV1A_PRIME;
    }
    std::ostringstream token;
    token << std::hex << std::setfill('0') << std::setw(16) << hash;
    return token.str();
}

Json to_json(const WorkflowRuntimeRecord& record)
{
    if (record.candidates.size() > MAX_COMPARABLE_CANDIDATES || record.detail.size() > 256 ||
        record.revision.fingerprint.size() > 128)
        throw std::runtime_error("Smart-slicing runtime metadata exceeds its bounds.");
    Json candidates = Json::array();
    for (const WorkflowRuntimeCandidate& candidate : record.candidates) {
        if (candidate.id.empty() || candidate.id.size() > 128)
            throw std::runtime_error("Smart-slicing runtime candidate id exceeds its bounds.");
        candidates.push_back({{"id", candidate.id}, {"goal", static_cast<int>(candidate.goal)},
                              {"status", static_cast<int>(candidate.status)}});
    }
    return {{"version", 1},
            {"workflow_id", record.workflow_id},
            {"state", static_cast<int>(record.state)},
            {"revision", {{"model", record.revision.model_revision}, {"config", record.revision.config_revision},
                           {"plate", record.revision.plate_revision}, {"fingerprint", record.revision.fingerprint}}},
            {"candidates", std::move(candidates)},
            {"detail", record.detail},
            {"updated_at", record.updated_at_epoch_seconds}};
}

WorkflowRuntimeRecord from_json(const Json& value)
{
    if (value.at("version").get<int>() != 1)
        throw std::runtime_error("Unsupported smart-slicing runtime journal version.");
    WorkflowRuntimeRecord record;
    record.workflow_id = value.at("workflow_id").get<WorkflowId>();
    const int state = value.at("state").get<int>();
    if (state < static_cast<int>(WorkflowState::Idle) || state > static_cast<int>(WorkflowState::Failed))
        throw std::runtime_error("Invalid smart-slicing runtime state.");
    record.state = static_cast<WorkflowState>(state);
    const Json& revision = value.at("revision");
    record.revision = {revision.at("model").get<uint64_t>(), revision.at("config").get<uint64_t>(),
                       revision.at("plate").get<uint64_t>(), revision.at("fingerprint").get<std::string>()};
    record.detail = value.value("detail", std::string{});
    if (record.detail.size() > 256 || record.revision.fingerprint.size() > 128)
        throw std::runtime_error("Smart-slicing runtime journal contains oversized metadata.");
    record.updated_at_epoch_seconds = value.value("updated_at", int64_t{0});
    const Json candidates = value.value("candidates", Json::array());
    if (!candidates.is_array() || candidates.size() > MAX_COMPARABLE_CANDIDATES)
        throw std::runtime_error("Smart-slicing runtime journal has too many candidates.");
    for (const Json& candidate : candidates) {
        const std::string id = candidate.at("id").get<std::string>();
        const int goal = candidate.at("goal").get<int>();
        const int status = candidate.at("status").get<int>();
        if (id.empty() || id.size() > 128 || goal < static_cast<int>(CandidateGoal::Stability) ||
            goal > static_cast<int>(CandidateGoal::MaterialSaving) || status < static_cast<int>(CandidateStatus::Draft) ||
            status > static_cast<int>(CandidateStatus::Failed))
            throw std::runtime_error("Invalid smart-slicing runtime candidate metadata.");
        record.candidates.push_back({id, static_cast<CandidateGoal>(goal), static_cast<CandidateStatus>(status)});
    }
    return record;
}

boost::filesystem::path sibling_generation(const boost::filesystem::path& journal_path, const char* suffix)
{
    boost::filesystem::path path = journal_path;
    path += suffix;
    return path;
}

std::optional<WorkflowRuntimeRecord> load_record(const boost::filesystem::path& path)
{
    if (!boost::filesystem::exists(path))
        return std::nullopt;
    if (boost::filesystem::file_size(path) > MAX_JOURNAL_BYTES)
        throw std::runtime_error("Smart-slicing runtime journal exceeds its size limit.");
    boost::nowide::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Smart-slicing runtime journal is unreadable.");
    return from_json(Json::parse(stream));
}

} // namespace

boost::filesystem::path orca_workflow_runtime_journal_path(
    const boost::filesystem::path& data_directory, const std::string& executable_instance)
{
    return data_directory / "cache" /
           ("OrcaSlicer-smart-slicing-runtime-v1-" + instance_token(executable_instance) + ".json");
}

OrcaWorkflowRuntimeStore::OrcaWorkflowRuntimeStore(boost::filesystem::path journal_path)
    : m_journal_path(std::move(journal_path))
{}

std::optional<AI::SmartSlicing::WorkflowRuntimeRecord> OrcaWorkflowRuntimeStore::load()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const boost::filesystem::path backup = sibling_generation(m_journal_path, ".bak");
    std::optional<WorkflowRuntimeRecord> record;
    try {
        record = load_record(m_journal_path);
    } catch (...) {
        if (!boost::filesystem::exists(backup))
            throw;
    }
    if (!record)
        record = load_record(backup);
    if (record)
        m_known_workflow_id = record->workflow_id;
    return record;
}

void OrcaWorkflowRuntimeStore::save(const AI::SmartSlicing::WorkflowRuntimeRecord& record)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string serialized = to_json(record).dump();
    if (serialized.size() > MAX_JOURNAL_BYTES)
        throw std::runtime_error("Smart-slicing runtime journal exceeds its size limit.");
    boost::filesystem::create_directories(m_journal_path.parent_path());
    const boost::filesystem::path temporary = sibling_generation(m_journal_path, ".tmp");
    const boost::filesystem::path backup = sibling_generation(m_journal_path, ".bak");
    {
        boost::nowide::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("Smart-slicing runtime journal is unwritable.");
        stream << serialized;
        if (!stream)
            throw std::runtime_error("Smart-slicing runtime journal write failed.");
    }
    boost::system::error_code error;
    boost::filesystem::remove(backup, error);
    if (error) {
        boost::system::error_code cleanup_error;
        boost::filesystem::remove(temporary, cleanup_error);
        throw boost::filesystem::filesystem_error(
            "Unable to clear the previous smart-slicing runtime journal backup.", error);
    }

    const bool had_journal = boost::filesystem::exists(m_journal_path);
    if (had_journal) {
        boost::filesystem::rename(m_journal_path, backup, error);
        if (error) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(temporary, cleanup_error);
            throw boost::filesystem::filesystem_error(
                "Unable to preserve the previous smart-slicing runtime journal.", error);
        }
    }

    error.clear();
    boost::filesystem::rename(temporary, m_journal_path, error);
    if (error) {
        const boost::system::error_code publish_error = error;
        if (had_journal) {
            error.clear();
            boost::filesystem::rename(backup, m_journal_path, error);
            if (error)
                throw boost::filesystem::filesystem_error(
                    "Unable to restore the previous smart-slicing runtime journal.", error);
        }
        boost::filesystem::remove(temporary, error);
        throw boost::filesystem::filesystem_error(
            "Unable to publish smart-slicing runtime journal.", publish_error);
    }
    boost::filesystem::remove(backup, error);
    m_known_workflow_id = record.workflow_id;
}

void OrcaWorkflowRuntimeStore::clear(AI::SmartSlicing::WorkflowId workflow_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (workflow_id != 0 && m_known_workflow_id != 0 && workflow_id != m_known_workflow_id)
        return;
    boost::system::error_code error;
    boost::filesystem::remove(m_journal_path, error);
    error.clear();
    boost::filesystem::remove(sibling_generation(m_journal_path, ".tmp"), error);
    error.clear();
    boost::filesystem::remove(sibling_generation(m_journal_path, ".bak"), error);
    m_known_workflow_id = 0;
}

} // namespace Slic3r::GUI
