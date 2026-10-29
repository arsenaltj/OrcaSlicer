#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Slic3r::GUI {

// Presentation time only: provider progress and task state never depend on it.
class ModelGenerationAnimationClock
{
public:
    void synchronize(bool active, bool visible, double now)
    {
        if (!active) {
            m_elapsed = 0.0;
            m_running = false;
            return;
        }
        m_elapsed = elapsed(now);
        m_last = now;
        m_running = visible;
    }

    double elapsed(double now) const
    {
        return m_elapsed + (m_running && std::isfinite(now) && now > m_last ? now - m_last : 0.0);
    }
    bool running() const { return m_running; }

private:
    double m_elapsed {0.0};
    double m_last {0.0};
    bool m_running {false};
};

class ModelGenerationStarfield
{
public:
    static constexpr std::size_t particle_count = 100;
    static constexpr double cycle_seconds = 9.0;
    struct Point { double x, y, radius, opacity; bool warm; };

    ModelGenerationStarfield()
    {
        std::uint32_t seed = 0x81e47a3u;
        auto next = [&seed] {
            seed = 1664525u * seed + 1013904223u;
            return double(seed >> 8) / double(1u << 24);
        };
        for (auto& star : m_stars)
            star = {next() * 2.0 * pi, std::sqrt(0.08 + 0.92 * next()),
                    next() * 2.0 * pi, 0.7 + 0.9 * next(), next() > 0.65};
    }

    Point sample(std::size_t index, double seconds) const
    {
        const auto& star = m_stars[index % particle_count];
        const double time = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
        const double phase = std::fmod(time, cycle_seconds) * 2.0 * pi / cycle_seconds;
        // Smooth at both ends of the cycle: dispersed -> gathered -> dispersed.
        const double spread = 0.43 + 0.57 * (1.0 + std::cos(phase)) / 2.0;
        const double angle = star.angle + 0.18 * std::sin(phase) +
                             0.10 * std::sin(phase + star.twinkle);
        const double radius = 0.46 * star.distance * spread;
        return {0.5 + radius * std::cos(angle), 0.5 + radius * std::sin(angle), star.radius,
                0.48 + 0.35 * (1.0 + std::sin(phase * 2.0 + star.twinkle)) / 2.0, star.warm};
    }

private:
    static constexpr double pi = 3.14159265358979323846;
    struct Star { double angle, distance, twinkle, radius; bool warm; };
    std::array<Star, particle_count> m_stars;
};

} // namespace Slic3r::GUI
