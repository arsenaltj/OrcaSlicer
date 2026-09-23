#pragma once

#include <boost/filesystem.hpp>

namespace Slic3r::AI {

// Finishing versions retain the original model's directory so their base
// texture reference remains local. Their authoritative history record lives
// in downloads; generated originals and external models use adjacent drafts.
inline boost::filesystem::path beauty_metadata_path(const boost::filesystem::path& model,
                                                    const boost::filesystem::path& library_root)
{
    auto adjacent=model;adjacent.replace_extension(".json");
    if(model.filename().string().find("orcaslicer-ai-finish-")!=0)return adjacent;
    boost::system::error_code ec;
    const auto canonical_root=boost::filesystem::canonical(library_root,ec);
    if(ec)return adjacent;
    const auto canonical_model=boost::filesystem::canonical(model,ec);
    if(ec)return adjacent;
    const auto relative=canonical_model.lexically_relative(canonical_root);
    if(relative.empty() || relative.is_absolute() || *relative.begin()=="..")return adjacent;
    const auto history=library_root/"downloads"/adjacent.filename();
    return boost::filesystem::is_regular_file(history,ec)?history:adjacent;
}

}
