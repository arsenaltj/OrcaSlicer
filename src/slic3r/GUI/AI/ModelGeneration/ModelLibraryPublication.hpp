#pragma once
#include "ModelLibraryObservations.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <mutex>

namespace Slic3r::GUI {
// The model is already downloaded. Replace only its registration, beside the
// original record; a failed write must leave the last complete record intact.
inline bool publish_model_library_record(const boost::filesystem::path& path, nlohmann::json metadata)
{
    static std::mutex publication_mutex;
    std::lock_guard<std::mutex> lock(publication_mutex);
    auto temporary = path;
    temporary += ".pending-" + boost::uuids::to_string(boost::uuids::random_generator()());
    struct Cleanup {
        boost::filesystem::path path;
        ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove(path, ec); }
    } cleanup {temporary};
    try {
        nlohmann::json previous;
        { boost::filesystem::ifstream input(path); if (input) previous = nlohmann::json::parse(input, nullptr, false); }
        if (ModelLibraryObservations::has_check_certificate(previous)) return true;
        if (previous.is_object())
            for (const auto* key : {"generated_at", "imported_at", "print_feedback", "print_feedback_at"})
                if (previous.contains(key)) metadata[key] = previous.at(key);
        if (!metadata.contains("generated_at")) metadata["generated_at"] = std::time(nullptr);
        { boost::filesystem::ofstream output(temporary, std::ios::binary | std::ios::trunc);
          if (!output) return false;
          output << metadata.dump(2); output.flush();
          if (!output) return false;
          output.close(); if (output.fail()) return false; }
        return !Slic3r::rename_file(boost::nowide::narrow(temporary.wstring()), boost::nowide::narrow(path.wstring()));
    } catch (...) { return false; }
}
}
