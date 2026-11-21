#include "orca_mobile_slicing_sdk/resident_memory.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

using orca::mobile::slicing::detail::resident_bytes_from_status;
int main()
{
    unsigned checked = 0;
    auto expect = [&](const std::string& input, std::optional<uint64_t> expected) {
        std::istringstream stream(input);
        if (resident_bytes_from_status(stream) != expected)
            throw std::runtime_error("Unexpected resident byte interpretation");
        ++checked;
    };
    expect("Name:\tsh\nVmRSS:\t3756 kB\nThreads: 1\n", 3846144);
    expect("VmRSS: 0 kB\n", 0);
    expect("VmRSS: 2097152 kB\n", uint64_t{2} * 1024 * 1024 * 1024);
    expect("VmRSS: 18014398509481983 kB\n", UINT64_MAX - 1023);
    for (const std::string value : {"", "VmSize: 3756 kB\n", "VmRSS: -1 kB\n",
             "VmRSS: +1 kB\n", "VmRSS: 1.5 kB\n", "VmRSS: 18014398509481984 kB\n",
             "VmRSS: 9999999999999999999999999999 kB\n", "VmRSS: 1 MB\n",
             "VmRSS: 1\n", "VmRSS: 1 kB extra\n", "VmRSS: 1 kB\nVmRSS: 2 kB\n"})
        expect(value, std::nullopt);
    std::istringstream broken("VmRSS: 1 kB\n");
    broken.setstate(std::ios::badbit);
    if (resident_bytes_from_status(broken)) throw std::runtime_error("Unreadable measurement accepted");
    std::cout << checked + 1 << " resident-memory checks passed\n";
}
