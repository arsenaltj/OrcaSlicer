#pragma once

#include <algorithm>
#include <cstddef>

namespace Slic3r::GUI {

struct ImageHistoryGrid
{
    int edge;
    int rows;
    static ImageHistoryGrid fit(int width, int height, int gap)
    {
        const int edge = std::max(1, std::min((width - gap) / 2, height));
        return {edge, std::clamp((height + gap) / (edge + gap), 1, 5)};
    }
};

// UI-only paging over the filtered history, without changing stored records.
class ImageHistoryPagination
{
public:
    size_t page() const { return m_page; }
    size_t capacity() const { return m_capacity; }
    size_t pages() const { return m_count / m_capacity + (m_count % m_capacity != 0); }
    size_t begin() const { return m_page * m_capacity; }
    size_t end() const { return std::min(m_count, begin() + m_capacity); }
    bool contains(size_t index) const { return index >= begin() && index < end(); }
    void go_to(size_t page) { m_page = std::min(page, pages() == 0 ? 0 : pages() - 1); }
    void set_count(size_t count, bool reset = false)
    {
        m_count = count;
        go_to(reset ? 0 : m_page);
    }
    void set_capacity(size_t capacity)
    {
        const size_t anchor = begin();
        m_capacity = std::max(size_t(1), capacity);
        go_to(anchor / m_capacity);
    }
private:
    size_t m_count {0}, m_capacity {10}, m_page {0};
};

}
