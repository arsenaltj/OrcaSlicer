#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <thread>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <stb_dxt/stb_dxt.h>

namespace Slic3r::GUI {

// Each band begins on a DXT block row, so neither source pixels nor output
// blocks overlap. The last band retains rygCompress's partial-edge behavior.
inline void compress_font_dxt5(unsigned char* dst, unsigned char* src, int width, int height, int& compressed_size)
{
    if (width <= 0 || height <= 0) {
        compressed_size = 0;
        return;
    }

    const size_t blocks_per_row = size_t((width + 3) / 4);
    const size_t block_rows = size_t((height + 3) / 4);
    const size_t row_bytes = blocks_per_row * 16;
    const unsigned hardware = std::thread::hardware_concurrency();
    const unsigned workers = std::min(4u, hardware == 0 ? 1u : hardware);
    if (row_bytes * block_rows < 1024 * 1024 || workers < 2) {
        rygCompress(dst, src, width, height, 1, compressed_size);
        return;
    }

    const size_t grain = std::max<size_t>(32, (block_rows + workers * 8 - 1) / (workers * 8));
    tbb::task_arena arena(static_cast<int>(workers));
    arena.execute([&] {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, block_rows, grain), [&](const tbb::blocked_range<size_t>& range) {
            const size_t first_row = range.begin() * 4;
            const size_t band_height = std::min<size_t>(height - first_row, (range.end() - range.begin()) * 4);
            int band_bytes = 0;
            rygCompress(dst + range.begin() * row_bytes, src + first_row * size_t(width) * 4,
                width, static_cast<int>(band_height), 1, band_bytes);
            assert(size_t(band_bytes) == (range.end() - range.begin()) * row_bytes);
        });
    });
    compressed_size = static_cast<int>(row_bytes * block_rows);
}

} // namespace Slic3r::GUI
