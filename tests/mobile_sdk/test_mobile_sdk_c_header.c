#include <orca/sdk_c.h>

int orca_sdk_c_header_compile_probe(void)
{
    orca_sdk_options_t options = {0};
    orca_sdk_session_options_t session_options = {0};
    orca_sdk_task_request_t request = {0};
    orca_sdk_event_t event = {0};
    orca_sdk_string_t string = {0};
    orca_sdk_bytes_t bytes = {0};

    options.struct_size = (uint32_t)sizeof(options);
    session_options.struct_size = (uint32_t)sizeof(session_options);
    request.struct_size = (uint32_t)sizeof(request);
    event.struct_size = (uint32_t)sizeof(event);
    string.data = "c11";
    string.size = 3u;
    bytes.data = (const uint8_t*)string.data;
    bytes.size = string.size;
    request.payload_utf8 = string;
    event.payload = bytes;

    _Static_assert(ORCA_SDK_TASK_KIND_DISCOVER == 2u, "discover kind must remain stable");
    _Static_assert(ORCA_SDK_TASK_KIND_CONNECT == 3u, "connect kind must remain stable");
    _Static_assert(ORCA_SDK_TASK_KIND_DISCONNECT == 4u, "disconnect kind must remain stable");
    _Static_assert(ORCA_SDK_TASK_KIND_STATUS == 5u, "status kind must remain stable");
    _Static_assert(ORCA_SDK_TASK_KIND_COMMAND == 6u, "command kind must remain stable");
    _Static_assert(ORCA_SDK_TASK_KIND_TRANSFER == 7u, "transfer kind must remain stable");

    return (int)(options.struct_size + session_options.struct_size + request.struct_size +
                 event.struct_size + request.payload_utf8.size + event.payload.size);
}
