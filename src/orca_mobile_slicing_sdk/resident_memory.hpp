#pragma once
#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

namespace orca::mobile::slicing::detail {
// proc status publishes KiB explicitly. statm uses kernel pages, which need not
// equal the application's emulated page size (Android's x86 16 KiB image).
inline std::optional<uint64_t> resident_bytes_from_status(std::istream& input)
{
    std::optional<uint64_t> result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, 6, "VmRSS:") != 0) continue;
        if (result) return std::nullopt;
        std::istringstream fields(line.substr(6));
        std::string number, unit, extra;
        if (!(fields >> number >> unit) || unit != "kB" || (fields >> extra) || number.empty())
            return std::nullopt;
        uint64_t kib = 0;
        constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max() / 1024;
        for (const char digit : number) {
            if (digit < '0' || digit > '9') return std::nullopt;
            const uint64_t value = static_cast<uint64_t>(digit - '0');
            if (kib > maximum / 10 || (kib == maximum / 10 && value > maximum % 10))
                return std::nullopt;
            kib = kib * 10 + value;
        }
        result = kib * 1024;
    }
    if (input.bad()) return std::nullopt;
    return result;
}
}
