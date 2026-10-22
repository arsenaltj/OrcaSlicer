#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(ORCA_MOBILE_DEVICE_BUILD_SHARED)
#    if defined(ORCA_MOBILE_DEVICE_BUILDING)
#      define ORCA_MOBILE_DEVICE_API __declspec(dllexport)
#    else
#      define ORCA_MOBILE_DEVICE_API __declspec(dllimport)
#    endif
#  else
#    define ORCA_MOBILE_DEVICE_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define ORCA_MOBILE_DEVICE_API __attribute__((visibility("default")))
#else
#  define ORCA_MOBILE_DEVICE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ORCA_MOBILE_DEVICE_ABI_VERSION_MAJOR 1u
#define ORCA_MOBILE_DEVICE_ABI_VERSION_MINOR 0u
#define ORCA_MOBILE_DEVICE_WAIT_INFINITE UINT32_MAX

typedef int32_t orca_mobile_device_error_t;
typedef orca_mobile_device_error_t orca_mobile_device_result_t;

#define ORCA_MOBILE_DEVICE_OK ((orca_mobile_device_error_t)0)
#define ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT ((orca_mobile_device_error_t)-1)
#define ORCA_MOBILE_DEVICE_E_INVALID_HANDLE ((orca_mobile_device_error_t)-2)
#define ORCA_MOBILE_DEVICE_E_STRUCT_TOO_SMALL ((orca_mobile_device_error_t)-3)
#define ORCA_MOBILE_DEVICE_E_BUFFER_TOO_SMALL ((orca_mobile_device_error_t)-4)
#define ORCA_MOBILE_DEVICE_E_TIMEOUT ((orca_mobile_device_error_t)-5)
#define ORCA_MOBILE_DEVICE_E_CANCELED ((orca_mobile_device_error_t)-6)
#define ORCA_MOBILE_DEVICE_E_ALREADY_COMPLETE ((orca_mobile_device_error_t)-7)
#define ORCA_MOBILE_DEVICE_E_BUSY ((orca_mobile_device_error_t)-8)
#define ORCA_MOBILE_DEVICE_E_UNSUPPORTED ((orca_mobile_device_error_t)-9)
#define ORCA_MOBILE_DEVICE_E_WAKEUP ((orca_mobile_device_error_t)-10)
#define ORCA_MOBILE_DEVICE_E_OUT_OF_MEMORY ((orca_mobile_device_error_t)-11)
#define ORCA_MOBILE_DEVICE_E_INTERNAL ((orca_mobile_device_error_t)-12)
#define ORCA_MOBILE_DEVICE_E_ABI_MISMATCH ((orca_mobile_device_error_t)-13)

typedef struct orca_mobile_device orca_mobile_device_t;
typedef struct orca_mobile_device_session orca_mobile_device_session_t;
typedef struct orca_mobile_device_task orca_mobile_device_task_t;

/* Strings are UTF-8 byte spans; data is not required to be NUL-terminated. */
typedef struct orca_mobile_device_string {
    const char* data;
    uint32_t size;
} orca_mobile_device_string_t;

typedef struct orca_mobile_device_bytes {
    const uint8_t* data;
    uint32_t size;
} orca_mobile_device_bytes_t;

typedef struct orca_mobile_device_options {
    uint32_t struct_size;
    uint32_t flags;
} orca_mobile_device_options_t;

typedef struct orca_mobile_device_session_options {
    uint32_t struct_size;
    uint32_t flags;
    orca_mobile_device_string_t device_id;
} orca_mobile_device_session_options_t;

typedef struct orca_mobile_device_task_request {
    uint32_t struct_size;
    uint32_t kind;
    orca_mobile_device_string_t payload_utf8;
} orca_mobile_device_task_request_t;

typedef struct orca_mobile_device_event {
    uint32_t struct_size;
    uint32_t type;
    orca_mobile_device_error_t status;
    uint64_t sequence;
    uint64_t timestamp_ms;
    orca_mobile_device_task_t* task;
    orca_mobile_device_string_t task_id;
    orca_mobile_device_string_t message_utf8;
    orca_mobile_device_bytes_t payload;
} orca_mobile_device_event_t;

enum {
    ORCA_MOBILE_DEVICE_TASK_KIND_SYNC = 1u,
    /* JSON payload task kinds for the v1.7 asynchronous C ABI facade. */
    ORCA_MOBILE_DEVICE_TASK_KIND_DISCOVER = 2u,
    ORCA_MOBILE_DEVICE_TASK_KIND_CONNECT = 3u,
    ORCA_MOBILE_DEVICE_TASK_KIND_DISCONNECT = 4u,
    ORCA_MOBILE_DEVICE_TASK_KIND_STATUS = 5u,
    ORCA_MOBILE_DEVICE_TASK_KIND_COMMAND = 6u,
    ORCA_MOBILE_DEVICE_TASK_KIND_TRANSFER = 7u,
};

enum {
    ORCA_MOBILE_DEVICE_EVENT_NONE = 0u,
    ORCA_MOBILE_DEVICE_EVENT_SESSION_READY = 1u,
    ORCA_MOBILE_DEVICE_EVENT_TASK_SUBMITTED = 2u,
    ORCA_MOBILE_DEVICE_EVENT_TASK_COMPLETED = 3u,
    ORCA_MOBILE_DEVICE_EVENT_TASK_CANCELED = 4u,
    ORCA_MOBILE_DEVICE_EVENT_ERROR = 5u,
};

enum {
    ORCA_MOBILE_DEVICE_TASK_PENDING = 0u,
    ORCA_MOBILE_DEVICE_TASK_COMPLETED = 1u,
    ORCA_MOBILE_DEVICE_TASK_CANCELED = 2u,
};

ORCA_MOBILE_DEVICE_API const char* orca_mobile_device_abi_version(void);
ORCA_MOBILE_DEVICE_API const char* orca_mobile_device_contract_version(void);
ORCA_MOBILE_DEVICE_API const char* orca_mobile_device_source_commit(void);
ORCA_MOBILE_DEVICE_API const char* orca_mobile_device_source_commit_short(void);
ORCA_MOBILE_DEVICE_API const char* orca_mobile_device_error_message(orca_mobile_device_error_t error);

ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_get_abi_version(orca_mobile_device_string_t* out_version);
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_get_contract_version(orca_mobile_device_string_t* out_version);
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_get_source_commit(orca_mobile_device_string_t* out_version);

ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_create(
    const orca_mobile_device_options_t* options,
    orca_mobile_device_t** out_sdk);
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_destroy(orca_mobile_device_t* sdk);

ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_session_create(
    orca_mobile_device_t* sdk,
    const orca_mobile_device_session_options_t* options,
    orca_mobile_device_session_t** out_session);
/*
 * Destruction marks the session invalid, wakes blocked pollers, waits for
 * in-flight session/task calls, and then releases the native object. Calls
 * that start after destruction begins return ORCA_MOBILE_DEVICE_E_INVALID_HANDLE.
 */
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_session_destroy(orca_mobile_device_session_t* session);
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_session_submit_task(
    orca_mobile_device_session_t* session,
    const orca_mobile_device_task_request_t* request,
    orca_mobile_device_task_t** out_task);
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_task_cancel(orca_mobile_device_task_t* task);
/* A pending task, or a terminal task with an uncleared task event, returns BUSY from task_destroy. After task_destroy succeeds, every task API returns INVALID_HANDLE. */
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_task_destroy(orca_mobile_device_task_t* task);
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_task_get_id(
    const orca_mobile_device_task_t* task,
    orca_mobile_device_string_t* out_task_id);
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_task_get_state(
    const orca_mobile_device_task_t* task,
    uint32_t* out_state);

ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_poll_event(
    orca_mobile_device_session_t* session,
    uint32_t timeout_ms,
    orca_mobile_device_event_t* out_event);
/* Releases the storage backing the most recently polled event for this output. */
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_event_clear(orca_mobile_device_event_t* event);
/* Wakeup is a broadcast edge for pollers already waiting on this session. */
ORCA_MOBILE_DEVICE_API orca_mobile_device_error_t orca_mobile_device_wakeup(orca_mobile_device_session_t* session);

#ifdef __cplusplus
} /* extern "C" */
#endif
