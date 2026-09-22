#include "RedesignState.hpp"

#include <utility>

namespace Slic3r::GUI {

const RedesignStateSnapshot& RedesignStateStore::snapshot() const
{
    return m_snapshot;
}

void RedesignStateStore::publish(RedesignStateSnapshot snapshot)
{
    if (snapshot.revision <= m_snapshot.revision)
        snapshot.revision = m_snapshot.revision + 1;

    m_snapshot = std::move(snapshot);
    for (const Listener& listener : m_listeners) {
        if (listener)
            listener(m_snapshot);
    }
}

void RedesignStateStore::subscribe(Listener listener)
{
    if (listener)
        m_listeners.emplace_back(std::move(listener));
}

}
