#pragma once
#include <stdint.h>
#if defined(_WIN32) && defined(ORCA_MODEL_BUILD)
#define ORCA_MODEL_API __declspec(dllexport)
#elif defined(_WIN32)
#define ORCA_MODEL_API __declspec(dllimport)
#else
#define ORCA_MODEL_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct orca_model_context orca_model_context;
/* Callback text is borrowed until callback returns. Never destroy from callback. */
typedef void (*orca_model_event_fn)(const char* event_json, void* user_data);
enum orca_model_error {
    ORCA_MODEL_OK          = 0,
    ORCA_MODEL_INVALID     = 1,
    ORCA_MODEL_IO          = 2,
    ORCA_MODEL_UNSUPPORTED = 3,
    ORCA_MODEL_CANCELED    = 4,
    ORCA_MODEL_BUSY        = 5,
    ORCA_MODEL_INTERNAL    = 6
};
ORCA_MODEL_API const char* orca_model_version(void);
ORCA_MODEL_API const char* orca_model_capabilities(void);
ORCA_MODEL_API orca_model_context* orca_model_create(const char* workspace_utf8);
/* UTF-8 request/result JSON. Result allocation belongs to this library. */
ORCA_MODEL_API int orca_model_execute(
    orca_model_context*, const char* request_json, orca_model_event_fn, void* user_data, char** result_json);
ORCA_MODEL_API void orca_model_cancel(orca_model_context*);
ORCA_MODEL_API void orca_model_destroy(orca_model_context*);
ORCA_MODEL_API void orca_model_free_string(char*);
/* Host performs authenticated platform HTTP only. Returned UTF-8 response is
 * borrowed until the next callback. SDK copies it before invoking host again.
 * Configure before execution; host lifetime must exceed the context. */
typedef int (*orca_model_platform_request_fn)(
    const char* method, const char* path, const char* body_json, void* user_data, const char** response_json);
ORCA_MODEL_API int orca_model_set_platform_transport(orca_model_context*, orca_model_platform_request_fn, void* user_data);
/* Trusted host configuration, never copied from an untrusted model/job request.
 * Host verifies the immutable bundle manifest/hash before supplying this path. */
ORCA_MODEL_API int orca_model_set_slicing_backend(orca_model_context*, const char* library_utf8);
#ifdef __cplusplus
}
#endif
