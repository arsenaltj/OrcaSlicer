#pragma once
#include <wx/string.h>
#include <vector>
#include <string>

namespace Slic3r::GUI {
enum class ModelLibraryCategory { All, Design, Model, Finishing };
enum class ModelLibraryAssetKind { Design, Model, LocalImport, Finishing };

// Local import copies use a reserved finish-import- namespace in existing
// history. Distinguish it before finish- without rewriting persisted IDs.
inline ModelLibraryAssetKind model_library_asset_kind(bool design_only, const std::string& local_id)
{
    if (design_only) return ModelLibraryAssetKind::Design;
    if (local_id.rfind("finish-import-", 0) == 0) return ModelLibraryAssetKind::LocalImport;
    if (local_id.rfind("finish-", 0) == 0) return ModelLibraryAssetKind::Finishing;
    return ModelLibraryAssetKind::Model;
}

// A projection of the existing snapshot: source indices keep action/thumbnail
// identity together without copying or altering persisted history records.
template<class Entries>
std::vector<size_t> filter_model_library(const Entries& entries, ModelLibraryCategory category, wxString query, bool images_only = false)
{
    query.Trim(true).Trim(false).MakeLower();
    const bool image_view = images_only || category == ModelLibraryCategory::Design;
    std::vector<size_t> result;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const auto kind = model_library_asset_kind(entry.design_only, entry.job_id);
        const bool finishing = kind == ModelLibraryAssetKind::Finishing;
        // Imported copies may refer to an original task image; keep that image
        // on its original card rather than adding a duplicate to image views.
        if (image_view && !entry.design_only &&
            (finishing || kind == ModelLibraryAssetKind::LocalImport || entry.ai_image_path.empty())) continue;
        if (!image_view && ((category == ModelLibraryCategory::Model && (entry.design_only || finishing)) ||
            (category == ModelLibraryCategory::Finishing && !finishing))) continue;
        const wxString haystack = (entry.search_text + "\n" + entry.title + "\n" +
            wxString::FromUTF8(entry.job_id) + "\n" + wxString::FromUTF8(entry.provider_task_id) +
            "\n" + wxString::FromUTF8(entry.provider_conversion_task_id)).Lower();
        if (query.empty() || haystack.Contains(query)) result.push_back(i);
    }
    return result;
}
} // namespace Slic3r::GUI
