#pragma once

namespace orca_model_detail {
// Private policy shared with a host test; it never resolves or probes syscalls.
using ExclusiveRename = int (*)(int, const char*, int, const char*, unsigned);
using LegacyLink = int (*)(const char*, const char*);
struct ExclusivePublishResult { int status; bool consumed_temporary; };
inline ExclusivePublishResult android_exclusive_publish(
    int api, ExclusiveRename rename_exclusive, LegacyLink legacy,
    const char* temporary, const char* destination)
{
    if (api >= 30 && rename_exclusive) {
        const int status = rename_exclusive(-100, temporary, -100, destination, 1u);
        return {status, status == 0};
    }
    return {legacy(temporary, destination), false};
}
}
