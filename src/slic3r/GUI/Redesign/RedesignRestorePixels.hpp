#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace Slic3r::GUI::RestoreCapture {

inline constexpr std::size_t max_capture_bytes = std::size_t(64) * 1024 * 1024 * 4;

// All rectangles are physical screen pixels; DIB buffers use bottom-up BGRA.
struct Bounds {
    int left {0}, top {0}, right {0}, bottom {0};
    bool operator==(const Bounds& other) const {
        return left == other.left && top == other.top && right == other.right && bottom == other.bottom;
    }
};

inline bool make_bounds(int left, int top, int width, int height, Bounds& output)
{
    const auto right = std::int64_t(left) + width;
    const auto bottom = std::int64_t(top) + height;
    if (width <= 0 || height <= 0 || right > std::numeric_limits<int>::max() ||
        bottom > std::numeric_limits<int>::max()) return false;
    output = {left, top, static_cast<int>(right), static_cast<int>(bottom)};
    return true;
}

inline bool rgba_bytes(const Bounds& bounds, std::size_t& bytes,
                       std::size_t limit = max_capture_bytes)
{
    const auto width = std::int64_t(bounds.right) - bounds.left;
    const auto height = std::int64_t(bounds.bottom) - bounds.top;
    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max() || std::uint64_t(width) > limit / 4 ||
        std::uint64_t(height) > limit / 4 / std::uint64_t(width)) return false;
    bytes = std::size_t(width) * std::size_t(height) * 4;
    return true;
}

inline Bounds intersect(const Bounds& a, const Bounds& b)
{
    const Bounds value {std::max(a.left, b.left), std::max(a.top, b.top),
                        std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
    return value.left < value.right && value.top < value.bottom ? value : Bounds {};
}

inline bool contains(const Bounds& outer, const Bounds& inner)
{
    return inner.left < inner.right && inner.top < inner.bottom && outer.left <= inner.left &&
        outer.top <= inner.top && outer.right >= inner.right && outer.bottom >= inner.bottom;
}

struct Region { Bounds client, visible; };

enum class PlanError { None, InvalidRegion, BudgetExceeded };

struct Plan {
    Bounds capture;
    std::size_t bitmap_bytes {0}, output_bytes {0}, controls {0};
    PlanError error {PlanError::None};

    bool include(const Region& region, std::size_t retained_capacity = 0,
                 std::size_t limit = max_capture_bytes)
    {
        if (error != PlanError::None) return false;
        std::size_t bytes = 0;
        if (!contains(region.client, region.visible) || !rgba_bytes(region.client, bytes, limit)) {
            error = PlanError::InvalidRegion;
            return false;
        }
        const auto next = controls == 0 ? region.visible : Bounds {
            std::min(capture.left, region.visible.left), std::min(capture.top, region.visible.top),
            std::max(capture.right, region.visible.right), std::max(capture.bottom, region.visible.bottom)};
        std::size_t bitmap = 0;
        const auto output = std::max(bytes, retained_capacity);
        if (!rgba_bytes(next, bitmap, limit) || output > limit || output_bytes > limit - output ||
            bitmap > limit - output_bytes - output) {
            error = PlanError::BudgetExceeded;
            return false;
        }
        capture = next;
        bitmap_bytes = bitmap;
        output_bytes += output;
        ++controls;
        return true;
    }
};

inline bool copy_region(const unsigned char* source, std::size_t source_bytes, const Bounds& capture,
                        const Region& region, std::uint32_t background_bgra,
                        std::vector<unsigned char>& output)
{
    std::size_t capture_bytes = 0, output_bytes = 0;
    if (!source || !rgba_bytes(capture, capture_bytes) || source_bytes < capture_bytes ||
        !rgba_bytes(region.client, output_bytes) || !contains(capture, region.visible) ||
        !contains(region.client, region.visible)) return false;

    // Release an insufficient old allocation before growing; the batch budget
    // includes each retained capacity and the one temporary screen bitmap.
    if (output.capacity() < output_bytes) std::vector<unsigned char>().swap(output);
    output.resize(output_bytes);
    for (std::size_t offset = 0; offset < output_bytes; offset += 4)
        for (unsigned channel = 0; channel < 4; ++channel)
            output[offset + channel] = static_cast<unsigned char>(background_bgra >> (8 * channel));

    const auto source_stride = std::size_t(std::int64_t(capture.right) - capture.left) * 4;
    const auto output_stride = std::size_t(std::int64_t(region.client.right) - region.client.left) * 4;
    const auto copy_bytes = std::size_t(std::int64_t(region.visible.right) - region.visible.left) * 4;
    const auto source_x = std::size_t(std::int64_t(region.visible.left) - capture.left) * 4;
    const auto output_x = std::size_t(std::int64_t(region.visible.left) - region.client.left) * 4;
    const auto source_bottom = std::size_t(std::int64_t(capture.bottom) - region.visible.bottom);
    const auto output_bottom = std::size_t(std::int64_t(region.client.bottom) - region.visible.bottom);
    const auto rows = std::size_t(std::int64_t(region.visible.bottom) - region.visible.top);
    for (std::size_t row = 0; row < rows; ++row)
        std::memcpy(output.data() + (output_bottom + row) * output_stride + output_x,
                    source + (source_bottom + row) * source_stride + source_x, copy_bytes);
    return true;
}

struct FrameIdentity {
    std::uintptr_t window {0}, parent {0}, root {0};
    int width {0}, height {0}, dpi_x {0}, dpi_y {0};

    bool matches(const FrameIdentity& current) const {
        return window != 0 && root != 0 && window == current.window && parent == current.parent && root == current.root &&
            width > 0 && height > 0 && width == current.width && height == current.height &&
            dpi_x > 0 && dpi_y > 0 && dpi_x == current.dpi_x && dpi_y == current.dpi_y;
    }
};

enum class ReadError { None, ScreenDC, MemoryDC, Bitmap, Selection, Copy, Flush };

// The Win32 adapter owns platform calls. This scope owns their release order,
// including partially acquired resources, and permits one copy/flush attempt.
template<class Api> class ScreenBitmap {
public:
    explicit ScreenBitmap(Api& api) : m_api(api) {}
    ScreenBitmap(const ScreenBitmap&) = delete;
    ScreenBitmap& operator=(const ScreenBitmap&) = delete;
    ~ScreenBitmap() {
        if (m_selected) m_api.select(m_target, m_previous);
        if (m_bitmap) m_api.delete_bitmap(m_bitmap);
        if (m_target) m_api.delete_dc(m_target);
        if (m_source) m_api.release_screen(m_source);
    }

    bool prepare(const Bounds& bounds) {
        std::size_t bytes = 0;
        if (m_prepared || m_source || m_error != ReadError::None || !rgba_bytes(bounds, bytes)) return false;
        m_bounds = bounds;
        m_source = m_api.screen();
        if (!m_source) return fail(ReadError::ScreenDC);
        m_target = m_api.memory(m_source);
        if (!m_target) return fail(ReadError::MemoryDC);
        m_bitmap = m_api.bitmap(m_source, bounds.right - bounds.left, bounds.bottom - bounds.top, &m_bits);
        if (!m_bitmap || !m_bits) return fail(ReadError::Bitmap);
        m_previous = m_api.select(m_target, m_bitmap);
        if (!m_api.valid_selection(m_previous)) return fail(ReadError::Selection);
        m_selected = true;
        m_prepared = true;
        return true;
    }

    bool copy() {
        if (!m_prepared || m_copy_attempted) return false;
        m_copy_attempted = true;
        m_copied = m_api.copy(m_target, m_source, m_bounds);
        return m_copied || fail(ReadError::Copy);
    }
    bool flush() {
        if (!m_copied || m_flush_attempted) return false;
        m_flush_attempted = true;
        m_flushed = m_api.flush();
        return m_flushed || fail(ReadError::Flush);
    }
    const unsigned char* data() const { return m_flushed ? static_cast<const unsigned char*>(m_bits) : nullptr; }
    ReadError error() const { return m_error; }

private:
    bool fail(ReadError error) { m_error = error; return false; }
    Api& m_api;
    typename Api::DC m_source {}, m_target {};
    typename Api::Bitmap m_bitmap {};
    typename Api::Object m_previous {};
    void* m_bits {nullptr};
    Bounds m_bounds;
    ReadError m_error {ReadError::None};
    bool m_selected {false}, m_prepared {false}, m_copy_attempted {false}, m_copied {false};
    bool m_flush_attempted {false}, m_flushed {false};
};

} // namespace Slic3r::GUI::RestoreCapture
