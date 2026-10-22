#pragma once

#include "libslic3r/Utils.hpp"
#include "GUI.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/process/environment.hpp>
#include <wx/app.h>
#include <wx/stdpaths.h>

namespace Slic3r::GUI {

class AIModelOutputDirectory
{
public:
    explicit AIModelOutputDirectory(const boost::filesystem::path& installation_directory,
                                    const boost::filesystem::path& user_data_directory = {})
    {
        const char* configured = boost::nowide::getenv("ORCASLICER_AI_OUTPUT_DIR");
        const boost::filesystem::path selected = configured != nullptr && configured[0] != '\0'
            ? boost::filesystem::path(configured)
            // Generated models are user data. Never default to the executable's
            // directory: normal Windows installs live below Program Files and
            // are not writable by standard users.
            : (user_data_directory.empty() ? installation_directory : user_data_directory) / "models";
        m_root = boost::filesystem::absolute(selected).lexically_normal();
        m_custom_output = configured != nullptr && configured[0] != '\0';
    }

    const boost::filesystem::path& root() const { return m_root; }

    bool migrate_legacy_history(const boost::filesystem::path& legacy, std::string& error) const
    {
        namespace fs = boost::filesystem;
        error.clear();
        if (m_custom_output) return true;
        fs::path temporary;
        try {
            fs::create_directories(m_root / "downloads");
            fs::create_directories(m_root / "exports");
            fs::create_directories(m_root / "projects");
            if (!fs::is_directory(legacy) || fs::absolute(legacy).lexically_normal() == m_root) return true;
            for (fs::recursive_directory_iterator it(legacy), end; it != end; ++it) {
                // Preserve existing install-local versions and never follow links out of old history.
                const auto status = fs::symlink_status(it->path());
                if (fs::is_symlink(status)) continue;
                const auto target = m_root / fs::relative(it->path(), legacy);
                if (fs::is_directory(status)) { fs::create_directories(target); continue; }
                if (!fs::is_regular_file(status) || fs::exists(target)) continue;
                temporary = target.parent_path() / fs::unique_path(".history-import-%%%%%%%%-%%%%%%%%");
                fs::copy_file(it->path(), temporary);
                if (!fs::exists(target)) fs::rename(temporary, target);
                else fs::remove(temporary);
                temporary.clear();
            }
            return true;
        } catch (const std::exception& exception) {
            if (!temporary.empty()) {
                boost::system::error_code ec;
                fs::remove(temporary, ec);
            }
            error = exception.what();
            return false;
        }
    }

    void configure_child_environment(boost::process::environment& environment) const
    {
        environment["ORCASLICER_AI_OUTPUT_DIR"] = m_root.string();
    }

#ifdef _WIN32
    void configure_child_environment(boost::process::wenvironment& environment) const
    {
        environment[L"ORCASLICER_AI_OUTPUT_DIR"] = m_root.wstring();
    }
#endif

private:
    boost::filesystem::path m_root;
    bool m_custom_output {false};
};

inline const AIModelOutputDirectory& ai_model_output_directory()
{
    // Keep generated assets in Orca's selected per-user data directory. The
    // executable's directory is commonly under Program Files and is read-only
    // for standard Windows users. Use wx's path only during the very early
    // startup window before GUI_App has selected data_dir(). An explicit
    // ORCASLICER_AI_OUTPUT_DIR still wins for managed deployments.
    const boost::filesystem::path user_data = !Slic3r::data_dir().empty()
        ? boost::filesystem::path(Slic3r::data_dir())
        : (wxTheApp ? into_path(wxStandardPaths::Get().GetUserDataDir())
                    : boost::filesystem::current_path());
    // Unit and command-line test hosts do not create a wxApp. wxStandardPaths
    // is not safe to query before wx startup, so retain a deterministic local
    // fallback there while the normal GUI path continues to use the executable
    // directory.
    const auto installation_directory = wxTheApp
        ? into_path(wxStandardPaths::Get().GetExecutablePath()).parent_path()
        : boost::filesystem::current_path();
    static const AIModelOutputDirectory directory(installation_directory, user_data);
    return directory;
}

inline boost::filesystem::path default_model_project_path(const boost::filesystem::path& proposed)
{
    const auto directory = ai_model_output_directory().root() / "projects";
    boost::filesystem::create_directories(directory);
    return directory / proposed.filename();
}

} // namespace Slic3r::GUI
