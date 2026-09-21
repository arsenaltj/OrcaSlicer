#include "IColorMixSolver.hpp"
#include "ColorSolver.hpp"
#include "TextureMapping.hpp"
#include <nlohmann/json.hpp>
#include <png.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
namespace Probe = Orca::ImageMapProbe;
using Json = nlohmann::json;
using RGB = Probe::RGB;
float difference(const RGB& a, const RGB& b)
{
    const auto x = Slic3r::color_solver_oklab_from_srgb(a), y = Slic3r::color_solver_oklab_from_srgb(b);
    float result = 0;
    for (int c = 0; c < 3; ++c) result += (x[c]-y[c])*(x[c]-y[c]);
    return std::sqrt(result);
}
size_t peak_memory()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters {};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.PeakWorkingSetSize : 0;
#else
    return 0;
#endif
}
Json result_json(const Probe::MixResult& result)
{
    Json weights = Json::array();
    for (const auto& item : result.weights) weights.push_back({{"slot_id", item.slot_id}, {"weight", item.weight}});
    return {{"ok", result.ok}, {"error", result.error}, {"weights", weights},
        {"predicted_rgb", result.predicted_rgb}, {"oklab_error", result.oklab_error},
        {"solver_identity", result.solver_identity}, {"candidate_count", result.candidate_count},
        {"candidate_build_ms", result.candidate_build_ms}, {"solve_ms", result.solve_ms}, {"cache_hit", result.cache_hit}};
}
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct UvFace { std::array<std::array<float, 2>, 3> corners; size_t image_index; };
std::array<uint8_t, 4> sample(const UvFace& face, const std::array<float, 3>& barycentric,
                            const std::vector<Slic3r::TextureMappingPrimeTowerImage>& images)
{
    const auto& image = images.at(face.image_index);
    require(image.valid(), "Invalid upstream RGBA image fields");
    std::array<float, 2> uv {};
    for (int corner = 0; corner < 3; ++corner) for (int axis = 0; axis < 2; ++axis)
        uv[axis] += barycentric[corner] * face.corners[corner][axis];
    const auto x = std::min(image.width - 1, unsigned(std::clamp(uv[0], 0.f, 1.f) * image.width));
    const auto y = std::min(image.height - 1, unsigned(std::clamp(uv[1], 0.f, 1.f) * image.height));
    const auto offset = (size_t(y) * image.width + x) * 4;
    return {image.rgba[offset], image.rgba[offset+1], image.rgba[offset+2], image.rgba[offset+3]};
}
Json uv_probe()
{
    // Two triangles share a geometric vertex but each keeps its own UV corner.
    // The image structure is the real upstream type. Sampling is this adapter's
    // contract fixture, not execution of ImageMap's slice or UV loader.
    Slic3r::TextureMappingPrimeTowerImage first;
    first.width = first.height = 2;
    first.image_name = "uv-seam-fixture";
    first.rgba = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,0};
    auto second = first;
    second.image_name = "second-material";
    second.rgba[0] = 20; second.rgba[1] = 40; second.rgba[2] = 60;
    const std::vector<Slic3r::TextureMappingPrimeTowerImage> images {first, second};
    const UvFace a {{{{0,0},{1,0},{0,1}}}, 0};
    const UvFace b {{{{0,1},{1,0},{1,1}}}, 0};
    auto c = a; c.image_index = 1;
    const auto left = sample(a, {1,0,0}, images), right = sample(b, {1,0,0}, images);
    require(left != right, "A shared position incorrectly merged its two UV corners");
    require(sample(b, {0,0,1}, images)[3] == 0, "Transparent texel was not preserved");
    require(sample(c, {1,0,0}, images) != left, "Per-face image/material selection was lost");
    return {{"status", "passed"}, {"uv_seam_preserved", true}, {"multiple_images_preserved", true},
        {"transparent_texel_keeps_zero_alpha", true}, {"upstream_image_fields_valid", first.valid()},
        {"scope", "Face-corner UV to actual upstream RGBA field contract only; no mesh atlas generation or slicing path."}};
}
void swatches(const std::filesystem::path& path, const std::vector<std::array<RGB, 3>>& rows)
{
    if (rows.empty()) return;
    constexpr unsigned cell = 48;
    png_image image {};
    image.version = PNG_IMAGE_VERSION;
    image.width = cell * 3;
    image.height = unsigned(rows.size()) * cell;
    image.format = PNG_FORMAT_RGB;
    std::vector<uint8_t> bytes(size_t(image.width) * image.height * 3);
    for (size_t row = 0; row < rows.size(); ++row) for (unsigned column = 0; column < 3; ++column)
        for (unsigned y = 0; y < cell; ++y) for (unsigned x = 0; x < cell; ++x) for (unsigned channel = 0; channel < 3; ++channel)
            bytes[((row * cell + y)*image.width + column*cell+x)*3+channel] =
                uint8_t(std::lround(std::clamp(rows[row][column][channel], 0.f, 1.f) * 255.f));
    if (!png_image_write_to_file(&image, path.string().c_str(), 0, bytes.data(), 0, nullptr))
        throw std::runtime_error(image.message);
}
int main(int argc, char** argv)
{
    try {
        require(argc >= 2, "Usage: imagemap_probe OUTPUT_DIR [targets.json] [palette.json]");
        const std::filesystem::path output = argv[1];
        std::filesystem::create_directories(output);
        std::vector<Probe::Filament> full {
            {"skin", {.88f,.71f,.61f}}, {"dark", {.10f,.10f,.10f}}, {"white", {.96f,.96f,.96f}},
            {"lips", {.72f,.24f,.29f}}, {"cool", {.30f,.48f,.46f}}, {"gray", {.43f,.42f,.40f}}};
        std::vector<std::pair<std::string, RGB>> targets {{"skin", {.76f,.55f,.41f}}, {"lips", {.68f,.26f,.24f}},
            {"brown-hair", {.27f,.17f,.12f}}, {"gray-cloth", {.48f,.49f,.47f}}, {"sclera", {.89f,.90f,.87f}}};
        if (argc > 2) {
            std::ifstream input(argv[2]);
            const auto source = Json::parse(input);
            targets.clear();
            for (const auto& item : source) targets.push_back({item.at("id").get<std::string>(), item.at("rgb").get<RGB>()});
            require(!targets.empty(), "The supplied pre-quantization material-center list is empty");
        }
        if (argc > 3) {
            std::ifstream input(argv[3]);
            const auto source = Json::parse(input);
            full.clear();
            for (const auto& item : source) full.push_back({item.at("id").get<std::string>(), item.at("rgb").get<RGB>()});
            require(full.size() >= 2 && full.size() <= 6, "Batch probe palette must contain two to six slots");
        }
        auto solver = Probe::make_imagemap_solver();
        Json report {{"schema", "orca.imagemap-probe/v1"}, {"upstream_commit", "92548381056dbf72836b0a1bdc455f238218dbfb"},
            {"prediction_only", true}, {"printing_recipe", false}, {"calibrated", false}, {"cases", Json::array()}};
        std::vector<std::array<RGB, 3>> pictures;
        report["target_source"] = argc > 2 ? argv[2] : "synthetic-contract-fixtures";
        report["palette_source"] = argc > 3 ? argv[3] : "synthetic-contract-fixtures";
        report["palette"] = Json::array();
        for (const auto& slot : full) report["palette"].push_back({{"id", slot.slot_id}, {"rgb", slot.rgb}});
        for (size_t count = 1; count <= full.size(); ++count) {
            const std::vector<Probe::Filament> palette(full.begin(), full.begin() + count);
            for (const auto& target : targets) {
                const auto solved = solver->solve(target.second, palette);
                require(solved.ok, solved.error.c_str());
                auto result = result_json(solved);
                size_t nearest = 0;
                float error = std::numeric_limits<float>::infinity();
                for (size_t i = 0; i < palette.size(); ++i) {
                    const float distance = difference(target.second, palette[i].rgb);
                    if (distance < error) { error = distance; nearest = i; }
                }
                result["colors"] = count;
                result["target_id"] = target.first;
                result["target_rgb"] = target.second;
                result["nearest_single_slot"] = palette[nearest].slot_id;
                result["nearest_single_rgb"] = palette[nearest].rgb;
                result["nearest_single_oklab_error"] = error;
                result["predicted_improvement"] = error - solved.oklab_error;
                report["cases"].push_back(result);
                if (count == full.size()) pictures.push_back({target.second, palette[nearest].rgb, solved.predicted_rgb});
            }
        }
        auto duplicated = full;
        duplicated[1].rgb = duplicated[0].rgb;
        const auto duplicate = solver->solve(targets.front().second, duplicated);
        require(duplicate.ok, "Duplicate-RGB palette failed");
        require(duplicate.weights.size() == duplicated.size(), "Duplicate RGB discarded a stable slot");
        for (size_t i = 0; i < duplicated.size(); ++i)
            require(duplicate.weights[i].slot_id == duplicated[i].slot_id, "Duplicate RGB changed slot identity");
        report["duplicate_rgb"] = result_json(duplicate);
        auto same = solver->solve(targets.front().second, duplicated);
        require(same.cache_hit, "Repeated target/palette did not reuse candidates");
        duplicated[1].slot_id = "renamed-slot";
        const auto renamed = solver->solve(targets.front().second, duplicated);
        require(!renamed.cache_hit, "Stable slot identity was absent from the candidate cache key");
        report["slot_identity_cache_test"] = "passed";
        auto invalid = duplicated; invalid[1].slot_id = invalid[0].slot_id;
        require(!solver->solve(targets.front().second, invalid).ok, "Duplicate slot ID was accepted");
        require(!solver->solve(targets.front().second, {}).ok, "Empty palette was accepted");
        auto too_many = full;
        while (too_many.size() <= 6) too_many.push_back({"extra-" + std::to_string(too_many.size()), {0,0,0}});
        require(!solver->solve(targets.front().second, too_many).ok, "More than six slots were accepted");
        report["invalid_input_tests"] = "passed";
        report["uv_contract"] = uv_probe();
        report["peak_process_bytes"] = peak_memory();
        report["status"] = "interface-probe-passed-not-print-validation";
        report["swatch_columns"] = {"pre-quantization target", "nearest single filament", "uncalibrated predicted mixture"};
        std::ofstream json(output / "result.json"); json << report.dump(2);
        require(bool(json), "Unable to write result JSON");
        swatches(output / "swatches.png", pictures);
        std::cout << "Passed " << report["cases"].size() << " target/palette cases, slot identity and UV contract probes.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
