#include "orca/mobile/device_sdk_c.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>

extern "C" int orca_mobile_device_c_header_compile_probe(void);

namespace {

orca_mobile_device_options_t sdk_options()
{
    orca_mobile_device_options_t options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    return options;
}

orca_mobile_device_session_options_t session_options()
{
    orca_mobile_device_session_options_t options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    return options;
}

orca_mobile_device_event_t event_output()
{
    orca_mobile_device_event_t event{};
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    return event;
}

} // namespace

TEST_CASE("Mobile SDK C header is consumable as C11", "[MobileDeviceSDK][CABI]")
{
    REQUIRE(orca_mobile_device_c_header_compile_probe() > 0);
}

TEST_CASE("Mobile SDK C ABI exposes versioned opaque handles", "[MobileDeviceSDK][CABI]")
{
    REQUIRE(std::strlen(orca_mobile_device_abi_version()) > 0);
    REQUIRE(std::strlen(orca_mobile_device_contract_version()) > 0);
    REQUIRE(std::strlen(orca_mobile_device_source_commit()) > 0);

    orca_mobile_device_string_t version{};
    REQUIRE(orca_mobile_device_get_contract_version(&version) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(version.data != nullptr);
    REQUIRE(version.size == std::strlen(orca_mobile_device_contract_version()));

    orca_mobile_device_t* sdk = nullptr;
    const orca_mobile_device_options_t options = sdk_options();
    REQUIRE(orca_mobile_device_create(&options, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(sdk != nullptr);

    orca_mobile_device_session_t* session = nullptr;
    const orca_mobile_device_session_options_t session_config = session_options();
    REQUIRE(orca_mobile_device_session_create(sdk, &session_config, &session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(session != nullptr);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_E_BUSY);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI submits and polls an explicit length task", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    const char payload[] = {'o', 'k', '\0', 'x'};
    orca_mobile_device_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = ORCA_MOBILE_DEVICE_TASK_KIND_SYNC;
    request.payload_utf8.data = payload;
    request.payload_utf8.size = static_cast<uint32_t>(sizeof(payload));

    orca_mobile_device_task_t* task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(task != nullptr);

    orca_mobile_device_string_t task_id{};
    REQUIRE(orca_mobile_device_task_get_id(task, &task_id) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(task_id.size > 0);

    orca_mobile_device_event_t event = event_output();
    REQUIRE(orca_mobile_device_poll_event(session, 1000, &event) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(event.type == ORCA_MOBILE_DEVICE_EVENT_TASK_SUBMITTED);
    REQUIRE(event.task == task);
    REQUIRE(event.task_id.size == task_id.size);
    REQUIRE(event.message_utf8.size > 0);
    REQUIRE(orca_mobile_device_event_clear(&event) == ORCA_MOBILE_DEVICE_OK);

    REQUIRE(orca_mobile_device_poll_event(session, 1000, &event) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(event.type == ORCA_MOBILE_DEVICE_EVENT_TASK_COMPLETED);
    REQUIRE(event.status == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(event.payload.size == sizeof(payload));
    REQUIRE(std::memcmp(event.payload.data, payload, sizeof(payload)) == 0);
    REQUIRE(orca_mobile_device_event_clear(&event) == ORCA_MOBILE_DEVICE_OK);
    uint32_t state = ORCA_MOBILE_DEVICE_TASK_PENDING;
    REQUIRE(orca_mobile_device_task_get_state(task, &state) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(state == ORCA_MOBILE_DEVICE_TASK_COMPLETED);
    REQUIRE(orca_mobile_device_task_destroy(task) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}


TEST_CASE("Mobile SDK C ABI accepts v1.7 JSON facade task kinds", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    const char payload[] = "{\"device_id\":\"printer-1\",\"command_id\":\"cmd-1\"}";
    const uint32_t kinds[] = {
        ORCA_MOBILE_DEVICE_TASK_KIND_DISCOVER,
        ORCA_MOBILE_DEVICE_TASK_KIND_CONNECT,
        ORCA_MOBILE_DEVICE_TASK_KIND_DISCONNECT,
        ORCA_MOBILE_DEVICE_TASK_KIND_STATUS,
        ORCA_MOBILE_DEVICE_TASK_KIND_COMMAND,
        ORCA_MOBILE_DEVICE_TASK_KIND_TRANSFER,
    };
    for (const uint32_t kind : kinds) {
        orca_mobile_device_task_request_t request{};
        request.struct_size = static_cast<uint32_t>(sizeof(request));
        request.kind = kind;
        request.payload_utf8.data = payload;
        request.payload_utf8.size = static_cast<uint32_t>(sizeof(payload) - 1u);

        orca_mobile_device_task_t* task = nullptr;
        REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(task != nullptr);

        orca_mobile_device_event_t submitted = event_output();
        REQUIRE(orca_mobile_device_poll_event(session, 1000, &submitted) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(submitted.type == ORCA_MOBILE_DEVICE_EVENT_TASK_SUBMITTED);
        REQUIRE(submitted.task == task);
        REQUIRE(orca_mobile_device_event_clear(&submitted) == ORCA_MOBILE_DEVICE_OK);

        orca_mobile_device_event_t completed = event_output();
        REQUIRE(orca_mobile_device_poll_event(session, 1000, &completed) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(completed.type == ORCA_MOBILE_DEVICE_EVENT_TASK_COMPLETED);
        REQUIRE(completed.status == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(completed.payload.size == sizeof(payload) - 1u);
        REQUIRE(std::memcmp(completed.payload.data, payload, sizeof(payload) - 1u) == 0);
        REQUIRE(orca_mobile_device_event_clear(&completed) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(orca_mobile_device_task_destroy(task) == ORCA_MOBILE_DEVICE_OK);
    }

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI keeps unsupported task kinds stable", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    const char payload[] = "{}";
    orca_mobile_device_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = 0xffffu;
    request.payload_utf8.data = payload;
    request.payload_utf8.size = static_cast<uint32_t>(sizeof(payload) - 1u);
    orca_mobile_device_task_t* task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_E_UNSUPPORTED);
    REQUIRE(task == nullptr);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}


TEST_CASE("Mobile SDK C ABI rejects invalid JSON for v1.7 facade tasks", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    const char invalid_json[] = "{\"device_id\":}";
    orca_mobile_device_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = ORCA_MOBILE_DEVICE_TASK_KIND_CONNECT;
    request.payload_utf8.data = invalid_json;
    request.payload_utf8.size = static_cast<uint32_t>(sizeof(invalid_json) - 1u);
    orca_mobile_device_task_t* task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT);
    REQUIRE(task == nullptr);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI reports timeout for an empty event queue", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    auto event = event_output();
    REQUIRE(orca_mobile_device_poll_event(session, 0, &event) == ORCA_MOBILE_DEVICE_E_TIMEOUT);
    REQUIRE(orca_mobile_device_poll_event(session, 1, &event) == ORCA_MOBILE_DEVICE_E_TIMEOUT);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI accepts and rejects truncated task requests by available fields", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_task_request_t too_small{};
    too_small.struct_size = static_cast<uint32_t>(sizeof(uint32_t) - 1u);
    orca_mobile_device_task_t* task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &too_small, &task) == ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL);
    REQUIRE(task == nullptr);

    orca_mobile_device_task_request_t sync_prefix{};
    sync_prefix.struct_size = static_cast<uint32_t>(sizeof(uint32_t));
    REQUIRE(orca_mobile_device_session_submit_task(session, &sync_prefix, &task) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(task != nullptr);
    auto submitted = event_output();
    REQUIRE(orca_mobile_device_poll_event(session, 1000, &submitted) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(submitted.type == ORCA_MOBILE_DEVICE_EVENT_TASK_SUBMITTED);
    REQUIRE(orca_mobile_device_event_clear(&submitted) == ORCA_MOBILE_DEVICE_OK);
    auto completed = event_output();
    REQUIRE(orca_mobile_device_poll_event(session, 1000, &completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(completed.type == ORCA_MOBILE_DEVICE_EVENT_TASK_COMPLETED);
    REQUIRE(orca_mobile_device_event_clear(&completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_task_destroy(task) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_task_request_t kind_prefix{};
    kind_prefix.struct_size = static_cast<uint32_t>(offsetof(orca_mobile_device_task_request_t, payload_utf8));
    kind_prefix.kind = ORCA_MOBILE_DEVICE_TASK_KIND_CONNECT;
    task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &kind_prefix, &task) == ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT);
    REQUIRE(task == nullptr);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI validates struct size and UTF-8", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_options_t too_small{};
    too_small.struct_size = sizeof(uint32_t) - 1u;
    orca_mobile_device_t* invalid_sdk = nullptr;
    REQUIRE(orca_mobile_device_create(&too_small, &invalid_sdk) == ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL);

    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    const char invalid_utf8[] = {'\xc3', '\x28'};
    orca_mobile_device_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = ORCA_MOBILE_DEVICE_TASK_KIND_SYNC;
    request.payload_utf8.data = invalid_utf8;
    request.payload_utf8.size = static_cast<uint32_t>(sizeof(invalid_utf8));
    orca_mobile_device_task_t* task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT);

    orca_mobile_device_event_t event{};
    event.struct_size = sizeof(uint32_t);
    REQUIRE(orca_mobile_device_poll_event(session, 0, &event) == ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL);
    REQUIRE(orca_mobile_device_poll_event(session, 0, nullptr) == ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}
TEST_CASE("Mobile SDK C ABI invalidates every task API after destroy", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = ORCA_MOBILE_DEVICE_TASK_KIND_SYNC;
    orca_mobile_device_task_t* task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_OK);
    orca_mobile_device_event_t submitted = event_output();
    REQUIRE(orca_mobile_device_poll_event(session, 1000, &submitted) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(submitted.type == ORCA_MOBILE_DEVICE_EVENT_TASK_SUBMITTED);
    REQUIRE(orca_mobile_device_event_clear(&submitted) == ORCA_MOBILE_DEVICE_OK);
    orca_mobile_device_event_t completed = event_output();
    REQUIRE(orca_mobile_device_poll_event(session, 1000, &completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(completed.type == ORCA_MOBILE_DEVICE_EVENT_TASK_COMPLETED);
    REQUIRE(orca_mobile_device_event_clear(&completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_task_destroy(task) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_string_t task_id{};
    uint32_t state = ORCA_MOBILE_DEVICE_TASK_PENDING;
    REQUIRE(orca_mobile_device_task_cancel(task) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_task_get_id(task, &task_id) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_task_get_id(task, nullptr) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_task_get_state(task, &state) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_task_get_state(task, nullptr) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_task_destroy(task) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI rejects null data with a maximum explicit UTF-8 length", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = ORCA_MOBILE_DEVICE_TASK_KIND_SYNC;
    request.payload_utf8.data = nullptr;
    request.payload_utf8.size = UINT32_MAX;
    orca_mobile_device_task_t* task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT);
    REQUIRE(task == nullptr);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}
TEST_CASE("Mobile SDK C ABI rejects stale handles after native storage reuse", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* old_sdk = nullptr;
    orca_mobile_device_session_t* old_session = nullptr;
    orca_mobile_device_task_t* old_task = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &old_sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(old_sdk, nullptr, &old_session) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = ORCA_MOBILE_DEVICE_TASK_KIND_SYNC;
    REQUIRE(orca_mobile_device_session_submit_task(old_session, &request, &old_task) == ORCA_MOBILE_DEVICE_OK);
    auto submitted = event_output();
    REQUIRE(orca_mobile_device_poll_event(old_session, 1000, &submitted) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_event_clear(&submitted) == ORCA_MOBILE_DEVICE_OK);
    auto completed = event_output();
    REQUIRE(orca_mobile_device_poll_event(old_session, 1000, &completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_event_clear(&completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_task_destroy(old_task) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_destroy(old_session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(old_sdk) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_t* new_sdk = nullptr;
    orca_mobile_device_session_t* new_session = nullptr;
    orca_mobile_device_task_t* new_task = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &new_sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(new_sdk, nullptr, &new_session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_submit_task(new_session, &request, &new_task) == ORCA_MOBILE_DEVICE_OK);

    const bool handles_are_distinct = old_sdk != new_sdk && old_session != new_session && old_task != new_task;
    REQUIRE(handles_are_distinct);
    if (!handles_are_distinct)
        return;

    uint32_t state = ORCA_MOBILE_DEVICE_TASK_PENDING;
    REQUIRE(orca_mobile_device_destroy(old_sdk) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_session_destroy(old_session) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_task_get_state(old_task, &state) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);

    auto new_submitted = event_output();
    REQUIRE(orca_mobile_device_poll_event(new_session, 1000, &new_submitted) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_event_clear(&new_submitted) == ORCA_MOBILE_DEVICE_OK);
    auto new_completed = event_output();
    REQUIRE(orca_mobile_device_poll_event(new_session, 1000, &new_completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_event_clear(&new_completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_task_destroy(new_task) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_destroy(new_session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(new_sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI keeps pending task destroy busy until events are cleared", "[MobileDeviceSDK][CABI][threading]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = ORCA_MOBILE_DEVICE_TASK_KIND_SYNC;
    orca_mobile_device_task_t* task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_OK);

    std::atomic<orca_mobile_device_error_t> destroy_result{ORCA_MOBILE_DEVICE_E_INTERNAL};
    std::thread destroyer([&] { destroy_result.store(orca_mobile_device_task_destroy(task)); });
    destroyer.join();
    REQUIRE(destroy_result.load() == ORCA_MOBILE_DEVICE_E_BUSY);

    auto submitted = event_output();
    REQUIRE(orca_mobile_device_poll_event(session, 1000, &submitted) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(submitted.type == ORCA_MOBILE_DEVICE_EVENT_TASK_SUBMITTED);
    REQUIRE(orca_mobile_device_event_clear(&submitted) == ORCA_MOBILE_DEVICE_OK);
    auto completed = event_output();
    REQUIRE(orca_mobile_device_poll_event(session, 1000, &completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(completed.type == ORCA_MOBILE_DEVICE_EVENT_TASK_COMPLETED);
    REQUIRE(completed.task == task);
    REQUIRE(orca_mobile_device_task_destroy(task) == ORCA_MOBILE_DEVICE_E_BUSY);
    REQUIRE(orca_mobile_device_event_clear(&completed) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_task_destroy(task) == ORCA_MOBILE_DEVICE_OK);

    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}

TEST_CASE("Mobile SDK C ABI event clear releases task and session storage", "[MobileDeviceSDK][CABI]")
{
    orca_mobile_device_t* sdk = nullptr;
    orca_mobile_device_session_t* session = nullptr;
    REQUIRE(orca_mobile_device_create(nullptr, &sdk) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_create(sdk, nullptr, &session) == ORCA_MOBILE_DEVICE_OK);

    orca_mobile_device_task_request_t request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.kind = ORCA_MOBILE_DEVICE_TASK_KIND_SYNC;
    auto event = event_output();
    for (int i = 0; i != 128; ++i) {
        orca_mobile_device_task_t* task = nullptr;
        REQUIRE(orca_mobile_device_session_submit_task(session, &request, &task) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(orca_mobile_device_poll_event(session, 1000, &event) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(event.type == ORCA_MOBILE_DEVICE_EVENT_TASK_SUBMITTED);
        REQUIRE(orca_mobile_device_event_clear(&event) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(orca_mobile_device_poll_event(session, 1000, &event) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(event.type == ORCA_MOBILE_DEVICE_EVENT_TASK_COMPLETED);
        REQUIRE(event.task == task);
        REQUIRE(orca_mobile_device_event_clear(&event) == ORCA_MOBILE_DEVICE_OK);
        REQUIRE(orca_mobile_device_task_destroy(task) == ORCA_MOBILE_DEVICE_OK);
    }

    orca_mobile_device_task_t* undelivered_task = nullptr;
    REQUIRE(orca_mobile_device_session_submit_task(session, &request, &undelivered_task) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_poll_event(session, 1000, &event) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_session_destroy(session) == ORCA_MOBILE_DEVICE_OK);
    uint32_t state = ORCA_MOBILE_DEVICE_TASK_PENDING;
    REQUIRE(orca_mobile_device_task_get_state(undelivered_task, &state) == ORCA_MOBILE_DEVICE_E_INVALID_HANDLE);
    REQUIRE(orca_mobile_device_event_clear(&event) == ORCA_MOBILE_DEVICE_OK);
    REQUIRE(orca_mobile_device_destroy(sdk) == ORCA_MOBILE_DEVICE_OK);
}
