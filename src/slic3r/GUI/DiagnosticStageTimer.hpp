#pragma once
#include <boost/log/trivial.hpp>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

namespace Slic3r::GUI {
// Optional stage diagnostics share one clock and never retain file contents.
class DiagnosticStageTimer {
public:
    DiagnosticStageTimer(const char* environment, const char* label, std::string operation = {})
        : m_enabled(std::getenv(environment) != nullptr), m_label(label), m_operation(std::move(operation)) {}
    void operator()(const char* stage) const
    {
        if (m_enabled)
            BOOST_LOG_TRIVIAL(info) << m_label << ": " << stage << " elapsed_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count()
                << (m_operation.empty() ? std::string{} : " operation=" + m_operation);
    }
private:
    bool m_enabled;
    const char* m_label;
    const std::string m_operation;
    const std::chrono::steady_clock::time_point m_start{std::chrono::steady_clock::now()};
};
} // namespace Slic3r::GUI
