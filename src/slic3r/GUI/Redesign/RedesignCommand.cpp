#include "RedesignCommand.hpp"

#include <utility>

namespace Slic3r::GUI {

bool RedesignCommandRegistry::register_command(RedesignCommandId id, CanExecuteFn can_execute, ExecuteFn execute)
{
    if (!execute || m_handlers.find(id) != m_handlers.end())
        return false;

    m_handlers.emplace(id, Handler{std::move(can_execute), std::move(execute)});
    return true;
}

bool RedesignCommandRegistry::contains(RedesignCommandId id) const
{
    return m_handlers.find(id) != m_handlers.end();
}

bool RedesignCommandRegistry::can_execute(RedesignCommandId id) const
{
    const auto it = m_handlers.find(id);
    if (it == m_handlers.end())
        return false;
    return !it->second.can_execute || it->second.can_execute();
}

bool RedesignCommandRegistry::execute(RedesignCommandId id) const
{
    const auto it = m_handlers.find(id);
    if (it == m_handlers.end() || !can_execute(id))
        return false;

    it->second.execute();
    return true;
}

std::size_t RedesignCommandRegistry::size() const
{
    return m_handlers.size();
}

}
