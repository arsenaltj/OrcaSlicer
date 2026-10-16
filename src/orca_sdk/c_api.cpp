#include "c_api_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace orca_sdk_internal {

struct handle_token {
    void* object{nullptr};
    handle_kind kind{handle_kind::sdk};
    void* owner{nullptr};
    bool alive{true};
    size_t active{0};
};

namespace {

std::mutex g_handles_mutex;
std::condition_variable g_handles_condition;
std::unordered_map<void*, std::shared_ptr<handle_token>> g_handles;

} // namespace

handle_guard::handle_guard(handle_guard&& other) noexcept
    : m_token(std::move(other.m_token))
    , m_object(other.m_object)
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

handle_guard acquire(void* object, handle_kind kind) noexcept
{
    if (object == nullptr)
        return {};

    std::lock_guard<std::mutex> lock(g_handles_mutex);
    const auto it = g_handles.find(object);
    if (it == g_handles.end() || !it->second->alive || it->second->kind != kind)
        return {};
    ++it->second->active;
    return handle_guard(it->second, object);
}

void register_handle(void* object, handle_kind kind, void* owner)
{
    if (object == nullptr)
        throw std::invalid_argument("cannot register a null handle");

    auto token = std::make_shared<handle_token>();
    token->object = object;
    token->kind = kind;
    token->owner = owner;

    std::lock_guard<std::mutex> lock(g_handles_mutex);
    const auto [it, inserted] = g_handles.emplace(object, std::move(token));
    if (!inserted)
        throw std::logic_error("handle is already registered");
}

void retire(handle_guard& guard) noexcept
{
    if (!guard.m_token || guard.m_object == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_handles_mutex);
    guard.m_token->alive = false;
    const auto it = g_handles.find(guard.m_object);
    if (it != g_handles.end() && it->second == guard.m_token)
        g_handles.erase(it);
}

void retire_owned_handles(void* owner) noexcept
{
    if (owner == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_handles_mutex);
    for (const auto& [object, token] : g_handles) {
        (void)object;
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
        const auto current = g_handles.find(token->object);
        if (current != g_handles.end() && current->second == token)
            g_handles.erase(current);
    }
}

bool has_field(uint32_t struct_size, size_t offset, size_t field_size) noexcept
{
    return offset <= static_cast<size_t>(struct_size) &&
           field_size <= static_cast<size_t>(struct_size) - offset;
}

bool valid_utf8(orca_sdk_string_t value) noexcept
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

std::string copy_string(orca_sdk_string_t value)
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

orca_sdk_error_t write_view(orca_sdk_string_t* output, const std::string& value) noexcept
{
    if (output == nullptr)
        return ORCA_SDK_E_INVALID_ARGUMENT;
    if (value.size() > std::numeric_limits<uint32_t>::max())
        return ORCA_SDK_E_BUFFER_TOO_SMALL;
    output->data = value.data();
    output->size = static_cast<uint32_t>(value.size());
    return ORCA_SDK_OK;
}

void reset_event(orca_sdk_event_t* event) noexcept
{
    if (event == nullptr)
        return;
    const uint32_t size = event->struct_size;
    std::memset(event, 0, std::min<size_t>(size, sizeof(*event)));
    event->struct_size = size;
}

bool fill_string(orca_sdk_string_t* output, const std::string& value) noexcept
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

void fill_event(const orca_sdk_event_record& record, orca_sdk_event_t* event) noexcept
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
        event->status = ORCA_SDK_E_BUFFER_TOO_SMALL;
}

} // namespace orca_sdk_internal

namespace {

template <typename Function>
orca_sdk_error_t guarded(Function&& function) noexcept
{
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return ORCA_SDK_E_OUT_OF_MEMORY;
    } catch (...) {
        return ORCA_SDK_E_INTERNAL;
    }
}

orca_sdk_error_t validate_output(void* output, uint32_t struct_size, size_t required_size)
{
    if (output == nullptr)
        return ORCA_SDK_E_INVALID_ARGUMENT;
    if (struct_size < sizeof(uint32_t) || struct_size < required_size)
        return ORCA_SDK_E_STRUCT_TOO_SMALL;
    return ORCA_SDK_OK;
}

} // namespace

orca_sdk_session::~orca_sdk_session()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    condition.notify_all();
    if (worker.joinable())
        worker.join();
}

void orca_sdk_session::start_worker()
{
    worker = std::thread([this] { worker_main(); });
}

void orca_sdk_session::enqueue_event(orca_sdk_event_record event)
{
    event.sequence = next_event_sequence++;
    event.timestamp_ms = orca_sdk_internal::now_millis();
    events.push_back(std::move(event));
    condition.notify_all();
}

void orca_sdk_session::worker_main() noexcept
{
    try {
        for (;;) {
            orca_sdk_task_t* task = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] {
                    if (stopping)
                        return true;
                    return std::any_of(tasks.begin(), tasks.end(), [](const auto& candidate) {
                        return candidate->state == ORCA_SDK_TASK_PENDING && !candidate->cancel_requested;
                    });
                });
                if (stopping)
                    return;
                const auto it = std::find_if(tasks.begin(), tasks.end(), [](const auto& candidate) {
                    return candidate->state == ORCA_SDK_TASK_PENDING && !candidate->cancel_requested;
                });
                if (it == tasks.end())
                    continue;
                task = it->get();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            std::lock_guard<std::mutex> lock(mutex);
            if (stopping)
                return;
            if (task->state != ORCA_SDK_TASK_PENDING || task->cancel_requested)
                continue;
            task->state = ORCA_SDK_TASK_COMPLETED;
            enqueue_event({
                ORCA_SDK_EVENT_TASK_COMPLETED,
                ORCA_SDK_OK,
                0,
                0,
                task,
                task->id,
                "task completed",
                task->payload,
            });
        }
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(mutex);
            if (!stopping)
                enqueue_event({ORCA_SDK_EVENT_ERROR, ORCA_SDK_E_INTERNAL, 0, 0, nullptr, {}, "worker failure", {}});
        } catch (...) {
        }
    }
}

namespace {

using orca_sdk_internal::acquire;
using orca_sdk_internal::handle_kind;
using orca_sdk_internal::handle_guard;

handle_guard acquire_sdk(orca_sdk_t* sdk)
{
    return acquire(sdk, handle_kind::sdk);
}

handle_guard acquire_session(orca_sdk_session_t* session)
{
    return acquire(session, handle_kind::session);
}

handle_guard acquire_task(orca_sdk_task_t* task)
{
    return acquire(task, handle_kind::task);
}

} // namespace

extern "C" {

const char* orca_sdk_abi_version(void)
{
    return "1.0";
}

const char* orca_sdk_contract_version(void)
{
    return ORCA_MOBILE_SDK_CONTRACT_VERSION;
}

const char* orca_sdk_source_commit(void)
{
    return ORCA_MOBILE_SDK_SOURCE_COMMIT;
}

const char* orca_sdk_source_commit_short(void)
{
    return ORCA_MOBILE_SDK_SOURCE_COMMIT_SHORT;
}

const char* orca_sdk_error_message(orca_sdk_error_t error)
{
    switch (error) {
    case ORCA_SDK_OK: return "ok";
    case ORCA_SDK_E_INVALID_ARGUMENT: return "invalid argument";
    case ORCA_SDK_E_INVALID_HANDLE: return "invalid handle";
    case ORCA_SDK_E_STRUCT_TOO_SMALL: return "struct is too small";
    case ORCA_SDK_E_BUFFER_TOO_SMALL: return "buffer is too small";
    case ORCA_SDK_E_TIMEOUT: return "poll timed out";
    case ORCA_SDK_E_CANCELED: return "task canceled";
    case ORCA_SDK_E_ALREADY_COMPLETE: return "task already completed";
    case ORCA_SDK_E_BUSY: return "resource is busy";
    case ORCA_SDK_E_UNSUPPORTED: return "operation is unsupported";
    case ORCA_SDK_E_WAKEUP: return "poll was woken";
    case ORCA_SDK_E_OUT_OF_MEMORY: return "out of memory";
    case ORCA_SDK_E_INTERNAL: return "internal error";
    case ORCA_SDK_E_ABI_MISMATCH: return "ABI mismatch";
    default: return "unknown error";
    }
}

orca_sdk_error_t orca_sdk_get_abi_version(orca_sdk_string_t* out_version)
{
    return guarded([&] { return orca_sdk_internal::write_view(out_version, orca_sdk_abi_version()); });
}

orca_sdk_error_t orca_sdk_get_contract_version(orca_sdk_string_t* out_version)
{
    return guarded([&] { return orca_sdk_internal::write_view(out_version, orca_sdk_contract_version()); });
}

orca_sdk_error_t orca_sdk_get_source_commit(orca_sdk_string_t* out_version)
{
    return guarded([&] { return orca_sdk_internal::write_view(out_version, orca_sdk_source_commit()); });
}

orca_sdk_error_t orca_sdk_create(const orca_sdk_options_t* options, orca_sdk_t** out_sdk)
{
    return guarded([&] {
        if (out_sdk == nullptr)
            return ORCA_SDK_E_INVALID_ARGUMENT;
        *out_sdk = nullptr;
        if (options != nullptr && options->struct_size < sizeof(uint32_t))
            return ORCA_SDK_E_STRUCT_TOO_SMALL;

        auto sdk = std::make_unique<orca_sdk_t>();
        orca_sdk_internal::register_handle(sdk.get(), handle_kind::sdk, nullptr);
        *out_sdk = sdk.release();
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_destroy(orca_sdk_t* sdk)
{
    return guarded([&] {
        auto sdk_handle = acquire_sdk(sdk);
        if (!sdk_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        const auto token = sdk_handle.token();
        {
            std::lock_guard<std::mutex> lock(sdk->mutex);
            if (sdk->shutting_down || sdk->session_count != 0)
                return ORCA_SDK_E_BUSY;
            sdk->shutting_down = true;
            sdk->magic = 0;
        }

        // Retire the registry entry before releasing the final guard. No C ABI
        // call can start after this point, while calls already in flight keep
        // the object alive until their guards are released.
        orca_sdk_internal::retire(sdk_handle);
        sdk_handle.release();
        orca_sdk_internal::wait_idle(token);
        delete sdk;
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_session_create(
    orca_sdk_t* sdk,
    const orca_sdk_session_options_t* options,
    orca_sdk_session_t** out_session)
{
    return guarded([&] {
        if (out_session == nullptr)
            return ORCA_SDK_E_INVALID_ARGUMENT;
        *out_session = nullptr;
        auto sdk_handle = acquire_sdk(sdk);
        if (!sdk_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        if (options != nullptr && options->struct_size < sizeof(uint32_t))
            return ORCA_SDK_E_STRUCT_TOO_SMALL;
        if (options != nullptr &&
            orca_sdk_internal::has_field(options->struct_size,
                                         offsetof(orca_sdk_session_options_t, device_id),
                                         sizeof(options->device_id)) &&
            !orca_sdk_internal::valid_utf8(options->device_id))
            return ORCA_SDK_E_INVALID_ARGUMENT;

        auto session = std::make_unique<orca_sdk_session_t>(sdk);
        {
            std::lock_guard<std::mutex> lock(sdk->mutex);
            if (sdk->shutting_down)
                return ORCA_SDK_E_BUSY;
            ++sdk->session_count;
        }
        try {
            session->start_worker();
            orca_sdk_internal::register_handle(session.get(), handle_kind::session, sdk);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(sdk->mutex);
                --sdk->session_count;
            }
            throw;
        }
        *out_session = session.release();
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_session_destroy(orca_sdk_session_t* session)
{
    return guarded([&] {
        auto session_handle = acquire_session(session);
        if (!session_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        const auto token = session_handle.token();
        orca_sdk_t* sdk = session->sdk;

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->stopping)
                return ORCA_SDK_E_BUSY;
            session->stopping = true;
            session->magic = 0;
        }
        orca_sdk_internal::retire_owned_handles(session);
        orca_sdk_internal::retire(session_handle);
        session->condition.notify_all();
        if (session->worker.joinable())
            session->worker.join();

        // Existing task calls are allowed to finish, but no task call can
        // start once destruction begins. Delete the task storage only after
        // every in-flight task operation has released its guard.
        orca_sdk_internal::wait_owned_handles(session);
        session_handle.release();
        orca_sdk_internal::wait_idle(token);

        auto sdk_handle = acquire_sdk(sdk);
        if (sdk_handle) {
            std::lock_guard<std::mutex> lock(sdk->mutex);
            if (sdk->session_count != 0)
                --sdk->session_count;
        }
        delete session;
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_session_submit_task(
    orca_sdk_session_t* session,
    const orca_sdk_task_request_t* request,
    orca_sdk_task_t** out_task)
{
    return guarded([&] {
        if (out_task == nullptr)
            return ORCA_SDK_E_INVALID_ARGUMENT;
        *out_task = nullptr;
        auto session_handle = acquire_session(session);
        if (!session_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        if (request == nullptr || request->struct_size < sizeof(uint32_t))
            return request == nullptr ? ORCA_SDK_E_INVALID_ARGUMENT : ORCA_SDK_E_STRUCT_TOO_SMALL;

        orca_sdk_string_t payload{};
        uint32_t kind = ORCA_SDK_TASK_KIND_SYNC;
        if (orca_sdk_internal::has_field(request->struct_size,
                                         offsetof(orca_sdk_task_request_t, kind), sizeof(request->kind)))
            kind = request->kind;
        if (kind != ORCA_SDK_TASK_KIND_SYNC)
            return ORCA_SDK_E_UNSUPPORTED;
        if (orca_sdk_internal::has_field(request->struct_size,
                                         offsetof(orca_sdk_task_request_t, payload_utf8),
                                         sizeof(request->payload_utf8)))
            payload = request->payload_utf8;
        if (!orca_sdk_internal::valid_utf8(payload))
            return ORCA_SDK_E_INVALID_ARGUMENT;

        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->stopping)
            return ORCA_SDK_E_BUSY;
        auto task = std::make_unique<orca_sdk_task_t>();
        task->session = session;
        task->numeric_id = session->next_task_id++;
        task->id = "task-" + std::to_string(task->numeric_id);
        task->payload = orca_sdk_internal::copy_string(payload);
        orca_sdk_task_t* task_pointer = task.get();
        orca_sdk_internal::register_handle(task_pointer, handle_kind::task, session);
        session->tasks.push_back(std::move(task));
        session->enqueue_event({
            ORCA_SDK_EVENT_TASK_SUBMITTED,
            ORCA_SDK_OK,
            0,
            0,
            task_pointer,
            task_pointer->id,
            "task submitted",
            {},
        });
        *out_task = task_pointer;
        session->condition.notify_all();
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_task_cancel(orca_sdk_task_t* task)
{
    return guarded([&] {
        auto task_handle = acquire_task(task);
        if (!task_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        orca_sdk_session_t* session = task->session;
        std::lock_guard<std::mutex> lock(session->mutex);
        if (task->magic != orca_sdk_task_t::kMagic || task->user_destroyed)
            return ORCA_SDK_E_INVALID_HANDLE;
        if (task->state != ORCA_SDK_TASK_PENDING)
            return ORCA_SDK_E_ALREADY_COMPLETE;
        task->cancel_requested = true;
        task->state = ORCA_SDK_TASK_CANCELED;
        if (!task->cancel_event_enqueued) {
            task->cancel_event_enqueued = true;
            session->enqueue_event({
                ORCA_SDK_EVENT_TASK_CANCELED,
                ORCA_SDK_E_CANCELED,
                0,
                0,
                task,
                task->id,
                "task canceled",
                {},
            });
        }
        session->condition.notify_all();
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_task_destroy(orca_sdk_task_t* task)
{
    return guarded([&] {
        auto task_handle = acquire_task(task);
        if (!task_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        const auto token = task_handle.token();
        orca_sdk_session_t* session = task->session;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (task->magic != orca_sdk_task_t::kMagic || task->user_destroyed)
                return ORCA_SDK_E_INVALID_HANDLE;
            task->user_destroyed = true;
            task->magic = 0;
        }
        orca_sdk_internal::retire(task_handle);
        task_handle.release();
        orca_sdk_internal::wait_idle(token);
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_task_get_id(
    const orca_sdk_task_t* task,
    orca_sdk_string_t* out_task_id)
{
    return guarded([&] {
        auto task_handle = acquire_task(const_cast<orca_sdk_task_t*>(task));
        if (!task_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        if (out_task_id == nullptr)
            return ORCA_SDK_E_INVALID_ARGUMENT;
        auto* mutable_task = static_cast<orca_sdk_task_t*>(task_handle.object());
        std::lock_guard<std::mutex> lock(mutable_task->session->mutex);
        if (mutable_task->magic != orca_sdk_task_t::kMagic || mutable_task->user_destroyed)
            return ORCA_SDK_E_INVALID_HANDLE;
        return orca_sdk_internal::write_view(out_task_id, mutable_task->id);
    });
}

orca_sdk_error_t orca_sdk_task_get_state(const orca_sdk_task_t* task, uint32_t* out_state)
{
    return guarded([&] {
        auto task_handle = acquire_task(const_cast<orca_sdk_task_t*>(task));
        if (!task_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        if (out_state == nullptr)
            return ORCA_SDK_E_INVALID_ARGUMENT;
        auto* mutable_task = static_cast<orca_sdk_task_t*>(task_handle.object());
        std::lock_guard<std::mutex> lock(mutable_task->session->mutex);
        if (mutable_task->magic != orca_sdk_task_t::kMagic || mutable_task->user_destroyed)
            return ORCA_SDK_E_INVALID_HANDLE;
        *out_state = mutable_task->state;
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_poll_event(
    orca_sdk_session_t* session,
    uint32_t timeout_ms,
    orca_sdk_event_t* out_event)
{
    return guarded([&] {
        auto session_handle = acquire_session(session);
        if (!session_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        const orca_sdk_error_t output_result = validate_output(
            out_event,
            out_event == nullptr ? 0 : out_event->struct_size,
            sizeof(orca_sdk_event_t));
        if (output_result != ORCA_SDK_OK)
            return output_result;

        std::unique_lock<std::mutex> lock(session->mutex);
        const uint64_t observed_wake = session->wake_generation;
        const auto ready = [session, observed_wake] {
            return session->stopping || session->wake_generation != observed_wake || !session->events.empty();
        };
        if (!ready()) {
            if (timeout_ms == 0) {
                return ORCA_SDK_E_TIMEOUT;
            } else if (timeout_ms == ORCA_SDK_WAIT_INFINITE) {
                session->condition.wait(lock, ready);
            } else if (!session->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
                return ORCA_SDK_E_TIMEOUT;
            }
        }
        if (session->stopping)
            return ORCA_SDK_E_INVALID_HANDLE;
        if (session->events.empty() && session->wake_generation != observed_wake)
            return ORCA_SDK_E_WAKEUP;
        if (session->events.empty())
            return ORCA_SDK_E_TIMEOUT;

        session->delivered_events.push_back(std::move(session->events.front()));
        session->events.pop_front();
        orca_sdk_internal::fill_event(session->delivered_events.back(), out_event);
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_event_clear(orca_sdk_event_t* event)
{
    return guarded([&] {
        if (event == nullptr)
            return ORCA_SDK_E_INVALID_ARGUMENT;
        if (event->struct_size < sizeof(uint32_t))
            return ORCA_SDK_E_STRUCT_TOO_SMALL;
        orca_sdk_internal::reset_event(event);
        return ORCA_SDK_OK;
    });
}

orca_sdk_error_t orca_sdk_wakeup(orca_sdk_session_t* session)
{
    return guarded([&] {
        auto session_handle = acquire_session(session);
        if (!session_handle)
            return ORCA_SDK_E_INVALID_HANDLE;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->stopping)
                return ORCA_SDK_E_INVALID_HANDLE;
            ++session->wake_generation;
        }
        session->condition.notify_all();
        return ORCA_SDK_OK;
    });
}

} // extern "C"