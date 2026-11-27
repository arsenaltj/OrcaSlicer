#pragma once
#include "libslic3r.h"
#include <cmath>
#include <vector>
namespace Slic3r {
// Shared native layout normalization; configured/default purge estimates are not calibration.
inline void normalize_filament_flush_layout(std::vector<double>& matrix,std::vector<double>& multipliers,std::vector<double>& volumes,size_t num_filaments,size_t nozzle_nums,size_t to_delete_filament_id)
{
    std::vector<double> old_matrix = matrix;
    size_t old_nozzle_nums = multipliers.size();
    size_t old_number_of_filaments = old_nozzle_nums ? size_t(sqrt(old_matrix.size() / old_nozzle_nums) + EPSILON) : 0;

    if (old_nozzle_nums != nozzle_nums) {
        std::vector<double>& f_multiplier = multipliers;
        f_multiplier.resize(nozzle_nums, 1.f);
    }

    // Different slot/nozzle layouts can have the same flattened matrix size.
    // Normalize the vector independently and compare the layout as well.
    std::vector<double>& filaments = volumes;
    const double first_load = filaments.empty() ? 140. : filaments[0];
    const double first_unload = filaments.size() < 2 ? 140. : filaments[1];
    while (filaments.size() < 2 * num_filaments)
        filaments.push_back(filaments.size() % 2 ? first_unload : first_load);
    filaments.resize(2 * num_filaments);
    if (old_number_of_filaments != num_filaments || old_nozzle_nums != nozzle_nums ||
        old_matrix.size() != num_filaments * num_filaments * nozzle_nums) {

        size_t old_matrix_size = old_number_of_filaments * old_number_of_filaments;
        size_t new_matrix_size = num_filaments * num_filaments;
        std::vector<double> new_matrix(new_matrix_size * nozzle_nums, 0);
        for (unsigned int i = 0; i < num_filaments; ++i)
            for (unsigned int j = 0; j < num_filaments; ++j) {
                if (i < old_number_of_filaments && j < old_number_of_filaments) {
                    unsigned int old_i = i >= to_delete_filament_id ? i + 1 : i;
                    unsigned int old_j = j >= to_delete_filament_id ? j + 1 : j;
                    for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id) {
                        // Orca: only copy from old_matrix when the old layout actually has data
                        // for this nozzle slot; otherwise initialize from the per-filament
                        // flush volumes the same way the (i,j) out-of-range branch does.
                        if (nozzle_id < old_nozzle_nums) {
                            new_matrix[i * num_filaments + j + new_matrix_size * nozzle_id] = old_matrix[old_i * old_number_of_filaments + old_j + old_matrix_size * nozzle_id];
                        } else {
                            new_matrix[i * num_filaments + j + new_matrix_size * nozzle_id] = (i == j ? 0. : filaments[2 * i] + filaments[2 * j + 1]);
                        }
                    }
                } else {
                    for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id) {
                        new_matrix[i * num_filaments + j + new_matrix_size * nozzle_id] = (i == j ? 0. : filaments[2 * i] + filaments[2 * j + 1]);
                    }
                }
            }
        matrix = new_matrix;
    }
}
}
