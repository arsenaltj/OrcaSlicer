#include "ModelSemanticColoring.hpp"
#include "PortraitContourProposal.hpp"
#include "ModelColorPreviewShader.hpp"
#include "ModelPreviewNormals.hpp"
#include "LocalSemanticWorkerClient.hpp"
#include "PortraitColorPlanBuild.hpp"
#include "PortraitParentCleanup.hpp"
#include "PortraitParentCleanupLive.hpp"
#include "PortraitParentCoverage.hpp"
#include "PortraitPaletteRoles.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <numeric>
#include <thread>
#include <boost/dll/runtime_symbol_info.hpp>

namespace Slic3r::GUI {
namespace SC = AI::SemanticColoring;
namespace {
bool red_preview_color(const SC::Color& color)
{
    SC::Color linear = color;
    for (float& channel : linear)
        channel = channel <= .04045f ? channel / 12.92f : std::pow((channel + .055f) / 1.055f, 2.4f);
    const float l = std::cbrt(.4122214708f*linear[0] + .5363325363f*linear[1] + .0514459929f*linear[2]);
    const float m = std::cbrt(.2119034982f*linear[0] + .6806995451f*linear[1] + .1073969566f*linear[2]);
    const float s = std::cbrt(.0883024619f*linear[0] + .2817188376f*linear[1] + .6299787005f*linear[2]);
    const float a = 1.9779984951f*l - 2.428592205f*m + .4505937099f*s;
    const float b = .0259040371f*l + .7827717662f*m - .808675766f*s;
    return std::hypot(a, b) >= .055f && a >= .045f && a > b * 1.25f + .01f;
}
bool neutral_preview_color(const SC::Color& color)
{
    const float maximum = std::max({color[0], color[1], color[2]});
    const float minimum = std::min({color[0], color[1], color[2]});
    return maximum - minimum < .10f &&
        (color[0] + color[1] + color[2]) / 3.f >= .42f;
}
// Freeze provider selection with the request, so edits to the configuration
// cannot be read by a stale worker after the user has selected another model.
std::string provider_configuration(const std::filesystem::path& runtime)
{
    const auto file = runtime / "providers.json";
    std::error_code error;
    const auto bytes = std::filesystem::file_size(file, error);
    if (error == std::errc::no_such_file_or_directory) return "{}";
    if (error || bytes > 65536) return "invalid local provider configuration";
    std::ifstream input(file, std::ios::binary);
    if (!input) return "unreadable local provider configuration";
    return std::string(std::istreambuf_iterator<char>(input), {});
}

}

bool decode_semantic_result(const nlohmann::json& value,const std::string& geometry,size_t count,
    SC::FaceColors& faces,SC::SubfaceColors& children)
{
    faces.clear(); children.clear();
    try {
        if (!value.is_object() || value.at("schema")!="orca.semantic-result/v1" ||
            value.at("geometry_id")!=geometry || value.at("face_count")!=count ||
            !value.at("faces").is_array() || value.at("faces").size()>count ||
            !value.at("subfaces").is_array() || value.at("subfaces").size()>2000000)
            throw std::invalid_argument("Invalid saved semantic identity.");
        const auto integer=[](const nlohmann::json& field)->uint64_t {
            if (!field.is_number_integer() || field.get<int64_t>()<0)
                throw std::invalid_argument("Invalid saved semantic integer.");
            return field.get<uint64_t>();
        };
        const auto rgb=[](const nlohmann::json& field)->SC::Color {
            if (!field.is_array() || field.size()!=3) throw std::invalid_argument("Invalid saved semantic color.");
            for (const auto& c:field) if (!c.is_number() || !std::isfinite(c.get<double>()) || c.get<double>()<0 || c.get<double>()>1)
                throw std::invalid_argument("Invalid saved semantic channel.");
            return field.get<SC::Color>();
        };
        for (const auto& item:value.at("faces")) {
            if (!item.is_array() || item.size()!=2) throw std::invalid_argument("Invalid saved semantic face.");
            const auto face=integer(item[0]);
            if (face>=count || (!faces.empty() && face<=faces.back().first))
                throw std::invalid_argument("Unordered saved semantic face.");
            faces.push_back({size_t(face),rgb(item[1])});
        }
        for (const auto& item:value.at("subfaces")) {
            if (!item.is_array() || item.size()!=4) throw std::invalid_argument("Invalid saved semantic child.");
            const auto face=integer(item[0]),depth=integer(item[1]),path=integer(item[2]);
            if (face>=count || !depth || depth>4 || path>=(1ull<<(2*depth)))
                throw std::invalid_argument("Saved semantic child is out of range.");
            SC::SubfaceColor child{size_t(face),{uint8_t(depth),uint8_t(path)},rgb(item[3]),1.f};
            if (!children.empty() && !(std::tie(children.back().face_id,children.back().path)<std::tie(child.face_id,child.path)))
                throw std::invalid_argument("Unordered saved semantic child.");
            children.push_back(child);
        }
        return true;
    } catch (const std::exception&) { faces.clear(); children.clear(); return false; }
}

std::filesystem::path semantic_region_runtime_directory()
{
    boost::system::error_code error;
    const auto executable = boost::dll::program_location(error);
    return error ? std::filesystem::path {} : std::filesystem::path(executable.native()).parent_path() / "ai" / "portrait_semantics";
}

std::string portrait_shape_runtime_fingerprint()
{
    LocalSemanticWorker::Configuration config;
    std::string reason;
    const auto modules = LocalSemanticWorker::runtime_modules_directory(boost::filesystem::path(Slic3r::resources_dir()));
    if (!LocalSemanticWorker::read_runtime_configuration(boost::filesystem::path(Slic3r::data_dir()) / "local_semantic_runtime.json",
            boost::filesystem::path(Slic3r::resources_dir()) / "beauty-runtime", config, reason) || !config.enabled) return {};
    std::string identity = AI::model_artifact_sha256(config.python_executable);
    const auto inventory=config.python_executable.parent_path().parent_path()/"runtime-manifest.json";
    if(config.python_executable.parent_path().parent_path().filename()=="beauty-runtime") {
        const auto hash=AI::model_artifact_sha256(inventory);if(hash.empty())return {};identity+=hash;
    }
    for (const char* name : {"glb_artifact.py", "local_semantic_worker.py", "local_semantic_geometry.py", "local_semantic_render.py",
            "local_semantic_transform.py", "local_semantic_views.py", "local_semantic_projection.py", "local_semantic_pipeline.py",
            "local_semantic_request.py", "local_eye_landmarks.py", "local_face_landmarks.py", "local_shape_constraints.py", "local_brow_boundary.py", "local_body_regions.py",
            "local_contour_proposals.py","local_surface_contours.py","local_leaf_boundaries.py","beauty_leaf_domain.py",
            "local_hair_expansion_guard.py", "local_region_mask_consensus.py", "local_region_mask_projection.py",
            "local_surface_region_consensus.py", "local_surface_region_holes.py"}) {
        const auto hash = AI::model_artifact_sha256(modules / name);
        if (hash.empty()) return {};
        identity += std::string(name) + hash;
    }
    for (const char* name : {"mobilenet0.25_Final.pth", "face_parsing.farl.celebm.main_ema_181500_jit.pt", "face_landmarker.task"}) {
        const auto hash = AI::model_artifact_sha256(config.weights_directory / name);
        if (hash.empty()) return {};
        identity += std::string(name) + hash;
    }
    const auto raster = modules / "local_semantic_raster.dll";
    if (boost::filesystem::exists(raster)) identity += AI::model_artifact_sha256(raster);
    return PortraitShapeCache::digest(identity);
}

std::string semantic_region_runtime_identity(const std::filesystem::path& runtime)
{
    if (runtime.empty()) return {};
    std::string identity = std::string(SC::pipeline_version) + provider_configuration(runtime);
    for (const char* name : {"runtime-manifest.json", "libmediapipe.dll", "face_landmarker.task",
                             "selfie_multiclass_256x256.tflite"}) {
        const auto hash = AI::model_artifact_sha256(boost::filesystem::path((runtime / name).native()));
        if (hash.empty()) return {};
        identity += std::string(name) + hash;
    }
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(identity.data(), identity.size(), bytes, &length, EVP_sha256(), nullptr) != 1 || length != 32) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned int i = 0; i < length; ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
    return result;
}

std::shared_ptr<const SemanticRegionEvidence> load_legacy_semantic_region_evidence(
    const SC::MeshSnapshot& source, const std::filesystem::path& runtime, const std::filesystem::path& cache,
    const std::string& runtime_identity, std::string& error)
{
    try {
        if (runtime_identity.empty() || !std::filesystem::is_directory(cache) || std::filesystem::is_empty(cache)) return {};
        const auto configuration = nlohmann::json::parse(provider_configuration(runtime));
        // Reading identities and decoding an existing file never calls predict/analyze.
        auto providers = SC::create_region_recognizers(configuration.value("body_provider", "mediapipe.cpu.v1"),
            configuration.value("face_provider", "mediapipe.cpu.v1"), runtime);
        if (!providers.error.empty() || !providers.body || !providers.face) return {};
        const auto file = cache / (SC::analysis_cache_key(source, providers.body->identity(), providers.face->identity()) + ".json");
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(file, ec);
        if (ec) return {};
        if (bytes > SemanticRegionEvidenceCache::maximum_bytes) throw std::runtime_error("Legacy analysis cache too large");
        std::ifstream input(file, std::ios::binary);
        SC::Analysis analysis;
        if (!SC::decode_analysis(nlohmann::json::parse(input, nullptr, false), source,
            providers.body->identity(), providers.face->identity(), analysis, error)) return {};
        return SemanticRegionEvidence::from_analysis(analysis, runtime_identity);
    } catch (const std::exception& e) { error = e.what(); return {}; }
}

GLModel::Geometry build_semantic_colored_geometry(const SC::MeshSnapshot& source, const SC::FaceColors& paint,
                                                   const SC::SubfaceColors& subfaces, const SC::Cancel& cancel,
                                                   const nlohmann::json* surface_partition,
                                                   const std::map<std::string,SC::Color>* cell_colors)
{
    GLModel::Geometry geometry;
    geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2};
    const auto& mesh = source.mesh;
    std::map<size_t,const nlohmann::json*> clipped;
    if(surface_partition) {
        if(surface_partition->at("geometry_id")!=source.geometry_id || surface_partition->at("face_count")!=mesh.indices.size()) return {};
        for(const auto& row:surface_partition->at("faces")) clipped.emplace(row.at("source_face_id"),&row);
    }
    const auto normals = ModelPreviewNormals::corner_normals(mesh);
    std::vector<int> indices(mesh.indices.size(), -1);
    for (size_t i = 0; i < paint.size(); ++i)
        if (paint[i].first < indices.size()) indices[paint[i].first] = int(i);
    std::map<size_t, std::map<SC::SubfacePath, SC::Color>> children;
    for (const SC::SubfaceColor& item : subfaces) {
        if (item.face_id >= mesh.indices.size() || item.path.depth == 0 || item.path.depth > 4 ||
            unsigned(item.path.value) >= (1u << (2u * item.path.depth))) return {};
        children[item.face_id][item.path] = item.color;
    }
    geometry.reserve_vertices((mesh.indices.size() + subfaces.size() * 3) * 3);
    geometry.reserve_indices((mesh.indices.size() + subfaces.size() * 3) * 3);
    const bool vertex = source.vertex_colors.size() == mesh.vertices.size();
    const bool face = source.face_colors.size() == mesh.indices.size();
    const RGBA fallback {.7f, .7f, .7f, 1.f};
    for (size_t f = 0; f < mesh.indices.size(); ++f) {
        if ((f & 4095) == 0 && cancel && cancel()) return {};
        const auto& triangle = mesh.indices[f];
        std::vector<SC::SubfacePath> leaves;
        const auto child_colors = children.find(f);
        if (child_colors == children.end()) {
            leaves.push_back({0, 0});
        } else {
            std::set<SC::SubfacePath> branches;
            for (const auto& item : child_colors->second)
                for (uint8_t level=0;level<item.first.depth;++level)
                    branches.insert({level,uint8_t(item.first.value>>(2*(item.first.depth-level)))});
            const auto visit = [&](const auto& self, SC::SubfacePath path)->void {
                if (!branches.count(path)) { leaves.push_back(path); return; }
                for (uint8_t child=0;child<4;++child) self(self,{uint8_t(path.depth+1),uint8_t((path.value<<2)|child)});
            };
            visit(visit,{0,0});
        }
        for (const SC::SubfacePath& leaf : leaves) {
            std::array<SC::Barycentric, 3> barycentric;
            const auto corners = AI::BeautyLeafKey{f,leaf.depth,leaf.value}.barycentric();
            for (size_t i=0;i<3;++i) for (size_t c=0;c<3;++c) barycentric[i][c]=float(corners[i][c]);
            const SC::Color* leaf_color = nullptr;
            if (child_colors != children.end()) {
                for (uint8_t depth=leaf.depth;depth>0;--depth) {
                    const auto found=child_colors->second.find({depth,uint8_t(leaf.value>>(2*(leaf.depth-depth)))});
                    if (found!=child_colors->second.end()) { leaf_color=&found->second; break; }
                }
            }
            if (leaf_color == nullptr && indices[f] >= 0) leaf_color = &paint[size_t(indices[f])].second;
            struct Part {std::array<SC::Barycentric,3> corners;const SC::Color* color;};
            std::vector<Part> parts;
            const auto cut=clipped.find(f);
            if(cut==clipped.end()) {
                if (cell_colors) {
                    const auto manual = cell_colors->find("source:" + std::to_string(f));
                    if (manual != cell_colors->end()) leaf_color = &manual->second;
                }
                parts.push_back({barycentric,leaf_color});
            }
            else {
                nlohmann::json triangle=nlohmann::json::array();
                for(const auto& bary:barycentric) triangle.push_back({bary[0],bary[1],bary[2]});
                const auto region=AI::SurfacePartition::polygons(nlohmann::json::array({{{"polygon",triangle},{"holes",nlohmann::json::array()}}}),true);
                for(const auto& cell:cut->second->at("cells")) {
                    const auto intersections=intersection_ex(region,AI::SurfacePartition::polygons(nlohmann::json::array({cell}),true));
                    const SC::Color* target=leaf_color;
                    if(cell_colors) {
                        const auto found=cell_colors->find(cell.at("id"));
                        if(found!=cell_colors->end()) target=&found->second;
                    }
                    for(const auto& polygon:intersections) for(const auto& item:AI::SurfacePartition::triangles(polygon)) {
                        Part part;part.color=target;
                        for(size_t i=0;i<3;++i) for(size_t k=0;k<3;++k) part.corners[i][k]=item[i][k].get<float>();
                        parts.push_back(std::move(part));
                    }
                }
            }
            for(const auto& part:parts) {
            const unsigned base = unsigned(geometry.vertices_count());
            for (const SC::Barycentric& bary : part.corners) {
                Vec3f position = Vec3f::Zero(), normal = Vec3f::Zero();
                RGBA original {0.f,0.f,0.f,0.f};
                for (size_t corner = 0; corner < 3; ++corner) {
                    position += bary[corner] * mesh.vertices[triangle[corner]];
                    normal += bary[corner] * normals[f * 3 + corner];
                    const auto& color = vertex ? source.vertex_colors[triangle[corner]] :
                        face ? source.face_colors[f] : fallback;
                    for (size_t channel = 0; channel < 4; ++channel)
                        original[channel] += bary[corner] * color[channel];
                }
                if (normal.squaredNorm() > 1e-12f) normal.normalize();
                const SC::Color rgb = part.color == nullptr
                    ? SC::Color {original[0], original[1], original[2]} : *part.color;
                geometry.add_vertex(position, normal, Vec2f(float(preview_rgb8(rgb[0], rgb[1], rgb[2])),
                    part.color == nullptr ? original[3] : -1.f));
            }
            geometry.add_triangle(base, base + 1, base + 2);
            if(surface_partition && geometry.indices_count()/3>mesh.indices.size()+
                surface_partition->at("triangle_budget").get<size_t>()) return {};
            }
        }
    }
    return geometry;
}

struct ModelSemanticColoring::Impl {
    struct Request {
        std::shared_ptr<const Snapshot> source;
        std::vector<Color> palette, targets, portrait;
        std::vector<std::string> palette_roles;
        FaceColors manual;
        std::string configuration;
        uint64_t generation {0};
        std::filesystem::path source_path;
        std::shared_ptr<const PortraitShapeDetails> shapes;
        bool allow_boundary_upgrade = true;
        std::map<std::string, Color> manual_cells;
        std::shared_ptr<PortraitOptimizationTask> portrait_progress;
        std::shared_ptr<const SavedAppearance> saved_appearance;
    } request;
    struct Job {
        Request request;
        std::atomic<bool> canceled {false}, done {false};
        std::atomic<int> progress {0};
        std::unique_ptr<Result> result;
    };
    std::filesystem::path runtime, cache;
    SC::RegionRecognizers providers;
    std::string providers_configuration;
    std::shared_ptr<const SC::Analysis> cached_analysis;
    std::shared_ptr<Job> job;
    std::thread worker;
    uint64_t generation {0};
    bool wanted {false}, pending {false};

    void start() {
        if (!pending || job || !wanted) return;
        pending = false;
        job = std::make_shared<Job>(); job->request = request;
        const auto task = job;
        try {
        worker = std::thread([this, task] {
            const auto started = std::chrono::steady_clock::now();
            auto result = std::make_unique<Result>();
            const auto portrait = task->request.portrait_progress;
            std::string contour_evidence = task->request.shapes
                ? " 连续边界：恢复已保存的同源细节；父级证据独立校验。" : std::string();
            const auto report = [portrait](const PortraitProgress& progress) { if (portrait) portrait->report(progress); };
            report({PortraitStage::Preparing, "校验模型、原始贴图与运行时", 0, 0});
            const SC::Cancel cancel = [task] { return task->canceled.load(); };
            try {
                result->region_runtime_identity = semantic_region_runtime_identity(runtime);
                if (!providers.body || !providers.face || providers_configuration != task->request.configuration) {
                    std::string body_id = "mediapipe.cpu.v1", face_id = body_id;
                    const auto doc = nlohmann::json::parse(task->request.configuration);
                    body_id = doc.value("body_provider", body_id);
                    face_id = doc.value("face_provider", face_id);
                    providers = SC::create_region_recognizers(body_id, face_id, runtime);
                    providers_configuration = task->request.configuration;
                    cached_analysis.reset();
                }
                if (!providers.error.empty()) throw std::runtime_error(providers.error);
                if (!providers.body || !providers.face) throw std::runtime_error("Local region recognizers unavailable");
                std::shared_ptr<Snapshot> original_source;
                if (task->request.shapes && task->request.shapes->compatible(
                        task->request.source->geometry_id, task->request.source->mesh.indices.size())) {
                    const auto original = PortraitShapeCache::verified_source(
                        boost::filesystem::path(Slic3r::data_dir()) / "cache", task->request.shapes->locks.source_sha256);
                    TriangleMesh mesh;
                    ObjInfo colors;
                    std::string error;
                    if (!AI::load_model_artifact(original, mesh, colors, error)) throw std::runtime_error(error);
                    original_source = std::make_shared<Snapshot>();
                    original_source->mesh = std::move(mesh.its);
                    original_source->geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(original_source->mesh);
                    if (original_source->geometry_id != task->request.source->geometry_id ||
                        original_source->mesh.indices.size() != task->request.source->mesh.indices.size())
                        throw std::runtime_error("Original portrait face mapping changed.");
                    original_source->vertex_colors = std::move(colors.vertex_colors);
                    original_source->face_colors = std::move(colors.face_colors);
                    original_source->content_id = SC::content_fingerprint(*original_source);
                    result->verified_original_source = true;
                }
                const auto& source = original_source ? *original_source : *task->request.source;
                const auto semantic_modules = LocalSemanticWorker::runtime_modules_directory(
                    boost::filesystem::path(Slic3r::resources_dir()));
                const auto key = SC::analysis_cache_key(source, providers.body->identity(), providers.face->identity());
                auto analysis = std::make_shared<SC::Analysis>();
                const auto cache_file = cache / (key + ".json");
                if (cached_analysis && cached_analysis->signature == key) {
                    analysis = std::make_shared<SC::Analysis>(*cached_analysis);
                    result->cache_hit = true;
                } else {
                    std::error_code ec;
                    const auto bytes = std::filesystem::file_size(cache_file, ec);
                    if (!ec && bytes <= 128ULL * 1024 * 1024) {
                        std::ifstream input(cache_file, std::ios::binary);
                        const auto doc = nlohmann::json::parse(input, nullptr, false);
                        std::string error;
                        result->cache_hit = SC::decode_analysis(doc, source, providers.body->identity(), providers.face->identity(), *analysis, error);
                    }
                }
                if (!result->cache_hit && !cancel()) {
                    *analysis = SC::analyze(source, *providers.body, *providers.face, cancel,
                        [task, report](int value, const std::string& detail) {
                            task->progress.store(std::clamp(value, 0, 95));
                            report({PortraitStage::Recognizing, detail, 0, 0});
                        });
                    if (!analysis->canceled && analysis->error.empty() && !cancel()) {
                        std::error_code ec;
                        std::filesystem::create_directories(cache, ec);
                        if (!ec) {
                            auto temporary = cache_file; temporary += ".tmp";
                            { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output << SC::encode_analysis(*analysis); }
                            std::filesystem::rename(temporary, cache_file, ec);
                            if (ec) std::filesystem::remove(temporary, ec);
                        }
                    }
                }
                if (!cancel()) {
                    result->analysis = analysis;
                    result->person_detected = analysis->person_detected;
                    result->error = analysis->error;
                    if (analysis->error.empty() && !analysis->canceled) {
                        cached_analysis = analysis;
                        const auto source_automatic = SC::map_palette(
                            source, *analysis, task->request.palette, task->request.portrait);
                        result->automatic = SC::remap_palette_targets(source_automatic,
                            task->request.palette, task->request.targets);
                        if (task->request.palette.size() <= 4) {
                            // Slot remapping happens after semantic mapping. A
                            // safe source slot can therefore become red in the
                            // displayed target palette; enforce the same rule
                            // once more on the actual output colors.
                            std::vector<uint8_t> eye_vertices(source.mesh.vertices.size(), 0);
                            std::vector<size_t> eye_canonical(source.mesh.vertices.size());
                            std::iota(eye_canonical.begin(), eye_canonical.end(), 0);
                            if (!source.mesh.vertices.empty()) {
                                Vec3f lower = source.mesh.vertices.front(), upper = lower;
                                for (const auto& vertex : source.mesh.vertices) {
                                    lower = lower.cwiseMin(vertex);
                                    upper = upper.cwiseMax(vertex);
                                }
                                const float seam_tolerance = std::max((upper - lower).norm() * 1e-7f, 1e-6f);
                                const float seam_tolerance_squared = seam_tolerance * seam_tolerance;
                                std::vector<size_t> vertex_order(source.mesh.vertices.size());
                                std::iota(vertex_order.begin(), vertex_order.end(), 0);
                                std::sort(vertex_order.begin(), vertex_order.end(), [&](size_t lhs, size_t rhs) {
                                    for (int axis = 0; axis < 3; ++axis) {
                                        if (source.mesh.vertices[lhs][axis] < source.mesh.vertices[rhs][axis]) return true;
                                        if (source.mesh.vertices[lhs][axis] > source.mesh.vertices[rhs][axis]) return false;
                                    }
                                    return lhs < rhs;
                                });
                                size_t canonical_id = vertex_order.front();
                                for (size_t vertex : vertex_order) {
                                    const Vec3f delta = source.mesh.vertices[vertex] - source.mesh.vertices[canonical_id];
                                    if (delta.squaredNorm() > seam_tolerance_squared) canonical_id = vertex;
                                    eye_canonical[vertex] = canonical_id;
                                }
                            }
                            for (size_t face_id = 0; face_id < analysis->face_labels.size(); ++face_id) {
                                const auto label = analysis->face_labels[face_id];
                                if (label != SC::Label::EyeSclera && label != SC::Label::Iris &&
                                    label != SC::Label::Eyebrow) continue;
                                if (face_id >= source.mesh.indices.size()) continue;
                                for (int corner = 0; corner < 3; ++corner) {
                                    const int vertex = source.mesh.indices[face_id][corner];
                                    if (vertex >= 0 && size_t(vertex) < eye_vertices.size()) eye_vertices[eye_canonical[size_t(vertex)]] = 1;
                                }
                            }
                            const auto eye_zone_face = [&](size_t face_id) {
                                if (face_id >= source.mesh.indices.size() || face_id >= analysis->face_labels.size()) return false;
                                const auto label = analysis->face_labels[face_id];
                                if (label == SC::Label::EyeSclera || label == SC::Label::Iris || label == SC::Label::Eyebrow)
                                    return true;
                                if (label == SC::Label::Hair || label == SC::Label::Clothes || label == SC::Label::Accessories ||
                                    label == SC::Label::Lips) return false;
                                for (int corner = 0; corner < 3; ++corner) {
                                    const int vertex = source.mesh.indices[face_id][corner];
                                    if (vertex >= 0 && size_t(vertex) < eye_vertices.size() && eye_vertices[eye_canonical[size_t(vertex)]]) return true;
                                }
                                return false;
                            };
                            std::map<size_t, SC::Color> source_by_face;
                            for (const auto& item : source_automatic) source_by_face[item.first] = item.second;
                            std::map<size_t, SC::Color> safe;
                            for (const auto& item : result->automatic) {
                                const auto source_found = source_by_face.find(item.first);
                                const size_t proposed = [&]() {
                                    if (source_found == source_by_face.end()) return task->request.targets.size();
                                    const auto slot = std::find(task->request.palette.begin(), task->request.palette.end(), source_found->second);
                                    return slot == task->request.palette.end() ? task->request.targets.size() :
                                        size_t(slot - task->request.palette.begin());
                                }();
                                const auto label = item.first < analysis->face_labels.size()
                                    ? analysis->face_labels[item.first] : SC::Label::Unknown;
                                const bool facial = label == SC::Label::EyeSclera || label == SC::Label::Iris ||
                                    label == SC::Label::Eyebrow || label == SC::Label::FaceSkin || label == SC::Label::BodySkin;
                                const bool eye_zone = eye_zone_face(item.first);
                                const bool neutral_garment = label == SC::Label::Clothes && source_found != source_by_face.end() &&
                                    neutral_preview_color(source_found->second);
                                const bool source_red = source_found != source_by_face.end() && red_preview_color(source_found->second);
                                const bool block_red = facial || eye_zone || neutral_garment ||
                                    ((label == SC::Label::Clothes || label == SC::Label::Hair || label == SC::Label::Accessories) && !source_red);
                                const auto acceptable = [&](size_t slot) {
                                    if (slot >= task->request.targets.size()) return false;
                                    if (block_red && red_preview_color(task->request.targets[slot])) return false;
                                    if (neutral_garment && (!neutral_preview_color(task->request.targets[slot]) ||
                                                             task->request.targets[slot][0] < .35f)) return false;
                                    return true;
                                };
                                size_t selected = proposed;
                                if (!acceptable(selected)) {
                                    selected = task->request.targets.size();
                                    float best = std::numeric_limits<float>::max();
                                    for (size_t slot = 0; slot < task->request.targets.size(); ++slot) {
                                        if (!acceptable(slot)) continue;
                                        const auto& source_color = source_found != source_by_face.end()
                                            ? source_found->second : item.second;
                                        const float score = (source_color[0] - task->request.targets[slot][0]) *
                                            (source_color[0] - task->request.targets[slot][0]) +
                                            (source_color[1] - task->request.targets[slot][1]) *
                                            (source_color[1] - task->request.targets[slot][1]) +
                                            (source_color[2] - task->request.targets[slot][2]) *
                                            (source_color[2] - task->request.targets[slot][2]);
                                        if (score < best) { best = score; selected = slot; }
                                    }
                                }
                                if (selected < task->request.targets.size()) safe[item.first] = task->request.targets[selected];
                            }
                            result->automatic.assign(safe.begin(), safe.end());
                        }
                        SC::SubfaceBudgetResult source_subfaces;
                        std::string subface_error;
                        if (!SC::map_subface_palette(source, *analysis, source_automatic, task->request.palette,
                                task->request.portrait, {}, source_subfaces, subface_error))
                            throw std::runtime_error(subface_error);
                        if (task->request.palette.size() <= 4) {
                            // Temporary limited-palette safeguard: eye detail
                            // candidates are individually colored from tiny
                            // raster samples and can introduce white/skin/iris
                            // speckles. Keep whole-face eye mapping and all
                            // non-eye boundary repairs (hair, clothing, skin).
                            source_subfaces.accepted.erase(std::remove_if(
                                source_subfaces.accepted.begin(), source_subfaces.accepted.end(),
                                [&analysis = *analysis](const SC::SubfaceColor& item) {
                                    if (item.face_id >= analysis.face_labels.size()) return false;
                                    if (analysis.face_labels[item.face_id] == SC::Label::Lips ||
                                        analysis.face_labels[item.face_id] == SC::Label::EyeSclera ||
                                        analysis.face_labels[item.face_id] == SC::Label::Iris ||
                                        analysis.face_labels[item.face_id] == SC::Label::Eyebrow)
                                        return true;
                                    return std::any_of(analysis.subface_labels.begin(), analysis.subface_labels.end(),
                                        [&item](const SC::SubfaceLabelEvidence& evidence) {
                                            return evidence.face_id == item.face_id && evidence.path == item.path &&
                                                (evidence.label == SC::Label::Lips ||
                                                 evidence.label == SC::Label::EyeSclera ||
                                                 evidence.label == SC::Label::Iris ||
                                                 evidence.label == SC::Label::Eyebrow);
                                        });
                                }), source_subfaces.accepted.end());
                        }
                        result->automatic_subfaces = SC::remap_subface_palette_targets(
                            source_subfaces.accepted, task->request.palette, task->request.targets);
                        result->native_automatic = result->automatic;
                        result->native_automatic_subfaces = result->automatic_subfaces;
                        const auto fingerprint = portrait_shape_runtime_fingerprint();
                        const auto previous_shapes = task->request.shapes && task->request.shapes->compatible(
                            source.geometry_id, source.mesh.indices.size()) ? task->request.shapes : nullptr;
                        result->shape_details = task->request.shapes;
                        if (result->shape_details && (!result->shape_details->compatible(source.geometry_id, source.mesh.indices.size()) ||
                                (!result->shape_details->surface_partition && (fingerprint.empty() ||
                                 result->shape_details->runtime_fingerprint != fingerprint)))) result->shape_details.reset();
                        PortraitParentCleanup::Result parent_cleanup;
                        const auto cleanup_roots = std::array<boost::filesystem::path,2>{semantic_modules,
                            boost::filesystem::path(Slic3r::resources_dir()) / "beauty-runtime" / "modules"};
                        const auto source_hash = previous_shapes ? previous_shapes->locks.source_sha256 :
                            task->request.source_path.empty() ? std::string() :
                            AI::model_artifact_sha256(boost::filesystem::path(task->request.source_path.native()));
                        for (const auto& root : cleanup_roots) {
                            if (cancel()) break;
                            const auto* current = result->shape_details.get();
                            // A shipped historical table is not evidence for a new request.
                            // Reuse it only alongside current source-bound evidence with
                            // the identical evidence/runtime/policy identity.
                            if (!current || !current->current_parent_evidence(fingerprint)) {
                                result->parent_repair_status = "reviewed_candidate_requires_current_evidence";
                                continue;
                            }
                            const auto bundle = PortraitParentCleanup::load_bundle(root, source_hash,
                                source.geometry_id, source.mesh.indices.size());
                            if (!bundle.partition) { result->parent_repair_status = bundle.reason; continue; }
                            const auto& parent_identity=current->parent_evidence_identity;
                            const auto evidence_hash=parent_identity.empty() ? nlohmann::json(current->locks.evidence_sha256) : parent_identity.at("evidence_sha256");
                            const auto runtime_hash=parent_identity.empty() ? nlohmann::json(current->locks.runtime_sha256) : parent_identity.at("runtime_sha256");
                            const auto policy_hash=parent_identity.empty() ? nlohmann::json(current->locks.policy_sha256) : parent_identity.at("policy_sha256");
                            if(bundle.partition->at("evidence_sha256")!=evidence_hash || bundle.partition->at("runtime_sha256")!=runtime_hash ||
                               bundle.partition->at("policy_sha256")!=policy_hash) {
                                result->parent_repair_status = "reviewed_candidate_evidence_identity_changed";
                                continue;
                            }
                            if (!PortraitParentCleanup::can_adopt(bundle,
                                    current ? current->surface_partition.get() : nullptr,
                                    current ? current->contour_locks.get() : nullptr, task->request.allow_boundary_upgrade)) {
                                result->parent_repair_status = "edited_or_frozen_boundary_preserved";
                                continue;
                            }
                            const auto colors = PortraitParentCleanup::colors(bundle, task->request.palette_roles, task->request.targets);
                            if (!colors.applied) { result->parent_repair_status = colors.reason; continue; }
                            PortraitShapeCache::preserve_source(boost::filesystem::path(Slic3r::data_dir()) / "cache",
                                previous_shapes ? PortraitShapeCache::verified_source(boost::filesystem::path(Slic3r::data_dir()) / "cache", source_hash) :
                                    boost::filesystem::path(task->request.source_path.native()), source_hash);
                            auto details = current ? std::make_shared<PortraitShapeDetails>(*current) : std::make_shared<PortraitShapeDetails>();
                            details->locks = {};
                            details->locks.geometry_id = source.geometry_id;
                            details->locks.source_sha256 = source_hash;
                            details->locks.face_count = source.mesh.indices.size();
                            details->locks.evidence_sha256 = bundle.partition->at("evidence_sha256");
                            details->locks.runtime_sha256 = bundle.partition->at("runtime_sha256");
                            details->locks.policy_sha256 = bundle.partition->at("policy_sha256");
                            details->surface_ownership.reset();
                            details->color_inheritance.reset();
                            details->surface_partition = bundle.partition;
                            details->contour_locks = bundle.locks;
                            details->cell_colors = current ? PortraitParentCleanup::inherited_cell_colors(bundle, current->cell_colors) :
                                std::map<std::string, SC::Color>{};
                            details->base_colors = previous_shapes ? previous_shapes->base_colors :
                                source.face_colors.size() == source.mesh.indices.size() ? source.face_colors :
                                    AI::beauty_source_face_colors(source.mesh, source.vertex_colors);
                            details->runtime_fingerprint = fingerprint;
                            std::set<std::string> subjects;
                            for (const auto& lock : bundle.locks->document.at("locks")) subjects.insert(lock.at("subject_id").get<std::string>());
                            details->subjects.assign(subjects.begin(), subjects.end());
                            details->confirmed_parent_roots = colors.root_labels;
                            details->confirmed_parent_cell_labels = colors.cell_labels;
                            if (!details->compatible(source.geometry_id, source.mesh.indices.size()))
                                throw std::runtime_error("Reviewed parent partition source mapping changed.");
                            result->shape_details = std::move(details);
                            result->verified_original_source = true;
                            parent_cleanup = colors;
                            break;
                        }
                        const bool refresh_parent = result->shape_details && result->shape_details->surface_partition &&
                            !result->shape_details->current_parent_evidence(fingerprint) && !parent_cleanup.applied;
                        const auto retained_shapes = result->shape_details;
                        if ((!result->shape_details || refresh_parent) && !task->request.source_path.empty() && !cancel()) {
                          try {
                            LocalSemanticWorker::Configuration config;
                            std::string reason;
                            const auto data = boost::filesystem::path(Slic3r::data_dir());
                            const auto modules = LocalSemanticWorker::runtime_modules_directory(
                                boost::filesystem::path(Slic3r::resources_dir()));
                            if (LocalSemanticWorker::read_runtime_configuration(data / "local_semantic_runtime.json",
                                    boost::filesystem::path(Slic3r::resources_dir()) / "beauty-runtime", config, reason) && config.enabled) {
                                const auto requests = data / "beauty_semantic_requests";
                                boost::filesystem::create_directories(requests);
                                const auto owned = requests / boost::filesystem::unique_path("portrait-%%%%-%%%%-%%%%");
                                const auto cache = data / "cache";
                                const auto original = previous_shapes
                                    ? PortraitShapeCache::verified_source(cache, previous_shapes->locks.source_sha256)
                                    : PortraitShapeCache::preserve_source(cache, boost::filesystem::path(task->request.source_path.native()),
                                        AI::model_artifact_sha256(boost::filesystem::path(task->request.source_path.native())));
                                if (portrait) portrait->history(result->cache_hit ? "native-cached-" : "native-cold-",{});
                                const auto local = LocalSemanticWorker::analyze(config, modules, owned,
                                    original, source.mesh, task->canceled, data / "beauty_semantic_cache", portrait);
                                std::string cleanup;
                                LocalSemanticWorker::cleanup_request(owned, requests, cleanup);
                                if (local.process.status == LocalSemanticWorker::Status::Ready && !cancel()) {
                                    contour_evidence = local.cache_hit
                                        ? " 连续裁切与父级：复用当前源、几何、运行时和策略匹配的本地证据缓存。"
                                        : " 连续裁切与父级：本次从当前资产重新识别并校验证据。";
                                    auto details = refresh_parent ? std::make_shared<PortraitShapeDetails>(*retained_shapes) :
                                        std::make_shared<PortraitShapeDetails>();
                                    if (!refresh_parent) {
                                    details->locks = AI::ShapeLockSet::from_evidence(local.evidence, local.evidence_sha256);
                                    details->subjects = local.evidence.subjects;
                                    details->runtime_fingerprint = fingerprint;
                                    details->base_colors = previous_shapes ? previous_shapes->base_colors :
                                        source.face_colors.size() == source.mesh.indices.size() ? source.face_colors :
                                            AI::beauty_source_face_colors(source.mesh, source.vertex_colors);
                                    std::set<size_t> reserved;
                                    for (const auto& region : local.evidence.regions) if (AI::ShapeLockSet::supported_label(region.label))
                                        reserved.insert(region.faces.begin(), region.faces.end());
                                    for (const auto& shape : local.evidence.shape_details) {
                                        reserved.insert(shape.accepted_faces.begin(), shape.accepted_faces.end());
                                        reserved.insert(shape.rejected_faces.begin(), shape.rejected_faces.end());
                                    }
                                    details->reserved_faces.assign(reserved.begin(), reserved.end());
                                    if(!local.contour_request.is_null()) {
                                      try {
                                        auto partition=build_verified_contour_partition(local.contour_request,local.evidence,source.mesh,local.evidence_sha256);
                                        const auto hash=PortraitShapeCache::digest(partition.dump());
                                        details->contour_locks=std::make_shared<AI::BeautySurfaceShapeLock>(
                                            AI::BeautySurfaceShapeLock::from_partition(partition,local.evidence.shape_details,hash));
                                        details->surface_partition=std::make_shared<nlohmann::json>(std::move(partition));
                                      } catch(const std::exception& error) {
                                        details->locks.locks.clear();
                                        result->shape_error=std::string("Contour proposal retained the previous appearance: ")+error.what();
                                      }
                                    }
                                    }
                                    details->bind_parent_evidence(local.evidence,local.evidence_sha256,fingerprint);
                                    if(local.contour_request.contains("parent_proposal")) {
                                        PortraitParentCoverage::validate(local.contour_request.at("parent_proposal"),*details);
                                        details->parent_proposal=std::make_shared<nlohmann::json>(local.contour_request.at("parent_proposal"));
                                    }
                                    if (!details->compatible(source.geometry_id, source.mesh.indices.size())) throw std::runtime_error("Local shape source mapping changed.");
                                    result->shape_details = std::move(details);
                                } else result->shape_error = local.process.reason;
                            } else result->shape_error = reason.empty() ? "local_semantic_runtime_disabled" : reason;
                          } catch (const std::exception& error) {
                              result->shape_details=refresh_parent ? retained_shapes : nullptr;
                              result->shape_error = error.what();
                          }
                        }
                        if (portrait && !result->shape_error.empty() &&
                            (!result->shape_details || !result->shape_details->current_parent_evidence(fingerprint)))
                            throw std::runtime_error("Local portrait evidence failed: " + result->shape_error);
                        report({PortraitStage::Coloring, "校验连续裁切，合成手工颜色、受保护细节和父级配色", 0, 0});
                        SC::FaceColors locked_colors;
                        const bool contour_plan=result->shape_details && result->shape_details->surface_partition;
                        const bool leaf_plan=result->shape_details && result->shape_details->locks.leaf_domain;
                        if(contour_plan && !cancel()) {
                            auto details=std::make_shared<PortraitShapeDetails>(*result->shape_details);
                            // Refresh parent evidence over the accepted appearance. Missing
                            // new evidence is not permission to erase an earlier assignment.
                            if (task->request.saved_appearance) {
                                const auto& saved=*task->request.saved_appearance;
                                if(saved.geometry_id!=source.geometry_id || saved.face_count!=source.mesh.indices.size())
                                    throw std::invalid_argument("Saved portrait appearance source changed.");
                                result->automatic=saved.faces;
                                result->automatic_subfaces=saved.subfaces;
                            }
                            if (!parent_cleanup.applied) {
                                const auto reviewed_status=result->parent_repair_status;
                                if(details->current_parent_evidence(fingerprint)) {
                                    PortraitParentCoverage::apply(*details,source,*analysis,task->request.manual,task->request.manual_cells,cancel);
                                    parent_cleanup=PortraitParentCleanup::Live::colors(*details,source,*analysis,
                                        task->request.palette_roles,task->request.targets,result->automatic,
                                        task->request.manual,task->request.manual_cells,cancel);
                                }
                                else {
                                    parent_cleanup.reason="current_parent_evidence_unavailable";
                                    parent_cleanup.audit={{"status",parent_cleanup.reason},{"source_face_denominator",source.mesh.indices.size()},
                                        {"uniform_units",0},{"shape_diagnostic",result->shape_error}};
                                }
                                result->parent_repair_status=parent_cleanup.reason;
                                if(!parent_cleanup.audit.empty())parent_cleanup.audit["reviewed_candidate_status"]=reviewed_status;
                                result->parent_repair_audit=parent_cleanup.audit;
                                details->parent_cleanup_audit=parent_cleanup.audit;
                                for(const auto& row:parent_cleanup.root_labels)details->confirmed_parent_roots[row.first]=row.second;
                                for(const auto& row:parent_cleanup.cell_labels)details->confirmed_parent_cell_labels[row.first]=row.second;
                            }
                            std::set<size_t> previous_contour_roots;
                            for(const auto& row:result->shape_details->surface_partition->at("faces"))if(row.value("status",std::string()).compare(0,3,"R9_")!=0)
                                previous_contour_roots.insert(row.at("source_face_id").get<size_t>());
                            compose_portrait_shapes(*analysis,details.get(),result->automatic,result->automatic_subfaces);
                            result->automatic_subfaces.erase(std::remove_if(result->automatic_subfaces.begin(),result->automatic_subfaces.end(),
                                [&](const auto& child){return previous_contour_roots.count(child.face_id)!=0;}),result->automatic_subfaces.end());
                            // Composition retains legitimate parent color leaves.
                            // Suppressed native facial leaves must stay suppressed,
                            // including on unconfirmed siblings of a new parent cut.
                            PortraitParentCleanup::apply_roots(parent_cleanup, result->automatic, result->automatic_subfaces);
                            SC::SubfaceBudget budget;
                            budget.maximum_added_triangles=details->surface_partition->at("triangle_budget").get<size_t>()-
                                details->surface_partition->at("added_triangles").get<size_t>();
                            budget.maximum_added_ratio=.02f;budget.minimum_confidence=0;
                            SC::SubfaceBudgetResult admitted;std::string budget_error;
                            if(!SC::enforce_subface_budget(result->automatic_subfaces,source.mesh.indices.size(),budget,admitted,budget_error))
                                throw std::runtime_error(budget_error);
                            result->automatic_subfaces=std::move(admitted.accepted);
                            std::set<std::string> missing_roles;
                            const auto colors=portrait_contour_colors(*details->surface_partition,task->request.palette_roles,task->request.targets,&missing_roles);
                            for(const auto& entry:colors) details->cell_colors[entry.first]=entry.second;
                            details->color_diagnostic.clear();
                            for(const auto& role:missing_roles) {
                                if(!details->color_diagnostic.empty()) details->color_diagnostic+=", ";
                                details->color_diagnostic+=role;
                            }
                            if(!missing_roles.empty()) result->shape_error="Portrait color roles unavailable; affected colors were preserved: "+details->color_diagnostic;
                            for (const auto& entry : parent_cleanup.cell_colors) details->cell_colors[entry.first] = entry.second;
                            if (parent_cleanup.applied) {
                                result->parent_repair_cells = parent_cleanup.applied;
                                result->parent_repair_status = parent_cleanup.reason;
                            }
                            result->shape_details=std::move(details);
                        } else if (leaf_plan && !cancel()) {
                            const std::array<const char*,6> names{"portrait-skin","portrait-dark","portrait-light","portrait-lips","portrait-cool","portrait-mid"};
                            if (task->request.targets.size()<3 || task->request.targets.size()>6 || task->request.portrait.size()!=6)
                                throw std::runtime_error("The reviewed leaf boundary requires the fixed portrait card.");
                            std::vector<PortraitColorPlan::Slot> palette;
                            for (size_t slot=0;slot<task->request.targets.size();++slot) {
                                if (task->request.palette[slot]!=task->request.portrait[slot])
                                    throw std::runtime_error("Portrait card role order changed.");
                                palette.push_back({names[slot],task->request.targets[slot]});
                            }
                            const auto surface=AI::BeautySurface::build(source.mesh,source.vertex_colors,{},cancel);
                            if (result->shape_details->surface_ownership) {
                                const auto& inherited=result->shape_details->color_inheritance;
                                if (!inherited || inherited->palette!=task->request.targets)
                                    throw std::runtime_error("R6 parent repair requires the matching reviewed color baseline.");
                                result->automatic=inherited->faces;
                                result->automatic_subfaces=inherited->subfaces;
                            }
                            const auto plan=build_portrait_color_plan(*result->shape_details,source.mesh,*surface,*analysis,
                                palette,result->automatic,result->automatic_subfaces);
                            apply_portrait_color_plan(plan,result->automatic,result->automatic_subfaces);
                        } else if (result->shape_details && !result->shape_details->locks.empty() && !cancel()) {
                          try {
                            auto surface = AI::BeautySurface::build(source.mesh, source.vertex_colors, {}, cancel);
                            std::vector<AI::PhysicalFilamentChannel> channels;
                            for (size_t slot = 0; slot < task->request.targets.size(); ++slot) {
                                const auto& color = task->request.targets[slot];
                                std::ostringstream hex; hex << '#';
                                for (float value : color) hex << std::hex << std::setw(2) << std::setfill('0') <<
                                    unsigned(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
                                channels.push_back({slot, hex.str(), {}, true});
                            }
                            SC::FaceColors parent_colors;
                            for (const auto& item : result->automatic) if (item.first < analysis->face_labels.size() &&
                                (analysis->face_labels[item.first] == SC::Label::FaceSkin || analysis->face_labels[item.first] == SC::Label::BodySkin))
                                parent_colors.push_back(item);
                            locked_colors = match_portrait_shape_role_colors(*result->shape_details,task->request.targets,task->request.palette_roles);
                          } catch (const std::exception& error) {
                              result->shape_error = error.what();
                              result->shape_details.reset();
                          }
                        }
                        if (!leaf_plan && !contour_plan) compose_portrait_shapes(*analysis, result->shape_details.get(), result->automatic, result->automatic_subfaces, locked_colors);
                        result->subface_added_triangles = source_subfaces.added_triangles;
                        result->subface_rejected_candidates = source_subfaces.rejected_candidates;
                        if (!result->automatic.empty() || !result->automatic_subfaces.empty() || contour_plan) {
                            auto cell_colors=contour_plan ? result->shape_details->cell_colors : std::map<std::string,SC::Color>{};
                            if(contour_plan) {
                                const std::map<size_t,SC::Color> manual(task->request.manual.begin(),task->request.manual.end());
                                for(const auto& row:result->shape_details->surface_partition->at("faces")) {
                                    const auto color=manual.find(row.at("source_face_id"));
                                    if(color!=manual.end()) for(const auto& cell:row.at("cells")) cell_colors[cell.at("id")]=color->second;
                                }
                                for (const auto& entry : task->request.manual_cells) cell_colors[entry.first] = entry.second;
                            }
                            result->geometry = build_semantic_colored_geometry(*task->request.source,
                                SC::compose(result->automatic, task->request.manual, true),
                                SC::compose_subfaces(result->automatic_subfaces, task->request.manual, true), cancel,
                                contour_plan ? result->shape_details->surface_partition.get() : nullptr,
                                contour_plan ? &cell_colors : nullptr);
                            if(contour_plan && result->geometry.is_empty() && !cancel())
                                throw std::runtime_error("The combined contour editing surface exceeds its safe triangle budget.");
                        }
                    }
                }
            } catch (const std::exception& error) { result->error = error.what(); }
              catch (...) { result->error = "Unknown local region recognition failure"; }
            if (portrait && result->error.empty()) {
                std::string evidence = result->cache_hit ? "基础区域：复用同源缓存。" : "基础区域：当前资产新识别。";
                evidence += result->verified_original_source ? " 原始模型身份已验证。" : " 使用当前模型原始输入。";
                evidence += contour_evidence;
                if (task->request.shapes) evidence += " 已保存细节边界保留；父级证据独立校验。";
                evidence += " 未确认归属及受保护细节保留原外观；耳根、发际和颈侧仍需人工检查。";
                if (!result->shape_error.empty()) evidence += " " + result->shape_error;
                if (!result->parent_repair_status.empty()) evidence += " 父级处理：" + result->parent_repair_status;
                portrait->evidence(std::move(evidence), true);
            }
            result->elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            task->result = std::move(result);
            task->done.store(true);
        });
        } catch (const std::exception& error) {
            task->result = std::make_unique<Result>();
            task->result->error = error.what();
            task->done.store(true);
        }
    }
};

ModelSemanticColoring::ModelSemanticColoring(std::filesystem::path runtime, std::filesystem::path cache)
    : m_impl(std::make_unique<Impl>()) { m_impl->runtime = std::move(runtime); m_impl->cache = std::move(cache); }
ModelSemanticColoring::~ModelSemanticColoring() { cancel(); if (m_impl->worker.joinable()) m_impl->worker.join(); }
bool ModelSemanticColoring::request(std::shared_ptr<const Snapshot> source, std::vector<Color> palette, std::vector<Color> targets,
                                   std::vector<Color> portrait_card, FaceColors manual, std::filesystem::path source_path,
                                   std::shared_ptr<const PortraitShapeDetails> shapes,std::vector<std::string> palette_roles,
                                   bool allow_boundary_upgrade, std::map<std::string, Color> manual_cells,
                                   std::shared_ptr<PortraitOptimizationTask> progress,
                                   std::shared_ptr<const SavedAppearance> saved_appearance)
{
    auto& state = *m_impl;
    auto configuration = provider_configuration(state.runtime);
    const auto same_appearance=[&] {
        const auto& previous=state.request.saved_appearance;
        if(!previous || !saved_appearance) return bool(previous)==bool(saved_appearance);
        return previous->geometry_id==saved_appearance->geometry_id && previous->face_count==saved_appearance->face_count &&
            previous->faces==saved_appearance->faces && previous->subfaces.size()==saved_appearance->subfaces.size() &&
            std::equal(previous->subfaces.begin(),previous->subfaces.end(),saved_appearance->subfaces.begin(),
                [](const auto& a,const auto& b){return a.face_id==b.face_id && a.path==b.path && a.color==b.color;});
    };
    if (state.wanted && state.request.source == source && state.request.palette == palette &&
        state.request.targets == targets && state.request.portrait == portrait_card &&
        state.request.manual == manual && state.request.configuration == configuration &&
        state.request.source_path == source_path && state.request.shapes == shapes && state.request.palette_roles==palette_roles &&
        state.request.allow_boundary_upgrade == allow_boundary_upgrade && state.request.manual_cells == manual_cells && same_appearance()) return false;
    if (state.job) state.job->canceled.store(true);
    state.request = {std::move(source), std::move(palette), std::move(targets), std::move(portrait_card),std::move(palette_roles),
        std::move(manual), std::move(configuration), ++state.generation, std::move(source_path), std::move(shapes),
        allow_boundary_upgrade, std::move(manual_cells), std::move(progress),std::move(saved_appearance)};
    state.wanted = true; state.pending = true; state.start(); return true;
}
void ModelSemanticColoring::cancel() {
    auto& state = *m_impl;
    state.wanted = false; state.pending = false; ++state.generation;
    if (state.job) state.job->canceled.store(true);
}
std::unique_ptr<ModelSemanticColoring::Result> ModelSemanticColoring::poll() {
    auto& state = *m_impl;
    std::unique_ptr<Result> result;
    if (state.job && state.job->done.load()) {
        if (state.worker.joinable()) state.worker.join();
        if (!state.job->canceled.load() && state.wanted && state.job->request.generation == state.generation)
            result = std::move(state.job->result);
        state.job.reset();
    }
    state.start(); return result;
}
bool ModelSemanticColoring::busy() const { return m_impl->wanted && (m_impl->pending || bool(m_impl->job)); }
int ModelSemanticColoring::progress() const { return m_impl->job ? m_impl->job->progress.load() : 0; }
} // namespace Slic3r::GUI
