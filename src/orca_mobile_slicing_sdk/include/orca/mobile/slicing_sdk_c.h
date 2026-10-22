#pragma once
#include <stdint.h>
#if defined(_WIN32) && defined(ORCA_SLICING_BUILD)
#define ORCA_SLICE_API __declspec(dllexport)
#elif defined(_WIN32)
#define ORCA_SLICE_API __declspec(dllimport)
#else
#define ORCA_SLICE_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct orca_slicing_context orca_slicing_context;
typedef void (*orca_slicing_event_fn)(const char* event_json, void* user_data);
enum orca_slicing_error { ORCA_SLICE_OK=0, ORCA_SLICE_INVALID=1, ORCA_SLICE_IO=2, ORCA_SLICE_FORMAT=3, ORCA_SLICE_CONFIG=4, ORCA_SLICE_CANCELED=5, ORCA_SLICE_UNSUPPORTED=6, ORCA_SLICE_BUSY=7, ORCA_SLICE_INTERNAL=8, ORCA_SLICE_RESOURCE=9 };
ORCA_SLICE_API const char* orca_slicing_version(void);
ORCA_SLICE_API const char* orca_slicing_capabilities(void);
ORCA_SLICE_API orca_slicing_context* orca_slicing_create(const char* workspace_utf8);
ORCA_SLICE_API int orca_slicing_execute(orca_slicing_context*,const char* request_json,orca_slicing_event_fn,void*,char** result_json);
ORCA_SLICE_API void orca_slicing_cancel(orca_slicing_context*);
ORCA_SLICE_API void orca_slicing_destroy(orca_slicing_context*);
ORCA_SLICE_API void orca_slicing_free_string(char*);
#ifdef __cplusplus
}
#endif
