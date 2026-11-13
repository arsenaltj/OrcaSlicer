#pragma once

#include <wx/event.h>

namespace Slic3r {
class SlicingProcessCompletedEvent;

namespace GUI {
// Defined and handled by Plater. Presentation adapters observe this event
// without changing native completion, cancellation or error handling.
wxDECLARE_EVENT(EVT_PROCESS_COMPLETED, Slic3r::SlicingProcessCompletedEvent);
}
}
