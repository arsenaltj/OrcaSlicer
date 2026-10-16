#include "orca/sdk_c.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

TEST_CASE("Mobile SDK C ABI wakeup interrupts a blocking poll", "[MobileSDK][CABI][threading]")
{
    orca_sdk_t* sdk = nullptr;
    orca_sdk_session_t* session = nullptr;
    REQUIRE(orca_sdk_create(nullptr, &sdk) == ORCA_SDK_OK);
    REQUIRE(orca_sdk_session_create(sdk, nullptr, &session) == ORCA_SDK_OK);

    std::atomic<orca_sdk_error_t> result{ORCA_SDK_E_INTERNAL};
    std::thread waiter([&] {
        orca_sdk_event_t event{};
        event.struct_size = static_cast<uint32_t>(sizeof(event));
        result.store(orca_sdk_poll_event(session, ORCA_SDK_WAIT_INFINITE, &event));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(orca_sdk_wakeup(session) == ORCA_SDK_OK);
    waiter.join();
    REQUIRE(result.load() == ORCA_SDK_E_WAKEUP);

    REQUIRE(orca_sdk_session_destroy(session) == ORCA_SDK_OK);
    REQUIRE(orca_sdk_destroy(sdk) == ORCA_SDK_OK);
}

TEST_CASE("Mobile SDK C ABI cancels a pending task across threads", "[MobileSDK][CABI][threading]")
{
    orca_sdk_t* sdk = nullptr;
    orca_sdk_session_t* session = nullptr;
    REQUIRE(orca_sdk_create(nullptr, &sdk) == ORCA_SDK_OK);
    REQUIRE(orca_sdk_session_create(sdk, nullptr, &session) == ORCA_SDK_OK);

    orca_sdk_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = ORCA_SDK_TASK_KIND_SYNC;
    orca_sdk_task_t* task = nullptr;
    REQUIRE(orca_sdk_session_submit_task(session, &request, &task) == ORCA_SDK_OK);

    orca_sdk_event_t event{};
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    REQUIRE(orca_sdk_poll_event(session, 1000, &event) == ORCA_SDK_OK);
    REQUIRE(event.type == ORCA_SDK_EVENT_TASK_SUBMITTED);

    std::atomic<orca_sdk_error_t> result{ORCA_SDK_E_INTERNAL};
    std::thread canceler([&] { result.store(orca_sdk_task_cancel(task)); });
    canceler.join();
    REQUIRE(result.load() == ORCA_SDK_OK);

    REQUIRE(orca_sdk_poll_event(session, 1000, &event) == ORCA_SDK_OK);
    REQUIRE(event.type == ORCA_SDK_EVENT_TASK_CANCELED);
    REQUIRE(event.status == ORCA_SDK_E_CANCELED);
    REQUIRE(orca_sdk_task_get_state(task, nullptr) == ORCA_SDK_E_INVALID_ARGUMENT);

    REQUIRE(orca_sdk_task_destroy(task) == ORCA_SDK_OK);
    REQUIRE(orca_sdk_session_destroy(session) == ORCA_SDK_OK);
    REQUIRE(orca_sdk_destroy(sdk) == ORCA_SDK_OK);
}
TEST_CASE("Mobile SDK C ABI wakeup broadcasts to all waiting pollers", "[MobileSDK][CABI][threading]")
{
    orca_sdk_t* sdk = nullptr;
    orca_sdk_session_t* session = nullptr;
    REQUIRE(orca_sdk_create(nullptr, &sdk) == ORCA_SDK_OK);
    REQUIRE(orca_sdk_session_create(sdk, nullptr, &session) == ORCA_SDK_OK);

    std::atomic<int> ready{0};
    std::atomic<orca_sdk_error_t> first{ORCA_SDK_E_INTERNAL};
    std::atomic<orca_sdk_error_t> second{ORCA_SDK_E_INTERNAL};
    auto wait_for_wakeup = [&](std::atomic<orca_sdk_error_t>& result) {
        orca_sdk_event_t event{};
        event.struct_size = static_cast<uint32_t>(sizeof(event));
        ready.fetch_add(1);
        result.store(orca_sdk_poll_event(session, ORCA_SDK_WAIT_INFINITE, &event));
    };
    std::thread first_waiter(wait_for_wakeup, std::ref(first));
    std::thread second_waiter(wait_for_wakeup, std::ref(second));
    for (int attempt = 0; attempt != 100 && ready.load() != 2; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    REQUIRE(ready.load() == 2);
    REQUIRE(orca_sdk_wakeup(session) == ORCA_SDK_OK);
    first_waiter.join();
    second_waiter.join();
    REQUIRE(first.load() == ORCA_SDK_E_WAKEUP);
    REQUIRE(second.load() == ORCA_SDK_E_WAKEUP);

    REQUIRE(orca_sdk_session_destroy(session) == ORCA_SDK_OK);
    REQUIRE(orca_sdk_destroy(sdk) == ORCA_SDK_OK);
}

TEST_CASE("Mobile SDK C ABI session destruction wakes and invalidates pollers", "[MobileSDK][CABI][threading]")
{
    orca_sdk_t* sdk = nullptr;
    orca_sdk_session_t* session = nullptr;
    REQUIRE(orca_sdk_create(nullptr, &sdk) == ORCA_SDK_OK);
    REQUIRE(orca_sdk_session_create(sdk, nullptr, &session) == ORCA_SDK_OK);

    std::atomic<orca_sdk_error_t> result{ORCA_SDK_E_INTERNAL};
    std::thread waiter([&] {
        orca_sdk_event_t event{};
        event.struct_size = static_cast<uint32_t>(sizeof(event));
        result.store(orca_sdk_poll_event(session, ORCA_SDK_WAIT_INFINITE, &event));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    REQUIRE(orca_sdk_session_destroy(session) == ORCA_SDK_OK);
    waiter.join();
    REQUIRE(result.load() == ORCA_SDK_E_INVALID_HANDLE);
    REQUIRE(orca_sdk_wakeup(session) == ORCA_SDK_E_INVALID_HANDLE);
    REQUIRE(orca_sdk_poll_event(session, 0, nullptr) == ORCA_SDK_E_INVALID_HANDLE);
    REQUIRE(orca_sdk_destroy(sdk) == ORCA_SDK_OK);
}
TEST_CASE("Mobile SDK C ABI serializes concurrent destruction", "[MobileSDK][CABI][threading]")
{
    orca_sdk_t* sdk = nullptr;
    REQUIRE(orca_sdk_create(nullptr, &sdk) == ORCA_SDK_OK);

    std::atomic<bool> start{false};
    std::atomic<orca_sdk_error_t> first{ORCA_SDK_E_INTERNAL};
    std::atomic<orca_sdk_error_t> second{ORCA_SDK_E_INTERNAL};
    auto destroy_sdk = [&](std::atomic<orca_sdk_error_t>& result) {
        while (!start.load())
            std::this_thread::yield();
        result.store(orca_sdk_destroy(sdk));
    };
    std::thread first_destroyer(destroy_sdk, std::ref(first));
    std::thread second_destroyer(destroy_sdk, std::ref(second));
    start.store(true);
    first_destroyer.join();
    second_destroyer.join();

    const orca_sdk_error_t first_result = first.load();
    const orca_sdk_error_t second_result = second.load();
    REQUIRE((first_result == ORCA_SDK_OK || second_result == ORCA_SDK_OK));
    REQUIRE((first_result == ORCA_SDK_E_BUSY || first_result == ORCA_SDK_E_INVALID_HANDLE ||
             second_result == ORCA_SDK_E_BUSY || second_result == ORCA_SDK_E_INVALID_HANDLE));
}

TEST_CASE("Mobile SDK C ABI does not double destroy a session", "[MobileSDK][CABI][threading]")
{
    orca_sdk_t* sdk = nullptr;
    orca_sdk_session_t* session = nullptr;
    REQUIRE(orca_sdk_create(nullptr, &sdk) == ORCA_SDK_OK);
    REQUIRE(orca_sdk_session_create(sdk, nullptr, &session) == ORCA_SDK_OK);

    std::atomic<bool> start{false};
    std::atomic<orca_sdk_error_t> first{ORCA_SDK_E_INTERNAL};
    std::atomic<orca_sdk_error_t> second{ORCA_SDK_E_INTERNAL};
    auto destroy_session = [&](std::atomic<orca_sdk_error_t>& result) {
        while (!start.load())
            std::this_thread::yield();
        result.store(orca_sdk_session_destroy(session));
    };
    std::thread first_destroyer(destroy_session, std::ref(first));
    std::thread second_destroyer(destroy_session, std::ref(second));
    start.store(true);
    first_destroyer.join();
    second_destroyer.join();

    const orca_sdk_error_t first_result = first.load();
    const orca_sdk_error_t second_result = second.load();
    REQUIRE((first_result == ORCA_SDK_OK || second_result == ORCA_SDK_OK));
    REQUIRE((first_result == ORCA_SDK_E_BUSY || first_result == ORCA_SDK_E_INVALID_HANDLE ||
             second_result == ORCA_SDK_E_BUSY || second_result == ORCA_SDK_E_INVALID_HANDLE));
    REQUIRE(orca_sdk_destroy(sdk) == ORCA_SDK_OK);
}