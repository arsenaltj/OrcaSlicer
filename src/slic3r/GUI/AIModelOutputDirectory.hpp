#pragma once

#include "libslic3r/Utils.hpp"
#include "GUI.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/process/environment.hpp>
#include <wx/stdpaths.h>

namespace Slic3r::GUI {

class AIModelOutputDirectory
{
public:
    explicit AIModelOutputDirectory(const boost::filesystem::path& installation_directory)
    {
        const char* configured = boost::nowide::getenv("ORCASLICER_AI_OUTPUT_DIR");
        const boost::filesystem::path selected = configured != nullptr && configured[0] != '\0'
            ? boost::filesystem::path(configured) : installation_directory / "models";
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
    // The actual executable location anchors both desktop history and the child
    // process output. Neither data-dir overrides nor file dialogs change it.
    static const AIModelOutputDirectory directory(into_path(wxStandardPaths::Get().GetExecutablePath()).parent_path());
    return directory;
}

inline boost::filesystem::path default_model_project_path(const boost::filesystem::path& proposed)
{
    const auto directory = ai_model_output_directory().root() / "projects";
    boost::filesystem::create_directories(directory);
    return directory / proposed.filename();
}

} // namespace Slic3r::GUI
