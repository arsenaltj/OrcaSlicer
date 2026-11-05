#include "model_internal.hpp"
namespace orca_model_detail {
J scene(const Document& d, bool include_mesh, bool include_appearance)
{
    if (d.id.empty())
        throw Error("NO_DOCUMENT", "Open or create a model first");
    V low = d.mesh.vertices.front(), high = low;
    for (auto& v : d.mesh.vertices)
        for (int i = 0; i < 3; ++i) {
            low[i]  = std::min(low[i], v[i]);
            high[i] = std::max(high[i], v[i]);
        }
    J snapshot{{"documentId", d.id},
               {"modelVersion", d.id + ":" + std::to_string(d.revision)},
               {"revision", d.revision},
               {"geometryRevision", d.geometry},
               {"inputSha256", d.input_sha},
               {"units", "mm"},
               {"upAxis", "Z"},
               {"bounds", {{"min", low}, {"max", high}}}};
    if (include_appearance)
        snapshot["appearanceMetadata"] = d.appearance;
    else {
        snapshot["appearanceMetadata"] = {{"partCount", d.appearance.contains("parts") ? d.appearance.at("parts").size() : 0},
                                          {"warnings", d.appearance.value("warnings", J::array())},
                                          {"materialRoutingVerified", false}};
    }
    if (include_mesh) {
        snapshot["protectedSources"] = d.sources;
        snapshot["vertices"]         = d.mesh.vertices;
        snapshot["triangles"]        = d.mesh.faces;
        snapshot["faceColors"]       = d.mesh.colors;
        snapshot["materialIds"]      = d.mesh.materials;
    }
    return snapshot;
}
void expected(Context& c, const J& r)
{
    if (c.doc.id.empty())
        throw Error("NO_DOCUMENT", "No editable document");
    if (!r.contains("expectedRevision") || r.at("expectedRevision").get<uint64_t>() != c.doc.revision)
        throw Error("STALE_REVISION", "Supply the current document revision");
}
void clear_candidate(Context& c)
{
    c.candidate            = Document{};
    c.candidate_base       = 0;
    c.candidate_generation = 0;
    c.candidate_geometry   = 0;
    c.candidate_id.clear();
    c.candidate_document.clear();
}
J preview_document(Context& c, const Document& next)
{
    valid(next.mesh);
    Context staged;
    staged.root              = c.root;
    staged.return_inline     = c.return_inline;
    staged.protected_sources = c.protected_sources;
    for (const auto& source : next.sources)
        staged.protected_sources[source.first] = source.second;
    auto descriptor = scene_descriptor(staged, next);
    check_cancel(c);
    return descriptor;
}
J adopt_document(Context& c, Document next)
{
    auto descriptor        = preview_document(c, next);
    auto protected_sources = c.protected_sources;
    for (const auto& source : next.sources)
        protected_sources[source.first] = source.second;
    c.doc = std::move(next);
    c.protected_sources.swap(protected_sources);
    c.undo.clear();
    c.redo.clear();
    clear_candidate(c);
    ++c.generation;
    return descriptor;
}
J commit(Context& c, Document next)
{
    valid(next.mesh);
    next.revision   = c.doc.revision + 1;
    auto descriptor = scene_descriptor(c, next);
    auto undo       = c.undo;
    undo.push_back(c.doc);
    if (undo.size() > 8)
        undo.erase(undo.begin());
    check_cancel(c);
    c.undo.swap(undo);
    c.redo.clear();
    c.doc = std::move(next);
    clear_candidate(c);
    return descriptor;
}
} // namespace orca_model_detail
