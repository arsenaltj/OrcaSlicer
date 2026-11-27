#pragma once

#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>

namespace Slic3r::AI {

// Keep filesystem syntax at the I/O boundary. History and model identities
// continue to use ordinary absolute or library-relative paths.
inline boost::filesystem::path model_io_path(const boost::filesystem::path& path)
{
#ifdef _WIN32
    // Legacy Windows directory creation leaves room for an 8.3 name.
    if (!path.empty() && (!path.is_absolute() || path.native().size() >= 248))
        return Slic3r::extended_path(path);
#endif
    return path;
}

inline std::string model_io_string(const boost::filesystem::path& path)
{
#ifdef _WIN32
    return boost::nowide::narrow(model_io_path(path).native());
#else
    return path.native();
#endif
}

inline boost::filesystem::path model_path_from_io(const boost::filesystem::path& path)
{
#ifdef _WIN32
    const auto& name = path.native();
    if (name.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return boost::filesystem::path(L"\\\\" + name.substr(8));
    if (name.rfind(L"\\\\?\\", 0) == 0)
        return boost::filesystem::path(name.substr(4));
#endif
    return path;
}

inline boost::filesystem::path canonical_model_path(const boost::filesystem::path& path,
                                                    boost::system::error_code& error)
{
    return model_path_from_io(boost::filesystem::canonical(model_io_path(path), error));
}

inline boost::filesystem::path canonical_model_path(const boost::filesystem::path& path)
{
    return model_path_from_io(boost::filesystem::canonical(model_io_path(path)));
}

} // namespace Slic3r::AI
