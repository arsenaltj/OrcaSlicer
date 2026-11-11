#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

struct BeautyPartitionPickResult {
    bool valid {false};
    std::vector<uint8_t> selected;
    std::vector<uint32_t> complete_pieces;
};

inline BeautyPartitionPickResult toggle_beauty_partition_pick(
    const std::vector<uint32_t>& face_piece,
    const std::vector<uint8_t>& protected_faces,
    const std::vector<uint8_t>& selected,
    size_t hit_face)
{
    BeautyPartitionPickResult result;
    if (selected.size() != face_piece.size() || hit_face >= face_piece.size() ||
        face_piece[hit_face] == 0) return result;

    const uint32_t clicked = face_piece[hit_face];
    size_t selectable = 0;
    bool complete = true;
    for (size_t face = 0; face < face_piece.size(); ++face) {
        if (face_piece[face] != clicked ||
            (face < protected_faces.size() && protected_faces[face])) continue;
        ++selectable;
        complete &= selected[face] != 0;
    }
    if (!selectable) return result;

    result.valid = true;
    result.selected = selected;
    for (size_t face = 0; face < face_piece.size(); ++face) {
        if (face_piece[face] == clicked &&
            (face >= protected_faces.size() || !protected_faces[face]))
            result.selected[face] = complete ? 0 : 1;
    }

    std::unordered_map<uint32_t, std::pair<size_t, size_t>> counts;
    for (size_t face = 0; face < face_piece.size(); ++face) {
        if (!face_piece[face] ||
            (face < protected_faces.size() && protected_faces[face])) continue;
        auto& count = counts[face_piece[face]];
        ++count.first;
        count.second += result.selected[face] != 0;
    }
    for (const auto& [piece, count] : counts)
        if (count.first == count.second) result.complete_pieces.push_back(piece);
    std::sort(result.complete_pieces.begin(), result.complete_pieces.end());
    return result;
}

} // namespace Slic3r::GUI
