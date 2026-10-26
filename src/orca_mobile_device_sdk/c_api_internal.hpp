#pragma once

#include "orca/mobile/device_sdk_c.h"
#include "orca/mobile/device/DeviceSDK.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct orca_mobile_device_event_record;
struct orca_mobile_device_native;
struct orca_mobile_device_session_native;
struct orca_mobile_device_task_native;

namespace orca_mobile_device_internal {

enum class handle_kind : uint8_t { sdk, session, task };
struct handle_token;

class handle_guard {
public:
    handle_guard() = default;
    handle_guard(const handle_guard&) = delete;
    handle_guard& operator=(const handle_guard&) = delete;
    handle_guard(handle_guard&& other) noexcept;
    handle_guard& operator=(handle_guard&& other) noexcept;
    ~handle_guard();

    explicit operator bool() const noexcept { return m_object != nullptr; }
    void* object() const noexcept { return m_object; }
    std::shared_ptr<handle_token> token() const noexcept { return m_token; }
    void release() noexcept;

private:
    friend handle_guard acquire(void* handle, handle_kind kind) noexcept;
    friend void retire(handle_guard& guard) noexcept;
    handle_guard(std::shared_ptr<handle_token> token, void* object) noexcept
        : m_token(std::move(token)), m_object(object) {}
    std::shared_ptr<handle_token> m_token;
    void* m_object{nullptr};
};

handle_guard acquire(void* handle, handle_kind kind) noexcept;
void* register_handle(void* object, handle_kind kind, void* owner);
void retire(handle_guard& guard) noexcept;
void retire_owned_handles(void* owner) noexcept;
void wait_idle(const std::shared_ptr<handle_token>& token) noexcept;
void wait_owned_handles(void* owner) noexcept;

bool has_field(uint32_t struct_size, size_t offset, size_t field_size) noexcept;
bool valid_utf8(orca_mobile_device_string_t value) noexcept;
bool valid_json_payload(orca_mobile_device_string_t value) noexcept;
std::string copy_string(orca_mobile_device_string_t value);
uint64_t now_millis() noexcept;

orca_mobile_device_error_t write_view(orca_mobile_device_string_t* output, const std::string& value) noexcept;
void reset_event(orca_mobile_device_event_t* event) noexcept;
void fill_event(const orca_mobile_device_event_record& record, orca_mobile_device_event_t* event) noexcept;
void deliver_event(orca_mobile_device_event_t* output, orca_mobile_device_session_native* session,
                   orca_mobile_device_event_record event);
void clear_event_delivery(orca_mobile_device_event_t* output) noexcept;
void clear_session_event_deliveries(orca_mobile_device_session_native* session) noexcept;
bool has_delivered_event(orca_mobile_device_task_native* task) noexcept;

} // namespace orca_mobile_device_internal

struct orca_mobile_device_native {
    static constexpr uint64_t kMagic = UINT64_C(0x4f52434153444b31);
    uint64_t magic{kMagic};
    orca_mobile_device_t* handle{nullptr};
    std::mutex mutex;
    uint32_t session_count{0};
    bool shutting_down{false};
};

struct orca_mobile_device_event_record {
    uint32_t type{ORCA_MOBILE_DEVICE_EVENT_NONE};
    orca_mobile_device_error_t status{ORCA_MOBILE_DEVICE_OK};
    uint64_t sequence{0};
    uint64_t timestamp_ms{0};
    orca_mobile_device_task_t* task{nullptr};
    orca_mobile_device_task_native* task_state{nullptr};
    std::string task_id;
    std::string message;
    std::string payload;
};

struct orca_mobile_device_task_native {
    static constexpr uint64_t kMagic = UINT64_C(0x4f52434154415331);
    uint64_t magic{kMagic};
    orca_mobile_device_task_t* handle{nullptr};
    orca_mobile_device_session_native* session{nullptr};
    uint64_t numeric_id{0};
    std::string id;
    uint32_t kind{ORCA_MOBILE_DEVICE_TASK_KIND_SYNC};
    std::string payload;
    uint32_t state{ORCA_MOBILE_DEVICE_TASK_PENDING};
    bool cancel_requested{false};
    bool cancel_event_enqueued{false};
    bool user_destroyed{false};
};

struct orca_mobile_device_session_native {
    static constexpr uint64_t kMagic = UINT64_C(0x4f52434153455331);
    uint64_t magic{kMagic};
    orca_mobile_device_session_t* handle{nullptr};
    orca_mobile_device_t* sdk_handle{nullptr};
    orca_mobile_device_native* sdk{nullptr};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<orca_mobile_device_event_record> events;
    std::vector<std::unique_ptr<orca_mobile_device_task_native>> tasks;
    std::thread worker;
    uint64_t next_task_id{1};
    uint64_t next_event_sequence{1};
    uint64_t wake_generation{0};
    bool stopping{false};

    explicit orca_mobile_device_session_native(orca_mobile_device_native* owner) : sdk(owner) {}
    ~orca_mobile_device_session_native();
    void start_worker();
    void worker_main() noexcept;
    void enqueue_event(orca_mobile_device_event_record event);
};
