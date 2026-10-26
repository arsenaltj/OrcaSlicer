#include "orca/mobile/device_sdk_c.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

TEST_CASE("Mobile SDK C ABI wakeup interrupts a blocking poll", "[MobileDeviceSDK][CABI][threading]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    std::atomic<orca_mobile_device_error_t> result{ORCA_MOBILE_DEVICE_E_INTERNAL};
    std::thread waiter([&] {
        orca_mobile_device_event_t event{};
        event.struct_size = static_cast<uint32_t>(sizeof(event));
        result.store(orca_mobile_device_poll_event(session, ORCA_MOBILE_DEVICE_WAIT_INFINITE, &event));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(orca_mobile_device_wakeup(session) == ORCA_MOBILE_DEVICE_OK);
    waiter.join();
    REQUIRE(result.load() == ORCA_MOBILE_DEVICE_E_WAKEUP);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI cancels pending non-SYNC facade tasks across threads", "[MobileDeviceSDK][CABI][threading]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    const uint32_t kinds[] = {
        ORCA_MOBILE_DEVICE_TASK_KIND_CONNECT,
        ORCA_MOBILE_DEVICE_TASK_KIND_COMMAND,
        ORCA_MOBILE_DEVICE_TASK_KIND_TRANSFER,
    };
    const char payload[] = "{\"device_id\":\"printer-1\",\"command_id\":\"cmd-1\"}";
    for (const uint32_t kind : kinds) {
        orca_mobile_device_task_request_t request{};
        request.struct_size = static_cast<uint32_t>(sizeof(request));
        request.kind = kind;
        request.payload_utf8.data = payload;
        request.payload_utf8.size = static_cast<uint32_t>(sizeof(payload) - 1u);
        orca_mobile_device_task_t* task = nullptr;
        REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_OK);

        auto submitted = orca_mobile_device_event_t{};
        submitted.struct_size = static_cast<uint32_t>(sizeof(submitted));
        REQUIRE(orca_mobile_device_poll_event(session, 1000, &submitted) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(submitted.type == ORCA_MOBILE_DEVICE_EVENT_TASK_SUBMITTED);
        REQUIRE(submitted.task == task);
        REQUIRE(orca_mobile_device_event_clear(&submitted) == ORCA_MOBILE_DEVICE_OK);

        std::atomic<orca_mobile_device_error_t> result{ORCA_MOBILE_DEVICE_E_INTERNAL};
        std::thread canceler([&] { result.store(orca_mobile_device_task_cancel(task)); });
        canceler.join();
        REQUIRE(result.load() == ORCA_MOBILE_DEVICE_OK);

        auto canceled = orca_mobile_device_event_t{};
        canceled.struct_size = static_cast<uint32_t>(sizeof(canceled));
        REQUIRE(orca_mobile_device_poll_event(session, 1000, &canceled) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(canceled.type == ORCA_MOBILE_DEVICE_EVENT_TASK_CANCELED);
        REQUIRE(canceled.task == task);
        REQUIRE(canceled.status == ORCA_MOBILE_DEVICE_E_CANCELED);
        REQUIRE(orca_mobile_device_event_clear(&canceled) == ORCA_MOBILE_DEVICE_OK);
        uint32_t state = ORCA_MOBILE_DEVICE_TASK_PENDING;
        REQUIRE(orca_mobile_device_task_get_state(task, &state) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(state == ORCA_MOBILE_DEVICE_TASK_CANCELED);
        REQUIRE(orca_mobile_device_task_destroy(task) == ORCA_MOBILE_DEVICE_OK);
    }

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}
TEST_CASE("Mobile SDK C ABI wakeup broadcasts to all waiting pollers", "[MobileDeviceSDK][CABI][threading]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    std::atomic<int> ready{0};
    std::atomic<orca_mobile_device_error_t> first{ORCA_MOBILE_DEVICE_E_INTERNAL};
    std::atomic<orca_mobile_device_error_t> second{ORCA_MOBILE_DEVICE_E_INTERNAL};
    auto wait_for_wakeup = [&](std::atomic<orca_mobile_device_error_t>& result) {
        orca_mobile_device_event_t event{};
        event.struct_size = static_cast<uint32_t>(sizeof(event));
        ready.fetch_add(1);
        result.store(orca_mobile_device_poll_event(session, ORCA_MOBILE_DEVICE_WAIT_INFINITE, &event));
    };
    std::thread first_waiter(wait_for_wakeup, std::ref(first));
    std::thread second_waiter(wait_for_wakeup, std::ref(second));
    for (int attempt = 0; attempt != 100 && ready.load() != 2; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    REQUIRE(ready.load() == 2);
    REQUIRE(orca_mobile_device_wakeup(session) == ORCA_MOBILE_DEVICE_OK);
    first_waiter.join();
    second_waiter.join();
    REQUIRE(first.load() == ORCA_MOBILE_DEVICE_E_WAKEUP);
    REQUIRE(second.load() == ORCA_MOBILE_DEVICE_E_WAKEUP);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI session destruction wakes and invalidates pollers", "[MobileDeviceSDK][CABI][threading]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    std::atomic<orca_mobile_device_error_t> result{ORCA_MOBILE_DEVICE_E_INTERNAL};
    std::thread waiter([&] {
        orca_mobile_device_event_t event{};
        event.struct_size = static_cast<uint32_t>(sizeof(event));
        result.store(orca_mobile_device_poll_event(session, ORCA_MOBILE_DEVICE_WAIT_INFINITE, &event));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    waiter.join();
    REQUIRE(result.load() == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_wakeup(session) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_poll_event(session, 0, nullptr) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}
TEST_CASE("Mobile SDK C ABI serializes concurrent destruction", "[MobileDeviceSDK][CABI][threading]")
{
    orca_mobile_device_t* sdk = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);

    std::atomic<bool> start{false};
    std::atomic<orca_mobile_device_error_t> first{ORCA_MOBILE_DEVICE_E_INTERNAL};
    std::atomic<orca_mobile_device_error_t> second{ORCA_MOBILE_DEVICE_E_INTERNAL};
    auto destroy_sdk = [&](std::atomic<orca_mobile_device_error_t>& result) {
        while (!start.load())
            std::this_thread::yield();
        result.store(orca_mobile_device_destroy(sdk));
    };
    std::thread first_destroyer(destroy_sdk, std::ref(first));
    std::thread second_destroyer(destroy_sdk, std::ref(second));
    start.store(true);
    first_destroyer.join();
    second_destroyer.join();

    const orca_mobile_device_error_t first_result = first.load();
    const orca_mobile_device_error_t second_result = second.load();
    REQUIRE((first_result == ORCA_MOBILE_DEVICE_OK || second_result == ORCA_MOBILE_DEVICE_OK));
    REQUIRE((first_result == ORCA_MOBILE_DEVICE_E_BUSY || first_result == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE ||
             second_result == ORCA_MOBILE_DEVICE_E_BUSY || second_result == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE));
}

TEST_CASE("Mobile SDK C ABI does not double destroy a session", "[MobileDeviceSDK][CABI][threading]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    std::atomic<bool> start{false};
    std::atomic<orca_mobile_device_error_t> first{ORCA_MOBILE_DEVICE_E_INTERNAL};
    std::atomic<orca_mobile_device_error_t> second{ORCA_MOBILE_DEVICE_E_INTERNAL};
    auto destroy_session = [&](std::atomic<orca_mobile_device_error_t>& result) {
        while (!start.load())
            std::this_thread::yield();
        result.store(orca_mobile_device_session_destroy(session));
    };
    std::thread first_destroyer(destroy_session, std::ref(first));
    std::thread second_destroyer(destroy_session, std::ref(second));
    start.store(true);
    first_destroyer.join();
    second_destroyer.join();

    const orca_mobile_device_error_t first_result = first.load();
    const orca_mobile_device_error_t second_result = second.load();
    REQUIRE((first_result == ORCA_MOBILE_DEVICE_OK || second_result == ORCA_MOBILE_DEVICE_OK));
    REQUIRE((first_result == ORCA_MOBILE_DEVICE_E_BUSY || first_result == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE ||
             second_result == ORCA_MOBILE_DEVICE_E_BUSY || second_result == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE));
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}
