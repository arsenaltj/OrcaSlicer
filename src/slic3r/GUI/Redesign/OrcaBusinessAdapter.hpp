#pragma once

#include "RedesignCommand.hpp"
#include "RedesignState.hpp"

namespace Slic3r::GUI {

class Plater;

class OrcaBusinessAdapter final
{
public:
    explicit OrcaBusinessAdapter(Plater* plater = nullptr);

    void set_plater(Plater* plater);
    bool attached() const;

    void register_commands(RedesignCommandRegistry& registry) const;
    RedesignStateSnapshot snapshot() const;
    void publish_state(RedesignStateStore& store) const;

private:
    bool can_start_slice() const;
    bool can_export_gcode() const;

    Plater* m_plater { nullptr };
};

}
