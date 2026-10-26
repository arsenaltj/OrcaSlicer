#pragma once

#include <cstdint>
#include <optional>

namespace Slic3r::GUI {

struct RedesignPageRoute
{
    int page {-1};
    int view {-1};

    bool operator==(const RedesignPageRoute& other) const
    {
        return page == other.page && view == other.view;
    }
};

// A small generation-checked coordinator for retained redesign pages. The UI
// owns preparation and visibility; this class makes late callbacks harmless.
class RedesignPageTransition
{
public:
    using Token = std::uint64_t;

    std::optional<Token> begin(RedesignPageRoute source, RedesignPageRoute target)
    {
        if (m_active) return std::nullopt;
        m_active = true;
        m_ready = false;
        m_source = source;
        m_target = target;
        return ++m_generation;
    }

    bool current(Token token) const { return m_active && token == m_generation; }
    bool active() const { return m_active; }
    bool ready() const { return m_active && m_ready; }
    RedesignPageRoute source() const { return m_source; }
    RedesignPageRoute target() const { return m_target; }

    bool mark_ready(Token token)
    {
        if (!current(token)) return false;
        m_ready = true;
        return true;
    }

    bool commit(Token token)
    {
        if (!current(token) || !m_ready) return false;
        m_active = false;
        m_ready = false;
        return true;
    }

    bool rollback(Token token)
    {
        if (!current(token)) return false;
        m_active = false;
        m_ready = false;
        return true;
    }

private:
    Token m_generation {0};
    bool m_active {false};
    bool m_ready {false};
    RedesignPageRoute m_source;
    RedesignPageRoute m_target;
};

} // namespace Slic3r::GUI
