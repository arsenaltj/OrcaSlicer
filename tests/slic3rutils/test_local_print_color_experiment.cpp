#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/ModelPreview3D.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorQuality.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorBoundaryRefinement.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintColorState.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/convert.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
boost::filesystem::path experiment_path(const std::string& utf8)
{
#ifdef _WIN32
    return boost::filesystem::path(boost::nowide::widen(utf8));
#else
    return boost::filesystem::path(utf8);
#endif
}

void write_experiment_json(const boost::filesystem::path& path, const nlohmann::json& value)
{
    REQUIRE_FALSE(boost::filesystem::exists(path));
    boost::filesystem::ofstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    stream << value.dump(2);
    stream.close();
    REQUIRE_FALSE(stream.fail());
}

void write_experiment_samples(const boost::filesystem::path& path,
                             const std::vector<Slic3r::GUI::LocalPrintColorMatching::FaceSample>& samples)
{
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
                  "Experiment samples require IEEE-754 binary64");
    REQUIRE_FALSE(boost::filesystem::exists(path));
    boost::filesystem::ofstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    stream.write("ORCAFS01", 8);
    auto write_u64 = [&](uint64_t value) {
        std::array<char, 8> bytes {};
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = char((value >> (8 * i)) & 255);
        stream.write(bytes.data(), bytes.size());
    };
    write_u64(uint64_t(samples.size()));
    for (const auto& face : samples) {
        // Widen the actual float32 RGB means exactly; never quantize back to RGB8.
        for (double value : {double(face.color[0]), double(face.color[1]), double(face.color[2]), face.area}) {
            uint64_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            write_u64(bits);
        }
    }
    stream.close();
    REQUIRE_FALSE(stream.fail());
    REQUIRE(boost::filesystem::file_size(path) == 16ULL + 32ULL * samples.size());
}
} // namespace

// Opt-in measurement of an existing local artifact, never a provider benchmark.
// Material variants are hypothetical unless independently verified as installed.
TEST_CASE("local color experiments retain exact source identity and report every candidate",
          "[LocalPrintColorExperiment][.]")
{
    using namespace Slic3r;
    namespace Matching = GUI::LocalPrintColorMatching;
    namespace fs = boost::filesystem;
    const char* specification = boost::nowide::getenv("ORCASLICER_COLOR_EVAL_SPEC");
    if (!specification || !*specification) SKIP("Set ORCASLICER_COLOR_EVAL_SPEC to a local experiment JSON.");
    const auto spec_path = fs::canonical(experiment_path(specification));
    REQUIRE(fs::is_regular_file(spec_path));
    REQUIRE(fs::file_size(spec_path) < 32ULL * 1024 * 1024);
    const std::string spec_hash = AI::model_artifact_sha256(spec_path);
    fs::ifstream spec_stream(spec_path);
    REQUIRE(spec_stream.is_open());
    const auto spec = nlohmann::json::parse(spec_stream);
    REQUIRE(spec.is_object());
    REQUIRE(spec.at("variants").is_array());
    REQUIRE_FALSE(spec.at("variants").empty());
    const auto requested_source = experiment_path(spec.at("source_path").get<std::string>());
    const auto output = experiment_path(spec.at("output_directory").get<std::string>());
    REQUIRE(requested_source.is_absolute());
    REQUIRE(output.is_absolute());
    REQUIRE_FALSE(fs::exists(output));
    const auto source = fs::canonical(requested_source);
    REQUIRE(fs::is_regular_file(source));
    const auto hash = AI::model_artifact_sha256(source);
    REQUIRE(AI::is_lowercase_sha256(hash));
    REQUIRE(hash == spec.at("source_sha256").get<std::string>());
    GUI::ModelPreview3D::PreparedModel prepared;
    std::string error;
    INFO("Native model preparation: " << source.string());
    REQUIRE(GUI::ModelPreview3D::prepare_model(source, prepared, error, {}, {}, false));
    REQUIRE(AI::model_artifact_sha256(source) == hash);
    REQUIRE_FALSE(prepared.geometry_id.empty());
    REQUIRE(prepared.triangles > 0);
    REQUIRE(prepared.triangles <= size_t(std::numeric_limits<int>::max()) / 3);
    REQUIRE(prepared.mesh.indices.size() == prepared.triangles);
    REQUIRE(prepared.geometry.format.vertex_layout == GUI::GLModel::Geometry::EVertexLayout::P3N3T2);
    REQUIRE(prepared.geometry.vertices.size() == prepared.triangles * 24);
    Matching::Input input;
    input.identity.source_sha256 = hash;
    input.identity.geometry_id = prepared.geometry_id;
    input.identity.face_count = prepared.triangles;
    input.identity.source_color_count = prepared.colors;
    const auto& geometry = prepared.geometry.vertices;
    indexed_triangle_set surface;
    surface.vertices.reserve(prepared.triangles * 3);
    surface.indices.reserve(prepared.triangles);
    input.faces.reserve(prepared.triangles);
    // Match LocalPrintColorPanel's packed RGB8 corner mean and float32 area,
    // including its expanded face-corner identity check. This is not UV sampling.
    for (size_t face = 0; face < prepared.triangles; ++face) {
        AI::PrintRgb mean {};
        for (size_t c = 0; c < 3; ++c) {
            const size_t offset = (face * 3 + c) * 8;
            surface.vertices.emplace_back(geometry[offset], geometry[offset+1], geometry[offset+2]);
            const auto packed = uint32_t(geometry[offset+6]);
            const RGBA rgb {float((packed >> 16) & 255)/255.f, float((packed >> 8) & 255)/255.f,
                            float(packed & 255)/255.f, 1.f};
            for (size_t channel = 0; channel < 3; ++channel)
                mean[channel] += rgb[channel] / 3.f;
        }
        surface.indices.emplace_back(int(face * 3), int(face * 3 + 1), int(face * 3 + 2));
        const auto& a = surface.vertices[face * 3];
        const auto& b = surface.vertices[face * 3 + 1];
        const auto& c = surface.vertices[face * 3 + 2];
        input.faces.push_back({mean, double((b-a).cross(c-a).norm()) / 2});
    }
    REQUIRE(AI::SurfaceSelectionPersistence::geometry_fingerprint(surface) == prepared.geometry_id);
    REQUIRE(AI::SurfaceSelectionPersistence::geometry_fingerprint(prepared.mesh) == prepared.geometry_id);
    nlohmann::json regions = nlohmann::json::array();
    fs::path regions_path;
    std::string regions_hash;
    if (spec.contains("regions_path")) {
        regions_path = experiment_path(spec.at("regions_path").get<std::string>());
        REQUIRE(regions_path.is_absolute());
        regions_path = fs::canonical(regions_path);
        REQUIRE(fs::is_regular_file(regions_path));
        REQUIRE(fs::file_size(regions_path) < 32ULL * 1024 * 1024);
        regions_hash = AI::model_artifact_sha256(regions_path);
        fs::ifstream stream(regions_path);
        REQUIRE(stream.is_open());
        const auto evidence = nlohmann::json::parse(stream);
        REQUIRE(evidence.at("source_sha256") == hash);
        REQUIRE(evidence.at("native_geometry_id") == prepared.geometry_id);
        REQUIRE(evidence.at("face_count") == prepared.triangles);
        regions = evidence.at("regions");
        REQUIRE(regions.is_array());
    }
    // Acquiring a new directory must succeed; never reuse a concurrently created run.
    REQUIRE(fs::create_directories(output));
    write_experiment_json(output/"experiment-spec.json", spec);
    nlohmann::json sample_export = nullptr;
    if (spec.value("export_samples", false)) {
        const auto path = output/"native-face-samples.bin";
        write_experiment_samples(path, input.faces);
        const auto samples_hash = AI::model_artifact_sha256(path);
        REQUIRE(AI::is_lowercase_sha256(samples_hash));
        sample_export = {{"file","native-face-samples.bin"},{"sha256",samples_hash},
            {"source_sha256",hash},{"native_geometry_id",prepared.geometry_id},{"face_count",prepared.triangles},
            {"format","ORCAFS01"},{"header_bytes",16},{"record_bytes",32},{"byte_order","little-endian"},
            {"header","8-byte ASCII magic, then uint64 face count"},
            {"record","IEEE754 float64 r,g,b,area in native face order; no face ID column"},
            {"rgb_units","normalized nonlinear sRGB; float32 mean of three packed RGB8 corners widened exactly"},
            {"area_units","mm^2; same float32 cross-product norm divided by 2 as LocalPrintColorPanel"},
            {"additional_export_loss","none; no RGB8 requantization"}};
    }
    nlohmann::json reports = nlohmann::json::array();
    for (const auto& variant : spec.at("variants")) {
        input.identity.requested_color_count = variant.value("n", size_t(6));
        input.identity.physical_channels.clear();
        input.identity.regions.clear();
        const auto palette = variant.at("palette").get<std::vector<std::string>>();
        for (size_t i=0; i<palette.size(); ++i)
            input.identity.physical_channels.push_back({i,palette[i],"PLA",true});
        input.identity.material_fingerprint = "experiment-palette:" + variant.at("palette").dump();
        input.identity.process_fingerprint = "offline-direct-color-experiment-no-physical-calibration";
        input.tolerance = variant.value("tolerance", 5.0);
        input.important_area_floor = variant.value("important_area_floor", 0.02);
        for (const auto& region : regions) {
            AI::PrintColorRegion r;
            r.id = region.at("id"); r.subject_id = region.at("subject_id"); r.label = region.at("label");
            r.confidence = region.at("confidence"); r.faces = region.at("faces").get<std::vector<size_t>>();
            const auto protected_labels = variant.value("protected_labels", std::vector<std::string>{});
            r.protect_color = std::find(protected_labels.begin(),protected_labels.end(),r.label) != protected_labels.end();
            input.identity.regions.push_back(std::move(r));
        }
        const auto start = std::chrono::steady_clock::now();
        auto computed = variant.value("refine_boundaries", false) ?
            GUI::LocalPrintColorBoundaryRefinement::compute_guarded(input, prepared.mesh) : Matching::compute(input);
        nlohmann::json report {{"variant",variant},{"ok",computed.ok()},{"error",computed.error},
            {"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()}};
        if (computed.ok() && variant.value("export_stages", false)) {
            // Replay the native stages with identical frozen input. Verify their
            // reconstructed result against the actual public entry point above;
            // this export is opt-in experiment evidence, not a product code path.
            namespace Boundary = GUI::LocalPrintColorBoundaryRefinement;
            REQUIRE(variant.value("refine_boundaries", false));
            REQUIRE(input.identity.user_overrides.empty());
            REQUIRE(input.identity.contrasts.empty());
            REQUIRE(std::none_of(input.identity.regions.begin(),input.identity.regions.end(),
                [](const auto& r) { return bool(r.locked_physical_slot); }));
            auto ordinary_input=input;
            bool has_preference=false;
            for(auto& r:ordinary_input.identity.regions) {
                if(r.user_protected) continue;
                has_preference |= r.protect_color && r.confidence>=.5 && !r.faces.empty();
                r.protect_color=false;
            }
            report["stages"]=nlohmann::json::array();
            auto record_stage=[&](const char* name,const Matching::Computation& stage) {
                INFO(name); REQUIRE(stage.ok());
                REQUIRE(stage.result.face_targets.size()==prepared.triangles);
                const std::string filename="stage-"+std::to_string(reports.size())+"-"+name+".json";
                write_experiment_json(output/filename,GUI::LocalPrintColorState::encode(stage.result));
                report["stages"].push_back({{"name",name},{"result_file",filename},
                    {"sha256",AI::model_artifact_sha256(output/filename)}});
            };
            auto semantic=Matching::compute(input);
            auto ordinary=Matching::compute(ordinary_input);
            record_stage("semantic-matching",semantic);record_stage("ordinary-matching",ordinary);
            semantic=Boundary::compute_refined(input,prepared.mesh,std::move(semantic),false);
            ordinary=Boundary::compute_refined(ordinary_input,prepared.mesh,std::move(ordinary),false);
            ordinary.result.regions=input.identity.regions;
            record_stage("semantic-local",semantic);record_stage("ordinary-local",ordinary);
            const auto sq=GUI::LocalPrintColorQuality::evaluate(input.faces,semantic.result);
            const auto oq=GUI::LocalPrintColorQuality::evaluate(input.faces,ordinary.result);
            REQUIRE(sq.error.empty());REQUIRE(oq.error.empty());
            const bool choose_semantic=!has_preference ||
                (Boundary::preserves_quality(sq,oq,input.identity.regions) &&
                 !Boundary::preserves_quality(oq,sq,input.identity.regions));
            auto selected=choose_semantic ? std::move(semantic) : std::move(ordinary);
            record_stage("selected-local",selected);
            report["selected_stage"]=choose_semantic ? "semantic-local" : "ordinary-local";
            if(selected.result.mode==AI::PrintColorMode::Layered)
                selected=Boundary::compute_refined(input,prepared.mesh,std::move(selected),true,false);
            record_stage("patches",selected);
            REQUIRE(selected.result.face_targets==computed.result.face_targets);
            REQUIRE(selected.result.targets.size()==computed.result.targets.size());
            for(size_t t=0;t<selected.result.targets.size();++t) {
                REQUIRE(selected.result.targets[t].physical_slot==computed.result.targets[t].physical_slot);
                REQUIRE(selected.result.targets[t].output==computed.result.targets[t].output);
            }
            report["stage_replay_matches_public_result"]=true;
        }
        if (computed.ok()) {
            REQUIRE(computed.result.face_targets.size() == prepared.triangles);
            REQUIRE(computed.result.targets.size() <= input.identity.requested_color_count);
            const auto quality = GUI::LocalPrintColorQuality::evaluate(input.faces,computed.result);
            REQUIRE(quality.error.empty());
            report["quality"] = {{"has_covered_samples",quality.has_covered_samples},{"mean_delta_e",quality.mean_delta_e},
                {"p95_delta_e",quality.p95_delta_e},{"max_delta_e",quality.worst_delta_e},
                {"uncovered_area_fraction",quality.unresolved_area_fraction},{"over_tolerance_area_fraction",quality.over_tolerance_area_fraction}};
            report["regions"] = nlohmann::json::array();
            for(const auto& q : quality.regions) report["regions"].push_back({{"id",q.id},{"label",q.label},
                {"area",q.area},{"mean_delta_e",q.mean_delta_e},{"max_delta_e",q.worst_delta_e},{"unresolved_area",q.unresolved_area}});
            const auto name = "candidate-" + std::to_string(reports.size()) + ".json";
            const auto persisted = GUI::LocalPrintColorState::encode(computed.result);
            AI::LocalPrintColorResult restored_result;
            std::string restore_error;
            REQUIRE(GUI::LocalPrintColorState::decode(persisted, hash, prepared.geometry_id,
                input.identity.material_fingerprint, input.identity.process_fingerprint,
                restored_result, restore_error, computed.result.algorithm_version));
            REQUIRE(restored_result.face_targets == computed.result.face_targets);
            REQUIRE(restored_result.algorithm_version == computed.result.algorithm_version);
            write_experiment_json(output/name, persisted);
            report["result_file"] = name;
        }
        reports.push_back(std::move(report));
    }
    REQUIRE(AI::model_artifact_sha256(source) == hash);
    REQUIRE(AI::model_artifact_sha256(spec_path) == spec_hash);
    if (!regions_path.empty()) REQUIRE(AI::model_artifact_sha256(regions_path) == regions_hash);
    // summary.json is written last; its presence marks a fully written experiment.
    write_experiment_json(output/"summary.json", {{"source_sha256",hash},{"native_geometry_id",prepared.geometry_id},
        {"spec_sha256",spec_hash},{"regions_sha256",regions_hash},{"completed",true},
        {"sample_export",sample_export},
        {"sample_extraction","LocalPrintColorPanel packed RGB8 corner mean; float32 native triangle area; no UV resampling"},
        {"face_count",prepared.triangles},{"material_scope","explicit hypothetical test palettes; no printer settings changed"},
        {"reports",reports}});
}
