#include "c_api_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace orca_mobile_device_internal {

struct handle_token {
    uintptr_t handle_value{0};
    void* object{nullptr};
    handle_kind kind{handle_kind::sdk};
    void* owner{nullptr};
    bool alive{true};
    size_t active{0};
};

namespace {

std::mutex g_handles_mutex;
std::condition_variable g_handles_condition;
std::unordered_map<uintptr_t, std::shared_ptr<handle_token>> g_handles;
uintptr_t g_next_handle_value = static_cast<uintptr_t>(0x10000u);

struct event_delivery {
    orca_mobile_device_session_native* session{nullptr};
    orca_mobile_device_event_record record;
};

std::mutex g_event_mutex;
std::unordered_map<const orca_mobile_device_event_t*, std::unique_ptr<event_delivery>> g_event_deliveries;

uintptr_t handle_key(const void* handle) noexcept
{
    return reinterpret_cast<uintptr_t>(handle);
}

} // namespace

handle_guard::handle_guard(handle_guard&& other) noexcept
    : m_token(std::move(other.m_token)), m_object(other.m_object)
{
    other.m_object = nullptr;
}

handle_guard& handle_guard::operator=(handle_guard&& other) noexcept
{
    if (this != &other) {
        release();
        m_token = std::move(other.m_token);
        m_object = other.m_object;
        other.m_object = nullptr;
    }
    return *this;
}

handle_guard::~handle_guard()
{
    release();
}

void handle_guard::release() noexcept
{
    if (!m_token)
        return;
    {
        std::lock_guard<std::mutex> lock(g_handles_mutex);
        if (m_token->active != 0)
            --m_token->active;
    }
    g_handles_condition.notify_all();
    m_token.reset();
    m_object = nullptr;
}

handle_guard acquire(void* handle, handle_kind kind) noexcept
{
    if (handle == nullptr)
        return {};
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    const auto it = g_handles.find(handle_key(handle));
    if (it == g_handles.end() || !it->second->alive || it->second->kind != kind)
        return {};
    ++it->second->active;
    return handle_guard(it->second, it->second->object);
}

void* register_handle(void* object, handle_kind kind, void* owner)
{
    if (object == nullptr)
        throw std::invalid_argument("cannot register a null handle object");

    std::lock_guard<std::mutex> lock(g_handles_mutex);
    constexpr uintptr_t kHandleStep = static_cast<uintptr_t>(0x10u);
    if (g_next_handle_value == 0 ||
        g_next_handle_value > std::numeric_limits<uintptr_t>::max() - kHandleStep)
        throw std::bad_alloc();

    auto token = std::make_shared<handle_token>();
    token->handle_value = g_next_handle_value;
    token->object = object;
    token->kind = kind;
    token->owner = owner;
    g_next_handle_value += kHandleStep;
    g_handles.emplace(token->handle_value, token);
    return reinterpret_cast<void*>(token->handle_value);
}

void retire(handle_guard& guard) noexcept
{
    if (!guard.m_token)
        return;
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    guard.m_token->alive = false;
    const auto it = g_handles.find(guard.m_token->handle_value);
    if (it != g_handles.end() && it->second == guard.m_token)
        g_handles.erase(it);
}

void retire_owned_handles(void* owner) noexcept
{
    if (owner == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    for (const auto& [handle, token] : g_handles) {
        (void)handle;
        if (token->owner == owner)
            token->alive = false;
    }
}

void wait_idle(const std::shared_ptr<handle_token>& token) noexcept
{
    if (!token)
        return;
    std::unique_lock<std::mutex> lock(g_handles_mutex);
    g_handles_condition.wait(lock, [&] { return token->active == 0; });
}

void wait_owned_handles(void* owner) noexcept
{
    if (owner == nullptr)
        return;
    for (;;) {
        std::unique_lock<std::mutex> lock(g_handles_mutex);
        auto it = std::find_if(g_handles.begin(), g_handles.end(), [owner](const auto& entry) {
            return entry.second->owner == owner;
        });
        if (it == g_handles.end())
            return;
        const auto token = it->second;
        if (token->active != 0)
            g_handles_condition.wait(lock, [&] { return token->active == 0; });
        const auto current = g_handles.find(token->handle_value);
        if (current != g_handles.end() && current->second == token)
            g_handles.erase(current);
    }
}

bool has_field(uint32_t struct_size, size_t offset, size_t field_size) noexcept
{
    return offset <= static_cast<size_t>(struct_size) &&
           field_size <= static_cast<size_t>(struct_size) - offset;
}

bool valid_utf8(orca_mobile_device_string_t value) noexcept
{
    if (value.size != 0 && value.data == nullptr)
        return false;

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(value.data);
    const size_t length = static_cast<size_t>(value.size);
    size_t index = 0;
    while (index < length) {
        const unsigned char first = bytes[index++];
        size_t continuation_count = 0;
        uint32_t code_point = 0;
        uint32_t minimum = 0;
        if (first <= 0x7f) {
            continue;
        } else if (first >= 0xc2 && first <= 0xdf) {
            continuation_count = 1;
            code_point = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation_count = 2;
            code_point = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation_count = 3;
            code_point = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (continuation_count > length - index)
            return false;
        for (size_t i = 0; i < continuation_count; ++i) {
            const unsigned char next = bytes[index++];
            if ((next & 0xc0) != 0x80)
                return false;
            code_point = (code_point << 6) | (next & 0x3f);
        }
        if (code_point < minimum || code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff))
            return false;
    }
    return true;
}

bool valid_json_payload(orca_mobile_device_string_t value) noexcept
{
    if (!valid_utf8(value) || value.size == 0)
        return false;
    try {
        const auto parsed = nlohmann::json::parse(
            value.data, value.data + value.size, nullptr, false);
        return !parsed.is_discarded();
    } catch (...) {
        return false;
    }
}

std::string copy_string(orca_mobile_device_string_t value)
{
    if (value.size == 0)
        return {};
    return std::string(value.data, static_cast<size_t>(value.size));
}

uint64_t now_millis() noexcept
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

orca_mobile_device_error_t write_view(orca_mobile_device_string_t* output, const std::string& value) noexcept
{
    if (output == nullptr)
        return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;
    if (value.size() > std::numeric_limits<uint32_t>::max())
        return ORCA_MOBILE_DEVICE_E_BUFFER_TOO_SMALL;
    output->data = value.data();
    output->size = static_cast<uint32_t>(value.size());
    return ORCA_MOBILE_DEVICE_OK;
}

void reset_event(orca_mobile_device_event_t* event) noexcept
{
    if (event == nullptr)
        return;
    const uint32_t size = event->struct_size;
    std::memset(event, 0, std::min<size_t>(size, sizeof(*event)));
    event->struct_size = size;
}

bool fill_string(orca_mobile_device_string_t* output, const std::string& value) noexcept
{
    if (value.size() > std::numeric_limits<uint32_t>::max()) {
        output->data = nullptr;
        output->size = 0;
        return false;
    }
    output->data = value.data();
    output->size = static_cast<uint32_t>(value.size());
    return true;
}

void fill_event(const orca_mobile_device_event_record& record, orca_mobile_device_event_t* event) noexcept
{
    const uint32_t size = event->struct_size;
    std::memset(event, 0, sizeof(*event));
    event->struct_size = size;
    event->type = record.type;
    event->status = record.status;
    event->sequence = record.sequence;
    event->timestamp_ms = record.timestamp_ms;
    event->task = record.task;
    const bool task_id_fits = fill_string(&event->task_id, record.task_id);
    const bool message_fits = fill_string(&event->message_utf8, record.message);
    const bool payload_fits = record.payload.size() <= std::numeric_limits<uint32_t>::max();
    if (payload_fits) {
        event->payload.data = reinterpret_cast<const uint8_t*>(record.payload.data());
        event->payload.size = static_cast<uint32_t>(record.payload.size());
    }
    if (!task_id_fits || !message_fits || !payload_fits)
        event->status = ORCA_MOBILE_DEVICE_E_BUFFER_TOO_SMALL;
}

void deliver_event(orca_mobile_device_event_t* output, orca_mobile_device_session_native* session,
                   orca_mobile_device_event_record event)
{
    std::lock_guard<std::mutex> lock(g_event_mutex);
    g_event_deliveries.erase(output);
    auto delivery = std::make_unique<event_delivery>();
    delivery->session = session;
    delivery->record = std::move(event);
    const auto [it, inserted] = g_event_deliveries.emplace(output, std::move(delivery));
    if (!inserted)
        throw std::logic_error("event output is already registered");
    fill_event(it->second->record, output);
}

void clear_event_delivery(orca_mobile_device_event_t* output) noexcept
{
    if (output == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_event_mutex);
    g_event_deliveries.erase(output);
}

void clear_session_event_deliveries(orca_mobile_device_session_native* session) noexcept
{
    if (session == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_event_mutex);
    for (auto it = g_event_deliveries.begin(); it != g_event_deliveries.end();) {
        if (it->second->session == session)
            it = g_event_deliveries.erase(it);
        else
            ++it;
    }
}

bool has_delivered_event(orca_mobile_device_task_native* task) noexcept
{
    if (task == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(g_event_mutex);
    return std::any_of(g_event_deliveries.begin(), g_event_deliveries.end(),
                       [task](const auto& entry) {
                           return entry.second->record.task_state == task;
                       });
}

} // namespace orca_mobile_device_internal

namespace {

template <typename Function>
orca_mobile_device_error_t guarded(Function&& function) noexcept
{
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return ORCA_MOBILE_DEVICE_E_OUT_OF_MEMORY;
    } catch (...) {
        return ORCA_MOBILE_DEVICE_E_INTERNAL;
    }
}

orca_mobile_device_error_t validate_output(void* output, uint32_t struct_size, size_t required_size)
{
    if (output == nullptr)
        return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;
    if (struct_size < sizeof(uint32_t) || struct_size < required_size)
        return ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL;
    return ORCA_MOBILE_DEVICE_OK;
}

orca_mobile_device_t* public_sdk_handle(void* value)
{
    return reinterpret_cast<orca_mobile_device_t*>(value);
}

orca_mobile_device_session_t* public_session_handle(void* value)
{
    return reinterpret_cast<orca_mobile_device_session_t*>(value);
}

orca_mobile_device_task_t* public_task_handle(void* value)
{
    return reinterpret_cast<orca_mobile_device_task_t*>(value);
}

bool has_queued_event(const orca_mobile_device_session_native* session, const orca_mobile_device_task_native* task)
{
    return std::any_of(session->events.begin(), session->events.end(),
                       [task](const auto& event) { return event.task_state == task; });
}

} // namespace

orca_mobile_device_session_native::~orca_mobile_device_session_native()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    condition.notify_all();
    if (worker.joinable())
        worker.join();
    events.clear();
    tasks.clear();
}

void orca_mobile_device_session_native::start_worker()
{
    worker = std::thread([this] { worker_main(); });
}

void orca_mobile_device_session_native::enqueue_event(orca_mobile_device_event_record event)
{
    event.sequence = next_event_sequence++;
    event.timestamp_ms = orca_mobile_device_internal::now_millis();
    events.push_back(std::move(event));
    condition.notify_all();
}

void orca_mobile_device_session_native::worker_main() noexcept
{
    try {
        for (;;) {
            orca_mobile_device_task_native* task = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] {
                    if (stopping)
                        return true;
                    return std::any_of(tasks.begin(), tasks.end(), [](const auto& candidate) {
                        return candidate->state == ORCA_MOBILE_DEVICE_TASK_PENDING && !candidate->cancel_requested;
                    });
                });
                if (stopping)
                    return;
                const auto it = std::find_if(tasks.begin(), tasks.end(), [](const auto& candidate) {
                    return candidate->state == ORCA_MOBILE_DEVICE_TASK_PENDING && !candidate->cancel_requested;
                });
                if (it == tasks.end())
                    continue;
                task = it->get();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            std::lock_guard<std::mutex> lock(mutex);
            if (stopping)
                return;
            if (task->state != ORCA_MOBILE_DEVICE_TASK_PENDING || task->cancel_requested || task->user_destroyed)
                continue;
            task->state = ORCA_MOBILE_DEVICE_TASK_COMPLETED;
            enqueue_event({ORCA_MOBILE_DEVICE_EVENT_TASK_COMPLETED, ORCA_MOBILE_DEVICE_OK, 0, 0,
                           task->handle, task, task->id, "task completed", task->payload});
        }
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(mutex);
            if (!stopping)
                enqueue_event({ORCA_MOBILE_DEVICE_EVENT_ERROR, ORCA_MOBILE_DEVICE_E_INTERNAL, 0, 0,
                               nullptr, nullptr, {}, "worker failure", {}});
        } catch (...) {
        }
    }
}

namespace {

using orca_mobile_device_internal::acquire;
using orca_mobile_device_internal::handle_kind;
using orca_mobile_device_internal::handle_guard;

handle_guard acquire_sdk(orca_mobile_device_t* sdk)
{
    return acquire(sdk, handle_kind::sdk);
}

handle_guard acquire_session(orca_mobile_device_session_t* session)
{
    return acquire(session, handle_kind::session);
}

handle_guard acquire_task(orca_mobile_device_task_t* task)
{
    return acquire(task, handle_kind::task);
}

} // namespace

extern "C" {

const char* orca_mobile_device_abi_version(void)
{
    return "1.0";
}

const char* orca_mobile_device_contract_version(void)
{
    return ORCA_MOBILE_DEVICE_SDK_CONTRACT_VERSION;
}

const char* orca_mobile_device_source_commit(void)
{
    return ORCA_MOBILE_DEVICE_SDK_SOURCE_COMMIT;
}

const char* orca_mobile_device_source_commit_short(void)
{
    return ORCA_MOBILE_DEVICE_SDK_SOURCE_COMMIT_SHORT;
}

const char* orca_mobile_device_error_message(orca_mobile_device_error_t error)
{
    switch (error) {
    case ORCA_MOBILE_DEVICE_OK: return "ok";
    case ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT: return "invalid argument";
    case ORCA_MOBILE_DEVICE_E_INVALID_HANDLE: return "invalid handle";
    case ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL: return "struct is too small";
    case ORCA_MOBILE_DEVICE_E_BUFFER_TOO_SMALL: return "buffer is too small";
    case ORCA_MOBILE_DEVICE_E_TIMEOUT: return "poll timed out";
    case ORCA_MOBILE_DEVICE_E_CANCELED: return "task canceled";
    case ORCA_MOBILE_DEVICE_E_ALREADY_COMPLETE: return "task already completed";
    case ORCA_MOBILE_DEVICE_E_BUSY: return "resource is busy";
    case ORCA_MOBILE_DEVICE_E_UNSUPPORTED: return "operation is unsupported";
    case ORCA_MOBILE_DEVICE_E_WAKEUP: return "poll was woken";
    case ORCA_MOBILE_DEVICE_E_OUT_OF_MEMORY: return "out of memory";
    case ORCA_MOBILE_DEVICE_E_INTERNAL: return "internal error";
    case ORCA_MOBILE_DEVICE_E_ABI_MISMATCH: return "ABI mismatch";
    default: return "unknown error";
    }
}

orca_mobile_device_error_t orca_mobile_device_get_abi_version(orca_mobile_device_string_t* out_version)
{
    return guarded([&] { return orca_mobile_device_internal::write_view(out_version, orca_mobile_device_abi_version()); });
}

orca_mobile_device_error_t orca_mobile_device_get_contract_version(orca_mobile_device_string_t* out_version)
{
    return guarded([&] { return orca_mobile_device_internal::write_view(out_version, orca_mobile_device_contract_version()); });
}

orca_mobile_device_error_t orca_mobile_device_get_source_commit(orca_mobile_device_string_t* out_version)
{
    return guarded([&] { return orca_mobile_device_internal::write_view(out_version, orca_mobile_device_source_commit()); });
}

orca_mobile_device_error_t orca_mobile_device_create(const orca_mobile_device_options_t* options, orca_mobile_device_t** out_sdk)
{
    return guarded([&] {
        if (out_sdk == nullptr)
            return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;
        *out_sdk = nullptr;
        if (options != nullptr && options->struct_size < sizeof(uint32_t))
            return ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL;

        auto sdk = std::make_unique<orca_mobile_device_native>();
        void* handle = orca_mobile_device_internal::register_handle(sdk.get(), handle_kind::sdk, nullptr);
        sdk->handle = public_sdk_handle(handle);
        *out_sdk = sdk->handle;
        sdk.release();
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_destroy(orca_mobile_device_t* sdk)
{
    return guarded([&] {
        auto sdk_handle = acquire_sdk(sdk);
        if (!sdk_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        auto* state = static_cast<orca_mobile_device_native*>(sdk_handle.object());
        const auto token = sdk_handle.token();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->shutting_down || state->session_count != 0)
                return ORCA_MOBILE_DEVICE_E_BUSY;
            state->shutting_down = true;
            state->magic = 0;
        }
        orca_mobile_device_internal::retire(sdk_handle);
        sdk_handle.release();
        orca_mobile_device_internal::wait_idle(token);
        delete state;
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_session_create(
    orca_mobile_device_t* sdk,
    const orca_mobile_device_session_options_t* options,
    orca_mobile_device_session_t** out_session)
{
    return guarded([&] {
        if (out_session == nullptr)
            return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;
        *out_session = nullptr;
        auto sdk_handle = acquire_sdk(sdk);
        if (!sdk_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        auto* sdk_state = static_cast<orca_mobile_device_native*>(sdk_handle.object());
        if (options != nullptr && options->struct_size < sizeof(uint32_t))
            return ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL;
        if (options != nullptr &&
            orca_mobile_device_internal::has_field(options->struct_size,
                                         offsetof(orca_mobile_device_session_options_t, device_id),
                                         sizeof(options->device_id)) &&
            !orca_mobile_device_internal::valid_utf8(options->device_id))
            return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;

        auto session = std::make_unique<orca_mobile_device_session_native>(sdk_state);
        {
            std::lock_guard<std::mutex> lock(sdk_state->mutex);
            if (sdk_state->shutting_down)
                return ORCA_MOBILE_DEVICE_E_BUSY;
            ++sdk_state->session_count;
        }
        try {
            session->start_worker();
            void* handle = orca_mobile_device_internal::register_handle(session.get(), handle_kind::session, sdk_state);
            session->handle = public_session_handle(handle);
            session->sdk_handle = sdk_state->handle;
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(sdk_state->mutex);
                --sdk_state->session_count;
            }
            throw;
        }
        *out_session = session.release()->handle;
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_session_destroy(orca_mobile_device_session_t* session)
{
    return guarded([&] {
        auto session_handle = acquire_session(session);
        if (!session_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        auto* state = static_cast<orca_mobile_device_session_native*>(session_handle.object());
        const auto token = session_handle.token();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stopping)
                return ORCA_MOBILE_DEVICE_E_BUSY;
            state->stopping = true;
            state->magic = 0;
        }
        orca_mobile_device_internal::retire(session_handle);
        state->condition.notify_all();
        if (state->worker.joinable())
            state->worker.join();

        session_handle.release();
        orca_mobile_device_internal::wait_idle(token);
        // After all session calls are idle, no task API can pass its stopping check.
        orca_mobile_device_internal::retire_owned_handles(state);
        orca_mobile_device_internal::wait_owned_handles(state);
        orca_mobile_device_internal::clear_session_event_deliveries(state);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->events.clear();
        }

        auto sdk_handle = acquire_sdk(state->sdk_handle);
        if (sdk_handle) {
            auto* sdk_state = static_cast<orca_mobile_device_native*>(sdk_handle.object());
            std::lock_guard<std::mutex> lock(sdk_state->mutex);
            if (sdk_state->session_count != 0)
                --sdk_state->session_count;
        }
        delete state;
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_session_submit_task(
    orca_mobile_device_session_t* session,
    const orca_mobile_device_task_request_t* request,
    orca_mobile_device_task_t** out_task)
{
    return guarded([&] {
        if (out_task == nullptr)
            return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;
        *out_task = nullptr;
        auto session_handle = acquire_session(session);
        if (!session_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        auto* session_state = static_cast<orca_mobile_device_session_native*>(session_handle.object());
        if (request == nullptr || request->struct_size < sizeof(uint32_t))
            return request == nullptr ? ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT : ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL;

        orca_mobile_device_string_t payload{};
        uint32_t kind = ORCA_MOBILE_DEVICE_TASK_KIND_SYNC;
        if (orca_mobile_device_internal::has_field(request->struct_size,
                                         offsetof(orca_mobile_device_task_request_t, kind), sizeof(request->kind)))
            kind = request->kind;
        switch (kind) {
        case ORCA_MOBILE_DEVICE_TASK_KIND_SYNC:
        case ORCA_MOBILE_DEVICE_TASK_KIND_DISCOVER:
        case ORCA_MOBILE_DEVICE_TASK_KIND_CONNECT:
        case ORCA_MOBILE_DEVICE_TASK_KIND_DISCONNECT:
        case ORCA_MOBILE_DEVICE_TASK_KIND_STATUS:
        case ORCA_MOBILE_DEVICE_TASK_KIND_COMMAND:
        case ORCA_MOBILE_DEVICE_TASK_KIND_TRANSFER:
            break;
        default:
            return ORCA_MOBILE_DEVICE_E_UNSUPPORTED;
        }
        if (orca_mobile_device_internal::has_field(request->struct_size,
                                         offsetof(orca_mobile_device_task_request_t, payload_utf8),
                                         sizeof(request->payload_utf8)))
            payload = request->payload_utf8;
        if (!orca_mobile_device_internal::valid_utf8(payload))
            return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;
        if (kind != ORCA_MOBILE_DEVICE_TASK_KIND_SYNC &&
            !orca_mobile_device_internal::valid_json_payload(payload))
            return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;

        std::lock_guard<std::mutex> lock(session_state->mutex);
        if (session_state->stopping)
            return ORCA_MOBILE_DEVICE_E_BUSY;
        auto task = std::make_unique<orca_mobile_device_task_native>();
        task->session = session_state;
        task->numeric_id = session_state->next_task_id++;
        task->id = "task-" + std::to_string(task->numeric_id);
        task->kind = kind;
        task->payload = orca_mobile_device_internal::copy_string(payload);
        auto* task_state = task.get();
        void* handle = orca_mobile_device_internal::register_handle(task_state, handle_kind::task, session_state);
        task_state->handle = public_task_handle(handle);
        session_state->tasks.push_back(std::move(task));
        session_state->enqueue_event({ORCA_MOBILE_DEVICE_EVENT_TASK_SUBMITTED, ORCA_MOBILE_DEVICE_OK, 0, 0,
                                      task_state->handle, task_state, task_state->id,
                                      "task submitted", {}});
        *out_task = task_state->handle;
        session_state->condition.notify_all();
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_task_cancel(orca_mobile_device_task_t* task)
{
    return guarded([&] {
        auto task_handle = acquire_task(task);
        if (!task_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        auto* task_state = static_cast<orca_mobile_device_task_native*>(task_handle.object());
        auto* session = task_state->session;
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->stopping || task_state->magic != orca_mobile_device_task_native::kMagic || task_state->user_destroyed)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        if (task_state->state != ORCA_MOBILE_DEVICE_TASK_PENDING)
            return ORCA_MOBILE_DEVICE_E_ALREADY_COMPLETE;
        task_state->cancel_requested = true;
        task_state->state = ORCA_MOBILE_DEVICE_TASK_CANCELED;
        if (!task_state->cancel_event_enqueued) {
            task_state->cancel_event_enqueued = true;
            session->enqueue_event({ORCA_MOBILE_DEVICE_EVENT_TASK_CANCELED, ORCA_MOBILE_DEVICE_E_CANCELED, 0, 0,
                                    task_state->handle, task_state, task_state->id,
                                    "task canceled", {}});
        }
        session->condition.notify_all();
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_task_destroy(orca_mobile_device_task_t* task)
{
    return guarded([&] {
        auto task_handle = acquire_task(task);
        if (!task_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        const auto token = task_handle.token();
        auto* task_state = static_cast<orca_mobile_device_task_native*>(task_handle.object());
        auto* session = task_state->session;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->stopping || task_state->magic != orca_mobile_device_task_native::kMagic || task_state->user_destroyed)
                return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
            if (task_state->state == ORCA_MOBILE_DEVICE_TASK_PENDING ||
                has_queued_event(session, task_state) ||
                orca_mobile_device_internal::has_delivered_event(task_state))
                return ORCA_MOBILE_DEVICE_E_BUSY;
            task_state->user_destroyed = true;
            task_state->magic = 0;
        }
        orca_mobile_device_internal::retire(task_handle);
        task_handle.release();
        orca_mobile_device_internal::wait_idle(token);
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            const auto it = std::find_if(session->tasks.begin(), session->tasks.end(),
                                         [task_state](const auto& candidate) {
                                             return candidate.get() == task_state;
                                         });
            if (it != session->tasks.end())
                session->tasks.erase(it);
        }
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_task_get_id(
    const orca_mobile_device_task_t* task,
    orca_mobile_device_string_t* out_task_id)
{
    return guarded([&] {
        auto task_handle = acquire_task(const_cast<orca_mobile_device_task_t*>(task));
        if (!task_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        if (out_task_id == nullptr)
            return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;
        auto* task_state = static_cast<orca_mobile_device_task_native*>(task_handle.object());
        std::lock_guard<std::mutex> lock(task_state->session->mutex);
        if (task_state->session->stopping || task_state->magic != orca_mobile_device_task_native::kMagic || task_state->user_destroyed)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        return orca_mobile_device_internal::write_view(out_task_id, task_state->id);
    });
}

orca_mobile_device_error_t orca_mobile_device_task_get_state(const orca_mobile_device_task_t* task, uint32_t* out_state)
{
    return guarded([&] {
        auto task_handle = acquire_task(const_cast<orca_mobile_device_task_t*>(task));
        if (!task_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        if (out_state == nullptr)
            return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;
        auto* task_state = static_cast<orca_mobile_device_task_native*>(task_handle.object());
        std::lock_guard<std::mutex> lock(task_state->session->mutex);
        if (task_state->session->stopping || task_state->magic != orca_mobile_device_task_native::kMagic || task_state->user_destroyed)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        *out_state = task_state->state;
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_poll_event(
    orca_mobile_device_session_t* session,
    uint32_t timeout_ms,
    orca_mobile_device_event_t* out_event)
{
    return guarded([&] {
        auto session_handle = acquire_session(session);
        if (!session_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        const orca_mobile_device_error_t output_result = validate_output(
            out_event, out_event == nullptr ? 0 : out_event->struct_size, sizeof(orca_mobile_device_event_t));
        if (output_result != ORCA_MOBILE_DEVICE_OK)
            return output_result;
        auto* session_state = static_cast<orca_mobile_device_session_native*>(session_handle.object());

        std::unique_lock<std::mutex> lock(session_state->mutex);
        const uint64_t observed_wake = session_state->wake_generation;
        const auto ready = [session_state, observed_wake] {
            return session_state->stopping || session_state->wake_generation != observed_wake ||
                   !session_state->events.empty();
        };
        if (!ready()) {
            if (timeout_ms == 0) {
                return ORCA_MOBILE_DEVICE_E_TIMEOUT;
            } else if (timeout_ms == ORCA_MOBILE_DEVICE_WAIT_INFINITE) {
                session_state->condition.wait(lock, ready);
            } else if (!session_state->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
                return ORCA_MOBILE_DEVICE_E_TIMEOUT;
            }
        }
        if (session_state->stopping)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        if (session_state->events.empty() && session_state->wake_generation != observed_wake)
            return ORCA_MOBILE_DEVICE_E_WAKEUP;
        if (session_state->events.empty())
            return ORCA_MOBILE_DEVICE_E_TIMEOUT;

        orca_mobile_device_event_record event = std::move(session_state->events.front());
        session_state->events.pop_front();
        // Keep the session lock while registering the delivery. task_destroy
        // uses the same order, so it cannot retire this task between pop/fill.
        orca_mobile_device_internal::deliver_event(out_event, session_state, std::move(event));
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_event_clear(orca_mobile_device_event_t* event)
{
    return guarded([&] {
        if (event == nullptr)
            return ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT;
        if (event->struct_size < sizeof(uint32_t))
            return ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL;
        orca_mobile_device_internal::clear_event_delivery(event);
        orca_mobile_device_internal::reset_event(event);
        return ORCA_MOBILE_DEVICE_OK;
    });
}

orca_mobile_device_error_t orca_mobile_device_wakeup(orca_mobile_device_session_t* session)
{
    return guarded([&] {
        auto session_handle = acquire_session(session);
        if (!session_handle)
            return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
        auto* session_state = static_cast<orca_mobile_device_session_native*>(session_handle.object());
        {
            std::lock_guard<std::mutex> lock(session_state->mutex);
            if (session_state->stopping)
                return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE;
            ++session_state->wake_generation;
        }
        session_state->condition.notify_all();
        return ORCA_MOBILE_DEVICE_OK;
    });
}

} // extern "C"
