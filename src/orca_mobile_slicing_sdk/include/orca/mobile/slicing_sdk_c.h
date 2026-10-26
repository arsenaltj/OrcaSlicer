#pragma once
#include <stdint.h>
#if defined(_WIN32) && defined(ORCA_SLICING_BUILD)
#define ORCA_MOBILE_SLICING_API __declspec(dllexport)
#elif defined(_WIN32)
#define ORCA_MOBILE_SLICING_API __declspec(dllimport)
#else
#define ORCA_MOBILE_SLICING_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct orca_mobile_slicing_context orca_mobile_slicing_context;
typedef void (*orca_mobile_slicing_event_fn)(const char* event_json, void* user_data);
enum orca_mobile_slicing_error { ORCA_MOBILE_SLICING_OK=0, ORCA_MOBILE_SLICING_INVALID=1, ORCA_MOBILE_SLICING_IO=2, ORCA_MOBILE_SLICING_FORMAT=3, ORCA_MOBILE_SLICING_CONFIG=4, ORCA_MOBILE_SLICING_CANCELED=5, ORCA_MOBILE_SLICING_UNSUPPORTED=6, ORCA_MOBILE_SLICING_BUSY=7, ORCA_MOBILE_SLICING_INTERNAL=8, ORCA_MOBILE_SLICING_RESOURCE=9 };
ORCA_MOBILE_SLICING_API const char* orca_mobile_slicing_version(void);
ORCA_MOBILE_SLICING_API const char* orca_mobile_slicing_capabilities(void);
ORCA_MOBILE_SLICING_API orca_mobile_slicing_context* orca_mobile_slicing_create(const char* workspace_utf8);
ORCA_MOBILE_SLICING_API int orca_mobile_slicing_execute(orca_mobile_slicing_context*,const char* request_json,orca_mobile_slicing_event_fn,void*,char** result_json);
ORCA_MOBILE_SLICING_API void orca_mobile_slicing_cancel(orca_mobile_slicing_context*);
ORCA_MOBILE_SLICING_API void orca_mobile_slicing_destroy(orca_mobile_slicing_context*);
ORCA_MOBILE_SLICING_API void orca_mobile_slicing_free_string(char*);
#ifdef __cplusplus
}
#endif
