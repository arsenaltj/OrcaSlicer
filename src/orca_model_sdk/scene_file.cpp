#include "model_internal.hpp"

namespace orca_model_detail {
namespace {
void word(std::string& bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(char(value >> (i * 8)));
}
void scalar(std::string& bytes, float value)
{
    static_assert(sizeof(float) == sizeof(uint32_t));
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    word(bytes, bits);
}
} // namespace
J scene_descriptor(Context& context, const Document& document)
{
    if (context.return_inline)
        return scene(document);
    auto snapshot        = scene(document, false, false);
    const auto& vertices = document.mesh.vertices;
    const auto& faces    = document.mesh.faces;
    const auto& colors   = document.mesh.colors;
    std::map<std::array<uint8_t, 4>, uint16_t> palette_index;
    std::vector<std::array<uint8_t, 4>> palette;
    std::vector<uint16_t> face_palette;
    for (const auto& value : colors) {
        check_cancel(context);
        std::array<uint8_t, 4> rgba{};
        for (size_t i = 0; i < 4; ++i)
            rgba[i] = uint8_t(std::lround(std::clamp(value.at(i), 0.0, 1.0) * 255));
        auto found = palette_index.find(rgba);
        if (found == palette_index.end()) {
            if (palette.size() == 65536)
                throw Error("PREVIEW_PALETTE_LIMIT", "Preview has more than 65536 distinct display colors");
            auto slot = uint16_t(palette.size());
            palette_index.emplace(rgba, slot);
            palette.push_back(rgba);
            face_palette.push_back(slot);
        } else {
            face_palette.push_back(found->second);
        }
    }
    const uint64_t length = 32 + vertices.size() * 12ull + faces.size() * 14ull + palette.size() * 4ull;
    if (length > 256ull * 1024 * 1024)
        throw Error("PREVIEW_MODEL_LIMIT", "Complete preview exceeds 256 MiB; source unchanged");
    std::string bytes("ORCASC01", 8);
    bytes.reserve(size_t(length));
    word(bytes, uint32_t(vertices.size()));
    word(bytes, uint32_t(faces.size()));
    word(bytes, uint32_t(palette.size()));
    word(bytes, 0);
    word(bytes, 0);
    word(bytes, 0);
    for (const auto& vertex : vertices) {
        check_cancel(context);
        for (const auto& coordinate : vertex)
            scalar(bytes, static_cast<float>(coordinate));
    }
    for (const auto& face : faces)
        for (const auto& index : face)
            word(bytes, index);
    for (auto slot : face_palette) {
        bytes.push_back(char(slot));
        bytes.push_back(char(slot >> 8));
    }
    for (const auto& color : palette)
        for (auto channel : color)
            bytes.push_back(char(channel));
    const auto attribute_bytes  = document.appearance.dump();
    const auto content_identity = sha256(sha256(bytes) + sha256(attribute_bytes));
    auto relative               = "scenes/" + snapshot.at("documentId").get<std::string>() + "/r" +
                    std::to_string(snapshot.at("revision").get<uint64_t>()) + "-" + content_identity.substr(0, 16) + ".sc01";
    auto filename = output(context, relative);
    std::vector<std::pair<F::path, std::string>> pending;
    const auto vertex_count = vertices.size(), face_count = faces.size();
    snapshot["scenePath"]        = filename.u8string();
    snapshot["sceneSha256"]      = sha256(bytes);
    snapshot["sceneFormat"]      = "ORCASC01";
    snapshot["sceneUsage"]       = "preview-only";
    snapshot["appearanceSha256"] = appearance_sha256(document.mesh);
    snapshot["vertexCount"]      = vertex_count;
    snapshot["triangleCount"]    = face_count;
    snapshot["sceneRanges"]      = J::array(
        {{{"firstTriangle", 0}, {"triangleCount", face_count}, {"objectIndex", 0}, {"instanceIndex", 0}, {"volumeIndex", 0}}});
    if (document.appearance.contains("parts")) {
        snapshot["sceneRanges"] = J::array();
        size_t index            = 0;
        for (const auto& part : document.appearance.at("parts")) {
            snapshot["sceneRanges"].push_back(J{{"entityId", part.at("entityId")},
                                                {"firstTriangle", part.at("firstTriangle")},
                                                {"triangleCount", part.at("triangleCount")},
                                                {"objectIndex", index++},
                                                {"instanceIndex", 0},
                                                {"volumeIndex", 0}});
        }
    }
    snapshot["artifacts"] = J::array(
        {{{"kind", "scene"}, {"path", filename.u8string()}, {"format", "sc01"}, {"sizeBytes", bytes.size()}, {"sha256", sha256(bytes)}}});
    if (!document.appearance.empty()) {
        const auto attribute_path          = output(context, relative + ".appearance-metadata.json");
        snapshot["appearanceMetadataPath"] = attribute_path.u8string();
        snapshot["artifacts"].push_back(J{{"kind", "appearanceMetadata"},
                                          {"path", attribute_path.u8string()},
                                          {"format", "json"},
                                          {"sizeBytes", attribute_bytes.size()},
                                          {"sha256", sha256(attribute_bytes)}});
        pending.emplace_back(attribute_path, attribute_bytes);
    }
    auto metadata                 = output(context, relative + ".json");
    snapshot["sceneMetadataPath"] = metadata.u8string();
    pending.emplace_back(metadata, snapshot.dump());
    pending.emplace_back(filename, std::move(bytes));
    std::vector<F::path> created;
    try {
        for (const auto& file : pending) {
            check_cancel(context);
            if (!F::exists(file.first))
                created.push_back(file.first);
            atomic_write(file.first, file.second);
        }
    } catch (...) {
        for (const auto& file : created) {
            std::error_code ignored;
            F::remove(file, ignored);
            auto temporary = file;
            temporary += ".tmp";
            F::remove(temporary, ignored);
        }
        throw;
    }
    return snapshot;
}
} // namespace orca_model_detail
