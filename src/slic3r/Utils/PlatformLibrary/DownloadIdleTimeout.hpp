#pragma once
#include <chrono>

namespace Slic3r::PlatformLibrary {
class DownloadIdleTimeout {
public:
    using Clock = std::chrono::steady_clock;

    explicit DownloadIdleTimeout(Clock::time_point now = Clock::now()) : m_last_write(now) {}
    void bytes_written(Clock::time_point now) { m_last_write = now; }
    bool expired(Clock::time_point now) const { return now - m_last_write >= std::chrono::seconds(20); }

private:
    Clock::time_point m_last_write;
};
}
