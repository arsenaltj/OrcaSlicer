#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace Slic3r::GUI {

enum class RedesignSliceStatus
{
    Idle,
    Dirty,
    Slicing,
    Complete,
    Failed,
    Cancelled
};

struct RedesignStateSnapshot
{
    std::uint64_t revision { 0 };
    bool project_open { false };
    bool project_dirty { false };
    bool has_selection { false };
    bool can_undo { false };
    bool can_redo { false };
    RedesignSliceStatus slice_status { RedesignSliceStatus::Idle };
};

class RedesignStateStore final
{
public:
    using Listener = std::function<void(const RedesignStateSnapshot&)>;

    const RedesignStateSnapshot& snapshot() const;
    void publish(RedesignStateSnapshot snapshot);
    void subscribe(Listener listener);

private:
    RedesignStateSnapshot m_snapshot;
    std::vector<Listener> m_listeners;
};

}
