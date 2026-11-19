#pragma once
#include <cerrno>
namespace orca_model_detail {
enum class IoBoundary { file_open, file_read, temp_create, temp_write, temp_flush, output_publish };
enum class IoKind { unknown, access_denied, no_space, quota_exceeded, read_only, not_found, too_many_files, unsupported, cross_device, io_failure, interrupted };
struct IoDiagnostic { IoBoundary boundary; IoKind kind; };
#ifdef ORCA_MODEL_IO_TESTING
inline int io_test_boundary=-1,io_test_code=0,io_test_mode=0;
inline bool io_fault(IoBoundary boundary) {
    if(io_test_boundary!=static_cast<int>(boundary))return false;
    io_test_boundary=-1;errno=io_test_code;
#ifdef _WIN32
    SetLastError(static_cast<DWORD>(io_test_code));
#endif
    return true;
}
#else
inline bool io_fault(IoBoundary) { return false; }
#endif
inline const char* io_name(IoBoundary value) {
    switch(value) {
    case IoBoundary::file_open:return "file_open";case IoBoundary::file_read:return "file_read";
    case IoBoundary::temp_create:return "temp_create";case IoBoundary::temp_write:return "temp_write";
    case IoBoundary::temp_flush:return "temp_flush";case IoBoundary::output_publish:return "output_publish";
    }return nullptr;
}
inline const char* io_name(IoKind value) {
    switch(value) {
    case IoKind::unknown:return "unknown";case IoKind::access_denied:return "access_denied";case IoKind::no_space:return "no_space";
    case IoKind::quota_exceeded:return "quota_exceeded";case IoKind::read_only:return "read_only";case IoKind::not_found:return "not_found";
    case IoKind::too_many_files:return "too_many_files";case IoKind::unsupported:return "unsupported";case IoKind::cross_device:return "cross_device";
    case IoKind::io_failure:return "io_failure";case IoKind::interrupted:return "interrupted";
    }return nullptr;
}
inline IoKind io_errno(int code) {
    switch(code) {
    case EACCES:case EPERM:return IoKind::access_denied;case ENOSPC:return IoKind::no_space;
#ifdef EDQUOT
    case EDQUOT:return IoKind::quota_exceeded;
#endif
    case EROFS:return IoKind::read_only;case ENOENT:return IoKind::not_found;case EMFILE:case ENFILE:return IoKind::too_many_files;
    case ENOSYS:return IoKind::unsupported;
#ifdef EOPNOTSUPP
    case EOPNOTSUPP:return IoKind::unsupported;
#endif
    case EXDEV:return IoKind::cross_device;case EIO:return IoKind::io_failure;case EINTR:return IoKind::interrupted;
    default:return IoKind::unknown;
    }
}
#ifdef _WIN32
inline IoKind io_windows(DWORD code) {
    switch(code) {
    case ERROR_ACCESS_DENIED:case ERROR_SHARING_VIOLATION:case ERROR_LOCK_VIOLATION:return IoKind::access_denied;
    case ERROR_DISK_FULL:case ERROR_HANDLE_DISK_FULL:return IoKind::no_space;
    case ERROR_DISK_QUOTA_EXCEEDED:return IoKind::quota_exceeded;case ERROR_WRITE_PROTECT:return IoKind::read_only;
    case ERROR_FILE_NOT_FOUND:case ERROR_PATH_NOT_FOUND:return IoKind::not_found;case ERROR_TOO_MANY_OPEN_FILES:return IoKind::too_many_files;
    case ERROR_NOT_SUPPORTED:case ERROR_CALL_NOT_IMPLEMENTED:case ERROR_INVALID_FUNCTION:return IoKind::unsupported;
    case ERROR_NOT_SAME_DEVICE:return IoKind::cross_device;case ERROR_IO_DEVICE:return IoKind::io_failure;
    case ERROR_OPERATION_ABORTED:return IoKind::interrupted;default:return IoKind::unknown;
    }
}
#endif
}
