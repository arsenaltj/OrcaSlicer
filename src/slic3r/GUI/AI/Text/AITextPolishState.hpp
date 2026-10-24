#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace Slic3r::GUI {

class AITextPolishState
{
public:
    std::uint64_t begin()
    {
        m_active = true;
        return ++m_sequence;
    }

    bool finish(std::uint64_t sequence, const std::string& original)
    {
        if (!m_active || sequence != m_sequence)
            return false;
        m_active = false;
        if (!m_original)
            m_original = original;
        return true;
    }

    bool fail(std::uint64_t sequence)
    {
        if (!m_active || sequence != m_sequence)
            return false;
        m_active = false;
        return true;
    }

    void cancel(bool clear_original)
    {
        ++m_sequence;
        m_active = false;
        if (clear_original)
            m_original.reset();
    }

    std::optional<std::string> restore()
    {
        auto original = std::move(m_original);
        m_original.reset();
        return original;
    }

    bool active() const { return m_active; }
    bool can_restore() const { return m_original.has_value(); }
    std::uint64_t sequence() const { return m_sequence; }

private:
    std::uint64_t m_sequence { 0 };
    bool m_active { false };
    std::optional<std::string> m_original;
};

} // namespace Slic3r::GUI
