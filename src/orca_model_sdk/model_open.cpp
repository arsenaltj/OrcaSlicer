#include "model_internal.hpp"
namespace orca_model_detail {
J open_document(Context& c, const J& r)
{
    const auto op = r.at("operation").get<std::string>();
    if (op == "import" || op == "inspect" || op == "importConverted") {
        if (r.value("units", std::string()) != "mm")
            throw Error("UNITS_REQUIRED", "Normalize input to explicit mm");
        auto p     = F::u8path(r.at("inputPath").get<std::string>());
        auto bytes = read(p);
        Document d;
        d.mesh      = parse_mesh(bytes, p.extension().string(), c, op == "importConverted");
        d.id        = sha256(bytes).substr(0, 24);
        d.input_sha = sha256(bytes);
        protect_source(d, p);
        if (d.sources.at(F::canonical(p).u8string()) != d.input_sha)
            throw Error("SOURCE_IDENTITY_CHANGED", "Input changed during import");
        if (op == "importConverted") {
            auto appearance            = J::parse(read(F::u8path(r.at("appearancePath").get<std::string>())));
            const auto appearance_path = F::u8path(r.at("appearancePath").get<std::string>());
            protect_source(d, appearance_path);
            if (appearance.value("schemaVersion", 0) != 1 || appearance.value("units", std::string()) != "mm" ||
                appearance.value("upAxis", std::string()) != "Z")
                throw Error("INVALID_APPEARANCE", "Converted appearance requires schema 1, mm, Z-up");
            if (appearance.at("geometrySha256").get<std::string>() != d.input_sha)
                throw Error("GEOMETRY_HASH_MISMATCH", "Appearance does not match converted geometry bytes");
            auto colors    = appearance.at("faceColors").get<std::vector<std::array<double, 4>>>();
            auto materials = appearance.at("materialIds").get<std::vector<int>>();
            if (colors.size() != d.mesh.faces.size() || materials.size() != d.mesh.faces.size())
                throw Error("APPEARANCE_FACE_COUNT", "Converted appearance does not match triangle sequence");
            for (auto color : colors)
                for (double value : color)
                    if (!std::isfinite(value) || value < 0 || value > 1)
                        throw Error("INVALID_COLOR", "Converted RGBA must be normalized finite channels");
            for (auto id : materials)
                if (id < 0 || id > 255)
                    throw Error("INVALID_MATERIAL", "Converted material ID outside 0..255");
            d.mesh.colors    = std::move(colors);
            d.mesh.materials = std::move(materials);
            if (appearance.contains("parts")) {
                size_t next_face = 0, next_vertex = 0;
                std::set<std::string> identities;
                if (!appearance.at("parts").is_array())
                    throw Error("INVALID_PART_RANGES", "Parts must be an array");
                for (const auto& part : appearance.at("parts")) {
                    const auto first = part.at("firstTriangle").get<size_t>(), count = part.at("triangleCount").get<size_t>();
                    const auto first_vertex = part.at("firstVertex").get<size_t>(), vertices = part.at("vertexCount").get<size_t>();
                    if (first != next_face || count > d.mesh.faces.size() - next_face || first_vertex != next_vertex ||
                        vertices > d.mesh.vertices.size() - next_vertex ||
                        !identities.insert(part.at("entityId").get<std::string>()).second)
                        throw Error("INVALID_PART_RANGES", "Parts must uniquely partition converted vertices and triangles");
                    for (size_t face = first; face < first + count; ++face)
                        for (auto index : d.mesh.faces[face])
                            if (index < first_vertex || index >= first_vertex + vertices)
                                throw Error("INVALID_PART_RANGES", "Triangle references a different part");
                    next_face += count;
                    next_vertex += vertices;
                }
                if (next_face != d.mesh.faces.size() || next_vertex != d.mesh.vertices.size())
                    throw Error("INVALID_PART_RANGES", "Part coverage does not match geometry");
            }
            appearance.erase("faceColors");
            appearance.erase("materialIds");
            // Only declared file references become dependencies. Retain hashes
            // across save/load so the original and texture assets remain safe.
            std::function<void(const J&)> dependencies = [&](const J& value) {
                if (value.is_object())
                    for (auto item = value.begin(); item != value.end(); ++item) {
                        if ((item.key() == "path" || item.key() == "inputPath") && item.value().is_string()) {
                            auto dependency = F::u8path(item.value().get<std::string>());
                            if (dependency.is_relative())
                                dependency = appearance_path.parent_path() / dependency;
                            if (F::is_regular_file(dependency))
                                protect_source(d, dependency);
                        } else
                            dependencies(item.value());
                    }
                else if (value.is_array())
                    for (const auto& item : value)
                        dependencies(item);
            };
            dependencies(appearance);
            d.appearance = std::move(appearance);
        }
        if (op == "import" || op == "importConverted") {
            return adopt_document(c, std::move(d));
        }
        return preview_document(c, d);
    }
    if (op == "create") {
        Document d;
        d.mesh.vertices = r.at("vertices").get<std::vector<V>>();
        d.mesh.faces    = r.at("triangles").get<std::vector<T>>();
        valid(d.mesh);
        defaults(d.mesh);
        if (r.contains("faceColors")) {
            d.mesh.colors = r.at("faceColors").get<std::vector<std::array<double, 4>>>();
            if (d.mesh.colors.size() != d.mesh.faces.size())
                throw Error("APPEARANCE_FACE_COUNT", "Expected one color per face");
        }
        if (r.contains("materialIds")) {
            d.mesh.materials = r.at("materialIds").get<std::vector<int>>();
            if (d.mesh.materials.size() != d.mesh.faces.size())
                throw Error("APPEARANCE_FACE_COUNT", "Expected one material per face");
        }
        valid(d.mesh);
        // Transport/view options must not change the identity of identical input.
        const auto source = J{{"vertices", d.mesh.vertices},
                              {"triangles", d.mesh.faces},
                              {"faceColors", d.mesh.colors},
                              {"materialIds", d.mesh.materials}}
                                .dump();
        d.input_sha = sha256(source);
        d.id        = d.input_sha.substr(0, 24);
        return adopt_document(c, std::move(d));
    }
    if (op == "load") {
        auto id = r.at("documentId").get<std::string>();
        if (id.empty() || id.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw Error("INVALID_ID", "Invalid document identity");
        const auto saved = F::weakly_canonical(c.root / F::u8path("documents/" + id + ".json"));
        auto j           = J::parse(read(saved));
        Document d;
        d.id = j.at("documentId");
        if (d.id != id)
            throw Error("INVALID_ID", "Stored document identity mismatch");
        d.revision       = j.at("revision");
        d.geometry       = j.at("geometryRevision");
        d.input_sha      = j.at("inputSha256");
        d.mesh.vertices  = j.at("vertices").get<std::vector<V>>();
        d.mesh.faces     = j.at("triangles").get<std::vector<T>>();
        d.mesh.colors    = j.at("faceColors").get<std::vector<std::array<double, 4>>>();
        d.mesh.materials = j.at("materialIds").get<std::vector<int>>();
        d.appearance     = j.value("appearanceMetadata", J::object());
        d.sources        = j.value("protectedSources", std::map<std::string, std::string>{});
        for (const auto& source : d.sources)
            if (F::is_regular_file(F::u8path(source.first)) && sha256(read(F::u8path(source.first))) != source.second)
                throw Error("SOURCE_IDENTITY_CHANGED", "Stored source no longer matches its protected hash");
        protect_source(d, saved);
        valid(d.mesh);
        if (d.mesh.colors.size() != d.mesh.faces.size() || d.mesh.materials.size() != d.mesh.faces.size())
            throw Error("INVALID_DOCUMENT", "Invalid material arrays");
        return adopt_document(c, std::move(d));
    }
    throw Error("UNSUPPORTED_OPERATION", "Unknown model open operation");
}
} // namespace orca_model_detail
