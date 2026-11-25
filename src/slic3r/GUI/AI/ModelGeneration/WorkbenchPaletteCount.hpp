#pragma once

#include "ModelPreviewPalette.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace Slic3r::GUI {

// GUI-thread input queue. Keep the active slot occupied until completion,
// including when its model or selection has already been invalidated.
class WorkbenchPaletteCountRequests {
public:
    using Clock = std::chrono::steady_clock;
    enum class Kind { Adjust, Reset };
    struct Request {
        std::uint64_t sequence;
        int count;
        bool final;
        Kind kind;
    };

    void request(int count, bool final, Kind kind = Kind::Adjust)
    {
        if (m_latest && m_latest->kind == kind && m_latest->count == count && (!m_latest->final || final)) {
            m_latest->final |= final;
            return;
        }
        m_latest = Request{++m_sequence, count, final, kind};
    }

    bool pending() const { return m_latest && m_latest->sequence != m_completed; }
    bool tracking() const { return m_latest && !m_latest->final; }
    const std::optional<Request>& latest() const { return m_latest; }

    std::optional<std::chrono::milliseconds> wait(Clock::time_point now) const
    {
        if (!pending() || m_running) return std::nullopt;
        if (m_latest->final || !m_last_started) return std::chrono::milliseconds::zero();
        const auto remaining = *m_last_started + std::chrono::milliseconds(100) - now;
        return std::max(std::chrono::milliseconds::zero(), std::chrono::ceil<std::chrono::milliseconds>(remaining));
    }

    std::optional<Request> start(Clock::time_point now)
    {
        const auto delay = wait(now);
        if (!delay || *delay != std::chrono::milliseconds::zero()) return std::nullopt;
        m_running = m_latest->sequence;
        m_last_started = now;
        return m_latest;
    }

    bool complete(const Request& request)
    {
        if (m_running != request.sequence) return false;
        m_running = 0;
        if (!m_latest || m_latest->sequence != request.sequence) return false;
        m_completed = request.sequence;
        return true;
    }

    void invalidate() { m_latest.reset(); }

private:
    std::optional<Request> m_latest;
    std::optional<Clock::time_point> m_last_started;
    std::uint64_t m_sequence {0}, m_running {0}, m_completed {0};
};

using WorkbenchNativePaletteBasis = std::function<std::optional<PreviewPalette::AutomaticColorBasis>(size_t)>;

struct WorkbenchPaletteCountSeed {
    using Palette = std::vector<PreviewPalette::Color>;
    bool automatic {false};
    bool fidelity {true};
    std::string geometry_id;
    Palette colors, mapping, semantic_colors, semantic_mapping;
    PreviewPalette::RetainedRows locks {};
};

struct WorkbenchPaletteCountResult {
    WorkbenchPaletteCountSeed palette;
    PreviewPalette::AutomaticColorBasis basis;
    int requested_count {0};
    std::optional<size_t> available_colors;
    bool limited_by_locks {false};
    bool used_native_basis {false};
};

inline void use_workbench_card_rows(WorkbenchPaletteCountSeed& seed, const PreviewPalette::ColorCardRemap& card)
{
    if (card.target_palette.empty() || !card.valid(seed.mapping.size())) return;
    auto mapping = card.target_palette;
    auto semantic_colors = card.target_palette;
    auto semantic_mapping = card.target_palette;
    for (size_t index = 0; index < card.source_to_target_slot.size(); ++index) {
        const size_t row = card.source_to_target_slot[index];
        mapping[row] = seed.mapping[index];
        if (index < seed.semantic_colors.size()) semantic_colors[row] = seed.semantic_colors[index];
        semantic_mapping[row] = index < seed.semantic_mapping.size() ? seed.semantic_mapping[index] : mapping[row];
    }
    seed.colors = card.target_palette;
    seed.mapping = std::move(mapping);
    seed.semantic_colors = std::move(semantic_colors);
    seed.semantic_mapping = std::move(semantic_mapping);
}

inline PreviewPalette::AutomaticColorBasis workbench_basis_for_palette(
    const PreviewPalette::Histogram& histogram, const PreviewPalette::AutomaticColorUnits* units,
    const std::string& geometry_id, size_t count, const std::vector<PreviewPalette::Color>& colors)
{
    if (colors.empty()) return {};
    PreviewPalette::AutomaticColorBasis basis;
    basis.geometry_id = geometry_id;
    basis.source_color_signature = histogram.signature();
    basis.requested_count = std::clamp(count, size_t(1), PreviewPalette::max_preview_colors);
    basis.source_palette = colors;
    basis.source_area = histogram.group_weights(colors);
    PreviewPalette::attach_automatic_color_units(basis, units);
    basis.basis_signature = PreviewPalette::automatic_basis_signature(basis);
    return basis;
}

inline WorkbenchPaletteCountResult prepare_workbench_palette_reset(
    const std::string& geometry, const PreviewPalette::Histogram& histogram,
    const PreviewPalette::AutomaticColorUnits* units, const WorkbenchNativePaletteBasis& native_basis = {},
    const PreviewPalette::AutomaticColorBasis* initial = nullptr)
{
    const size_t count = PreviewPalette::initial_trial_color_count();
    WorkbenchPaletteCountResult result;
    result.requested_count = int(count);
    const auto matches_default = [&geometry, count](const PreviewPalette::AutomaticColorBasis& basis) {
        return basis.valid() && basis.geometry_id == geometry && basis.requested_count == count;
    };
    if (initial && matches_default(*initial))
        result.basis = *initial;
    else if (auto native = native_basis ? native_basis(count) : std::nullopt; native && matches_default(*native))
        result.basis = std::move(*native);
    else
        result.basis = PreviewPalette::make_automatic_color_basis(histogram, count, geometry, true, units);

    auto& palette = result.palette;
    palette.automatic = true;
    palette.geometry_id = geometry;
    palette.colors = palette.mapping = palette.semantic_colors = palette.semantic_mapping = result.basis.source_palette;
    return result;
}

inline WorkbenchPaletteCountResult prepare_workbench_palette_count(WorkbenchPaletteCountSeed seed, int count,
    const PreviewPalette::Histogram& histogram, const PreviewPalette::AutomaticColorUnits* units,
    const WorkbenchNativePaletteBasis& native_basis = {})
{
    WorkbenchPaletteCountResult result;
    const int requested = std::clamp(count, 1, int(PreviewPalette::max_preview_colors));
    result.requested_count = requested;
    std::vector<size_t> retained;
    for (size_t row = 0; row < std::min(seed.colors.size(), seed.locks.size()); ++row)
        if (seed.locks[row]) retained.push_back(row);
    const size_t target = std::max(size_t(requested), retained.size());
    result.limited_by_locks = target != size_t(requested);

    if (seed.automatic) {
        WorkbenchPaletteCountSeed::Palette locked;
        for (const size_t row : retained) locked.push_back(seed.colors[row]);
        auto native = locked.empty() && native_basis ? native_basis(target) : std::nullopt;
        if (native && native->valid() && native->geometry_id == seed.geometry_id && native->requested_count == target) {
            seed.colors = native->source_palette;
            seed.locks = {};
            result.basis = std::move(*native);
            result.used_native_basis = true;
        } else {
            auto colors = histogram.trial_palette(target, locked, seed.fidelity);
            if (colors.size() != target) {
                result.available_colors = colors.size();
                return result;
            }
            PreviewPalette::place_retained_colors(colors, seed.colors, seed.locks);
            seed.colors = std::move(colors);
            for (size_t index = seed.colors.size(); index < seed.locks.size(); ++index) seed.locks[index] = false;
            result.basis = workbench_basis_for_palette(histogram, units, seed.geometry_id, target, seed.colors);
        }
        seed.mapping = seed.semantic_colors = seed.semantic_mapping = seed.colors;
    } else {
        auto positions = PreviewPalette::retained_row_positions(target, retained);
        size_t next = 0;
        PreviewPalette::RetainedRows locks {};
        for (size_t row = 0; row < positions.size(); ++row) {
            if (positions[row]) {
                locks[row] = true;
                continue;
            }
            while (next < seed.colors.size() && seed.locks[next]) ++next;
            if (next < seed.colors.size()) positions[row] = next++;
        }
        const auto suggestions = target > seed.colors.size()
            ? histogram.trial_palette(PreviewPalette::max_preview_colors, {}, true)
            : WorkbenchPaletteCountSeed::Palette{};
        const auto reorder = [&](const auto& colors, const auto& fallback) {
            WorkbenchPaletteCountSeed::Palette reordered;
            reordered.reserve(target);
            for (size_t row = 0; row < target; ++row) {
                const auto index = positions[row];
                if (index)
                    reordered.push_back(*index < colors.size() ? colors[*index] : fallback[*index]);
                else
                    reordered.push_back(suggestions.empty() ? PreviewPalette::Color{.5f, .5f, .5f} :
                        suggestions[row % suggestions.size()]);
            }
            return reordered;
        };
        if (seed.mapping.size() != seed.colors.size()) seed.mapping = seed.colors;
        seed.semantic_colors = reorder(seed.semantic_colors, seed.colors);
        seed.semantic_mapping = reorder(seed.semantic_mapping, seed.mapping);
        seed.mapping = reorder(seed.mapping, seed.colors);
        seed.colors = reorder(seed.colors, seed.colors);
        seed.locks = locks;
        result.basis = workbench_basis_for_palette(histogram, units, seed.geometry_id, target, seed.mapping);
    }
    result.palette = std::move(seed);
    return result;
}

} // namespace Slic3r::GUI
