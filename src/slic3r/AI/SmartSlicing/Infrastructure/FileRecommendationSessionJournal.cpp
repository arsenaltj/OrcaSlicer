#include "FileRecommendationSessionJournal.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

namespace Slic3r::AI::SmartSlicing {
namespace {

using Json = nlohmann::json;

constexpr const char* JOURNAL_SCHEMA = "orcaslicer.ai-smart-slicing-runtime/v1";

const char* status_name(GoalResultStatus status)
{
    switch (status) {
    case GoalResultStatus::Analyzing: return "analyzing";
    case GoalResultStatus::Ready: return "ready";
    case GoalResultStatus::Unavailable: return "unavailable";
    case GoalResultStatus::Failed: return "failed";
    case GoalResultStatus::Stale: return "stale";
    case GoalResultStatus::Applied: return "applied";
    }
    return "invalid";
}

std::optional<GoalResultStatus> parse_status(const Json& value)
{
    if (!value.is_string())
        return std::nullopt;
    const std::string status = value.get<std::string>();
    if (status == "analyzing") return GoalResultStatus::Analyzing;
    if (status == "ready") return GoalResultStatus::Ready;
    if (status == "unavailable") return GoalResultStatus::Unavailable;
    if (status == "failed") return GoalResultStatus::Failed;
    if (status == "stale") return GoalResultStatus::Stale;
    if (status == "applied") return GoalResultStatus::Applied;
    return std::nullopt;
}

bool valid_diagnostic_code(const std::string& code)
{
    return !code.empty() && code.size() <= 128 &&
           std::all_of(code.begin(), code.end(), [](unsigned char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '_' ||
                      character == '-' || character == '.';
           });
}

Json encode_result(const RecommendationSessionJournalResult& result)
{
    std::vector<std::string> diagnostic_codes;
    for (const std::string& code : result.diagnostic_codes) {
        if (valid_diagnostic_code(code))
            diagnostic_codes.push_back(code);
        else if (std::find(diagnostic_codes.begin(), diagnostic_codes.end(), "invalid_diagnostic_code") ==
                 diagnostic_codes.end())
            diagnostic_codes.emplace_back("invalid_diagnostic_code");
    }
    return {{"goal_id", result.goal_id},
            {"status", status_name(result.status)},
            {"candidate_id", result.candidate_id},
            {"diagnostic_codes", std::move(diagnostic_codes)}};
}

std::optional<RecommendationSessionJournalResult> decode_result(const Json& value)
{
    if (!value.is_object() || value.size() != 4 || !value.contains("goal_id") || !value.contains("status") ||
        !value.contains("candidate_id") || !value.contains("diagnostic_codes") ||
        !value.at("goal_id").is_string() || !value.at("candidate_id").is_string() ||
        !value.at("diagnostic_codes").is_array())
        return std::nullopt;
    const std::optional<GoalResultStatus> status = parse_status(value.at("status"));
    if (!status)
        return std::nullopt;

    RecommendationSessionJournalResult result;
    result.goal_id = value.at("goal_id").get<std::string>();
    result.status = *status;
    result.candidate_id = value.at("candidate_id").get<std::string>();
    for (const Json& code : value.at("diagnostic_codes")) {
        if (!code.is_string())
            return std::nullopt;
        const std::string diagnostic_code = code.get<std::string>();
        if (!valid_diagnostic_code(diagnostic_code))
            return std::nullopt;
        result.diagnostic_codes.push_back(diagnostic_code);
    }
    return result;
}

Json encode(const RecommendationSessionJournalRecord& record)
{
    Json goals = Json::array();
    for (const RecommendationSessionJournalResult& goal : record.goals)
        goals.push_back(encode_result(goal));
    return {{"schema", JOURNAL_SCHEMA},
            {"workflow_id", record.workflow_id},
            {"attempt_id", record.attempt_id},
            {"workspace_revision",
             {{"model_revision", record.workspace_revision.model_revision},
              {"config_revision", record.workspace_revision.config_revision},
              {"plate_revision", record.workspace_revision.plate_revision},
              {"fingerprint", record.workspace_revision.fingerprint}}},
            {"baseline", encode_result(record.baseline)},
            {"goals", std::move(goals)},
            {"contract_version", record.contract_version},
            {"strategy_version", record.strategy_version},
            {"started_at_epoch_seconds", record.started_at_epoch_seconds},
            {"updated_at_epoch_seconds", record.updated_at_epoch_seconds}};
}

std::optional<RecommendationSessionJournalRecord> decode(const Json& value)
{
    if (!value.is_object() || value.size() != 10 || value.value("schema", "") != JOURNAL_SCHEMA ||
        !value.contains("workflow_id") || !value.contains("attempt_id") ||
        !value.contains("workspace_revision") || !value.contains("baseline") ||
        !value.contains("goals") || !value.contains("contract_version") ||
        !value.contains("strategy_version") || !value.contains("started_at_epoch_seconds") ||
        !value.contains("updated_at_epoch_seconds") || !value.at("goals").is_array() ||
        value.at("goals").size() != RECOMMENDATION_GOALS.size())
        return std::nullopt;

    const Json& revision = value.at("workspace_revision");
    if (!revision.is_object() || revision.size() != 4 || !revision.contains("model_revision") ||
        !revision.contains("config_revision") || !revision.contains("plate_revision") ||
        !revision.contains("fingerprint") || !revision.at("fingerprint").is_string())
        return std::nullopt;

    const std::optional<RecommendationSessionJournalResult> baseline = decode_result(value.at("baseline"));
    if (!baseline)
        return std::nullopt;

    RecommendationSessionJournalRecord record;
    try {
        record.workflow_id = value.at("workflow_id").get<WorkflowId>();
        record.attempt_id = value.at("attempt_id").get<AttemptId>();
        record.workspace_revision.model_revision = revision.at("model_revision").get<uint64_t>();
        record.workspace_revision.config_revision = revision.at("config_revision").get<uint64_t>();
        record.workspace_revision.plate_revision = revision.at("plate_revision").get<uint64_t>();
        record.workspace_revision.fingerprint = revision.at("fingerprint").get<std::string>();
        record.baseline = *baseline;
        record.contract_version = value.at("contract_version").get<uint32_t>();
        record.strategy_version = value.at("strategy_version").get<std::string>();
        record.started_at_epoch_seconds = value.at("started_at_epoch_seconds").get<int64_t>();
        record.updated_at_epoch_seconds = value.at("updated_at_epoch_seconds").get<int64_t>();
        for (size_t index = 0; index < record.goals.size(); ++index) {
            const std::optional<RecommendationSessionJournalResult> goal = decode_result(value.at("goals").at(index));
            if (!goal)
                return std::nullopt;
            record.goals[index] = *goal;
        }
    } catch (const Json::exception&) {
        return std::nullopt;
    }
    return record;
}

} // namespace

FileRecommendationSessionJournal::FileRecommendationSessionJournal(std::filesystem::path path) : m_path(std::move(path)) {}

std::optional<RecommendationSessionJournalRecord> FileRecommendationSessionJournal::load()
{
    std::ifstream input(m_path, std::ios::binary);
    if (!input)
        return std::nullopt;
    try {
        const Json value = Json::parse(input, nullptr, false);
        if (value.is_discarded())
            return std::nullopt;
        return decode(value);
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

void FileRecommendationSessionJournal::save(const RecommendationSessionJournalRecord& record)
{
    if (!m_path.parent_path().empty())
        std::filesystem::create_directories(m_path.parent_path());
    std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("recommendation_session_journal_open_failed");
    output << encode(record).dump(2);
    output.flush();
    if (!output)
        throw std::runtime_error("recommendation_session_journal_write_failed");
}

void FileRecommendationSessionJournal::clear()
{
    std::error_code error;
    std::filesystem::remove(m_path, error);
    if (error)
        throw std::filesystem::filesystem_error("recommendation_session_journal_clear_failed", m_path, error);
}

} // namespace Slic3r::AI::SmartSlicing
