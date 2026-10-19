#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(ORCA_SDK_BUILD_SHARED)
#  if defined(ORCA_SDK_BUILDING)
#    define ORCA_SDK_API __declspec(dllexport)
#  else
#    define ORCA_SDK_API __declspec(dllimport)
#  endif
#else
#  define ORCA_SDK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ORCA_SDK_ABI_VERSION_MAJOR 1u
#define ORCA_SDK_ABI_VERSION_MINOR 0u
#define ORCA_SDK_WAIT_INFINITE UINT32_MAX

typedef int32_t orca_sdk_error_t;
typedef orca_sdk_error_t orca_sdk_result_t;

#define ORCA_SDK_OK ((orca_sdk_error_t)0)
#define ORCA_SDK_E_INVALID_ARGUMENT ((orca_sdk_error_t)-1)
#define ORCA_SDK_E_INVALID_HANDLE ((orca_sdk_error_t)-2)
#define ORCA_SDK_E_STRUCT_TOO_SMALL ((orca_sdk_error_t)-3)
#define ORCA_SDK_E_BUFFER_TOO_SMALL ((orca_sdk_error_t)-4)
#define ORCA_SDK_E_TIMEOUT ((orca_sdk_error_t)-5)
#define ORCA_SDK_E_CANCELED ((orca_sdk_error_t)-6)
#define ORCA_SDK_E_ALREADY_COMPLETE ((orca_sdk_error_t)-7)
#define ORCA_SDK_E_BUSY ((orca_sdk_error_t)-8)
#define ORCA_SDK_E_UNSUPPORTED ((orca_sdk_error_t)-9)
#define ORCA_SDK_E_WAKEUP ((orca_sdk_error_t)-10)
#define ORCA_SDK_E_OUT_OF_MEMORY ((orca_sdk_error_t)-11)
#define ORCA_SDK_E_INTERNAL ((orca_sdk_error_t)-12)
#define ORCA_SDK_E_ABI_MISMATCH ((orca_sdk_error_t)-13)

typedef struct orca_sdk orca_sdk_t;
typedef struct orca_sdk_session orca_sdk_session_t;
typedef struct orca_sdk_task orca_sdk_task_t;

/* Strings are UTF-8 byte spans; data is not required to be NUL-terminated. */
typedef struct orca_sdk_string {
    const char* data;
    uint32_t size;
} orca_sdk_string_t;

typedef struct orca_sdk_bytes {
    const uint8_t* data;
    uint32_t size;
} orca_sdk_bytes_t;

typedef struct orca_sdk_options {
    uint32_t struct_size;
    uint32_t flags;
} orca_sdk_options_t;

typedef struct orca_sdk_session_options {
    uint32_t struct_size;
    uint32_t flags;
    orca_sdk_string_t device_id;
} orca_sdk_session_options_t;

typedef struct orca_sdk_task_request {
    uint32_t struct_size;
    uint32_t kind;
    orca_sdk_string_t payload_utf8;
} orca_sdk_task_request_t;

typedef struct orca_sdk_event {
    uint32_t struct_size;
    uint32_t type;
    orca_sdk_error_t status;
    uint64_t sequence;
    uint64_t timestamp_ms;
    orca_sdk_task_t* task;
    orca_sdk_string_t task_id;
    orca_sdk_string_t message_utf8;
    orca_sdk_bytes_t payload;
} orca_sdk_event_t;

enum {
    ORCA_SDK_TASK_KIND_SYNC = 1u,
    /* JSON payload task kinds for the v1.7 asynchronous C ABI facade. */
    ORCA_SDK_TASK_KIND_DISCOVER = 2u,
    ORCA_SDK_TASK_KIND_CONNECT = 3u,
    ORCA_SDK_TASK_KIND_DISCONNECT = 4u,
    ORCA_SDK_TASK_KIND_STATUS = 5u,
    ORCA_SDK_TASK_KIND_COMMAND = 6u,
    ORCA_SDK_TASK_KIND_TRANSFER = 7u,
};

enum {
    ORCA_SDK_EVENT_NONE = 0u,
    ORCA_SDK_EVENT_SESSION_READY = 1u,
    ORCA_SDK_EVENT_TASK_SUBMITTED = 2u,
    ORCA_SDK_EVENT_TASK_COMPLETED = 3u,
    ORCA_SDK_EVENT_TASK_CANCELED = 4u,
    ORCA_SDK_EVENT_ERROR = 5u,
};

enum {
    ORCA_SDK_TASK_PENDING = 0u,
    ORCA_SDK_TASK_COMPLETED = 1u,
    ORCA_SDK_TASK_CANCELED = 2u,
};

ORCA_SDK_API const char* orca_sdk_abi_version(void);
ORCA_SDK_API const char* orca_sdk_contract_version(void);
ORCA_SDK_API const char* orca_sdk_source_commit(void);
ORCA_SDK_API const char* orca_sdk_source_commit_short(void);
ORCA_SDK_API const char* orca_sdk_error_message(orca_sdk_error_t error);

ORCA_SDK_API orca_sdk_error_t orca_sdk_get_abi_version(orca_sdk_string_t* out_version);
ORCA_SDK_API orca_sdk_error_t orca_sdk_get_contract_version(orca_sdk_string_t* out_version);
ORCA_SDK_API orca_sdk_error_t orca_sdk_get_source_commit(orca_sdk_string_t* out_version);

ORCA_SDK_API orca_sdk_error_t orca_sdk_create(
    const orca_sdk_options_t* options,
    orca_sdk_t** out_sdk);
ORCA_SDK_API orca_sdk_error_t orca_sdk_destroy(orca_sdk_t* sdk);

ORCA_SDK_API orca_sdk_error_t orca_sdk_session_create(
    orca_sdk_t* sdk,
    const orca_sdk_session_options_t* options,
    orca_sdk_session_t** out_session);
/*
 * Destruction marks the session invalid, wakes blocked pollers, waits for
 * in-flight session/task calls, and then releases the native object. Calls
 * that start after destruction begins return ORCA_SDK_E_INVALID_HANDLE.
 */
ORCA_SDK_API orca_sdk_error_t orca_sdk_session_destroy(orca_sdk_session_t* session);
ORCA_SDK_API orca_sdk_error_t orca_sdk_session_submit_task(
    orca_sdk_session_t* session,
    const orca_sdk_task_request_t* request,
    orca_sdk_task_t** out_task);
ORCA_SDK_API orca_sdk_error_t orca_sdk_task_cancel(orca_sdk_task_t* task);
/* A pending task, or a terminal task with an uncleared task event, returns BUSY from task_destroy. After task_destroy succeeds, every task API returns INVALID_HANDLE. */
ORCA_SDK_API orca_sdk_error_t orca_sdk_task_destroy(orca_sdk_task_t* task);
ORCA_SDK_API orca_sdk_error_t orca_sdk_task_get_id(
    const orca_sdk_task_t* task,
    orca_sdk_string_t* out_task_id);
ORCA_SDK_API orca_sdk_error_t orca_sdk_task_get_state(
    const orca_sdk_task_t* task,
    uint32_t* out_state);

ORCA_SDK_API orca_sdk_error_t orca_sdk_poll_event(
    orca_sdk_session_t* session,
    uint32_t timeout_ms,
    orca_sdk_event_t* out_event);
/* Releases the storage backing the most recently polled event for this output. */
ORCA_SDK_API orca_sdk_error_t orca_sdk_event_clear(orca_sdk_event_t* event);
/* Wakeup is a broadcast edge for pollers already waiting on this session. */
ORCA_SDK_API orca_sdk_error_t orca_sdk_wakeup(orca_sdk_session_t* session);

#ifdef __cplusplus
} /* extern "C" */
#endif
