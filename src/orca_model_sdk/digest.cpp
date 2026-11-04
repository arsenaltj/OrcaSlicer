#include "model_internal.hpp"
#include "libslic3r/Sha256Digest.hpp"
namespace orca_model_detail {
std::string appearance_sha256(const Mesh& mesh)
{
    Slic3r::Sha256Digest digest;
    auto append = [&](const std::string& text) {
        if (!digest.update(text.data(), text.size()))
            throw Error("DIGEST_FAILED", "Appearance digest failed");
    };
    append("[");
    for (size_t i = 0; i < mesh.colors.size(); ++i) {
        if (i)
            append(",");
        append(J(mesh.colors[i]).dump());
    }
    append("][");
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        if (i)
            append(",");
        append(J(mesh.materials[i]).dump());
    }
    append("]");
    auto result = digest.final_hex();
    if (result.empty())
        throw Error("DIGEST_FAILED", "Appearance digest finalization failed");
    return result;
}
std::string sha256(const std::string& bytes)
{
    // Reuse the existing SDK portable streaming backend on both SDK targets.
    Slic3r::Sha256Digest digest;
    if (!digest.update(bytes.data(), bytes.size()))
        throw Error("DIGEST_FAILED", "Input digest could not be calculated");
    auto result = digest.final_hex();
    if (result.empty())
        throw Error("DIGEST_FAILED", "Input digest could not be finalized");
    return result;
}
} // namespace orca_model_detail
