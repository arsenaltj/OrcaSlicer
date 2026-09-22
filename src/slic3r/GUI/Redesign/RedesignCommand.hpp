#pragma once

#include <cstddef>
#include <functional>
#include <map>

namespace Slic3r::GUI {

enum class RedesignCommandId
{
    NewProject,
    OpenProject,
    SaveProject,
    SaveProjectAs,
    ImportModel,
    DeleteSelection,
    DuplicateSelection,
    Undo,
    Redo,
    ApplyTransform,
    SelectPrinter,
    SelectMaterial,
    SelectProcess,
    StartSlice,
    CancelSlice,
    ExportGcode,
    SendToPrinter
};

class RedesignCommandRegistry final
{
public:
    using CanExecuteFn = std::function<bool()>;
    using ExecuteFn = std::function<void()>;

    bool register_command(RedesignCommandId id, CanExecuteFn can_execute, ExecuteFn execute);
    bool contains(RedesignCommandId id) const;
    bool can_execute(RedesignCommandId id) const;
    bool execute(RedesignCommandId id) const;
    std::size_t size() const;

private:
    struct Handler
    {
        CanExecuteFn can_execute;
        ExecuteFn execute;
    };

    std::map<RedesignCommandId, Handler> m_handlers;
};

}
