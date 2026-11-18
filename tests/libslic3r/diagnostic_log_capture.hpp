#pragma once

#include <boost/make_shared.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions/message.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <libslic3r/Utils.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::Test {

struct DiagnosticLogBackend : boost::log::sinks::basic_sink_backend<boost::log::sinks::synchronized_feeding>
{
    std::string prefix;
    std::vector<std::string> messages;
    bool fail = false;
    void consume(const boost::log::record_view& record)
    {
        const auto message = record[boost::log::expressions::smessage];
        if (!message || message.get().find(prefix) == std::string::npos) return;
        if (fail) throw std::runtime_error("Diagnostic sink unavailable");
        messages.push_back(message.get());
    }
};

struct DiagnosticLogCapture
{
    using Sink = boost::log::sinks::synchronous_sink<DiagnosticLogBackend>;
    boost::shared_ptr<Sink> sink = boost::make_shared<Sink>();
    unsigned previous_level = Slic3r::get_logging_level();
    bool previous_enabled = boost::log::core::get()->get_logging_enabled();
    explicit DiagnosticLogCapture(std::string prefix = "[CgalRepair]")
    {
        sink->locked_backend()->prefix = std::move(prefix);
        Slic3r::set_logging_level(3);
        boost::log::core::get()->set_logging_enabled(true);
        boost::log::core::get()->add_sink(sink);
    }
    ~DiagnosticLogCapture()
    {
        boost::log::core::get()->remove_sink(sink);
        boost::log::core::get()->set_logging_enabled(previous_enabled);
        Slic3r::set_logging_level(previous_level);
    }
};

} // namespace Slic3r::Test
