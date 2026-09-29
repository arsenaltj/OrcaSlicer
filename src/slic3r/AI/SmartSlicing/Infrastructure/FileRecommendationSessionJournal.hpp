#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IRecommendationSessionJournal.hpp"

#include <filesystem>

namespace Slic3r::AI::SmartSlicing {

class FileRecommendationSessionJournal final : public IRecommendationSessionJournal
{
public:
    explicit FileRecommendationSessionJournal(std::filesystem::path path);

    std::optional<RecommendationSessionJournalRecord> load() override;
    void save(const RecommendationSessionJournalRecord& record) override;
    void clear() override;

private:
    std::filesystem::path m_path;
};

} // namespace Slic3r::AI::SmartSlicing
