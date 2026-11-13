#pragma once
#include <boost/log/trivial.hpp>
#include <chrono>
#include <cstdlib>

namespace Slic3r::GUI {
// Optional stage diagnostics share one clock and never retain file contents.
class DiagnosticStageTimer {
public:
    DiagnosticStageTimer(const char* environment, const char* label)
        : m_enabled(std::getenv(environment) != nullptr), m_label(label) {}
    void operator()(const char* stage) const
    {
        if (m_enabled)
            BOOST_LOG_TRIVIAL(info) << m_label << ": " << stage << " elapsed_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count();
    }
private:
    bool m_enabled;
    const char* m_label;
    const std::chrono::steady_clock::time_point m_start{std::chrono::steady_clock::now()};
};
} // namespace Slic3r::GUI
