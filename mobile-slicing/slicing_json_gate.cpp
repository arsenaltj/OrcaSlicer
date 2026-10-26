#include "orca/mobile/slicing_sdk_c.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

// Device-only integration gate. Uses the public ABI and the same shared library
// as the app; writes native result separately from stdout/stderr diagnostics.
int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: gate WORKSPACE REQUEST_JSON RESULT_JSON [CANCEL_AFTER_MS]\n"); return 64; }
    std::ifstream source(argv[2], std::ios::binary);
    if (!source) { std::fprintf(stderr, "request file cannot be read\n"); return 66; }
    const std::string request((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    auto* context = orca_mobile_slicing_create(argv[1]);
    if (!context) { std::fprintf(stderr, "context creation failed\n"); return 70; }
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::thread cancel;
    if (argc > 4) {
        const long delay = std::stol(argv[4]);
        cancel = std::thread([&] {
            std::unique_lock<std::mutex> lock(mutex);
            if (!done.wait_for(lock, std::chrono::milliseconds(delay), [&] { return finished; })) {
                lock.unlock();
                orca_mobile_slicing_cancel(context);
            }
        });
    }
    char* result = nullptr;
    const int status = orca_mobile_slicing_execute(context, request.c_str(), [](const char* event, void*) {
        if (event) { std::fputs(event, stderr); std::fputc('\n', stderr); std::fflush(stderr); }
    }, nullptr, &result);
    { std::lock_guard<std::mutex> lock(mutex); finished = true; }
    done.notify_all();
    if (cancel.joinable()) cancel.join();
    int output_status = 0;
    if (result) {
        std::ofstream output(argv[3], std::ios::binary);
        output << result;
        if (!output) output_status = 74;
        orca_mobile_slicing_free_string(result);
    } else output_status = 70;
    orca_mobile_slicing_destroy(context);
    std::fprintf(stderr, "gate_status=%d\n", status);
    return output_status ? output_status : status;
}
