#include "ModelSemanticColoring.hpp"
#include "ModelColorPreviewShader.hpp"
#include "ModelPreviewNormals.hpp"
#include "ModelPreviewPalette.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>

namespace Slic3r::GUI {
namespace SC = AI::SemanticColoring;
namespace {
// Freeze provider selection with the request, so edits to the configuration
// cannot be read by a stale worker after the user has selected another model.
std::string provider_configuration(const std::filesystem::path& runtime)
{
    const auto file = runtime / "providers.json";
    std::error_code error;
    auto configuration = nlohmann::json::object();
    if (std::filesystem::exists(file, error)) {
        const auto bytes = std::filesystem::file_size(file, error);
        if (error || bytes > 65536) return "invalid local provider configuration";
        std::ifstream input(file, std::ios::binary);
        configuration = nlohmann::json::parse(input, nullptr, false);
        if (!configuration.is_object()) return "invalid local provider configuration";
    }
    // A resource restored or replaced at the same pathname must invalidate the
    // live provider. The adapter revalidates fixed content hashes on creation.
    auto stamps = nlohmann::json::array();
    std::vector<std::filesystem::path> paths;
    for (std::filesystem::recursive_directory_iterator it(runtime, error), end; !error && it != end; it.increment(error)) {
        if (it->is_directory(error) && it->path().filename() == "cache") { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(error)) continue;
        const auto ext = it->path().extension().string();
        if (ext == ".onnx" || ext == ".tflite" || ext == ".task" || ext == ".dll" || ext == ".json")
            paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(path, ec);
        const auto modified = std::filesystem::last_write_time(path, ec);
        if (!ec) stamps.push_back({path.lexically_relative(runtime).generic_string(), bytes, modified.time_since_epoch().count()});
    }
    configuration["_resource_stamp"] = std::move(stamps);
    return configuration.dump();
}

}

GLModel::Geometry build_semantic_colored_geometry(const SC::MeshSnapshot& source, const SC::FaceColors& paint,
                                                   const SC::SubfaceColors& subfaces, const SC::Cancel& cancel)
{
    GLModel::Geometry geometry;
    geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2};
    const auto& mesh = source.mesh;
    const auto normals = ModelPreviewNormals::corner_normals(mesh);
    std::vector<int> indices(mesh.indices.size(), -1);
    for (size_t i = 0; i < paint.size(); ++i)
        if (paint[i].first < indices.size()) indices[paint[i].first] = int(i);
    std::map<size_t, std::map<SC::SubfacePath, SC::Color>> children;
    for (const SC::SubfaceColor& item : subfaces) {
        if (item.face_id >= mesh.indices.size() || item.path.depth == 0 || item.path.depth > 3 ||
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
            const auto has_descendant = [&](const SC::SubfacePath& node) {
                for (const auto& item : child_colors->second) {
                    if (item.first.depth <= node.depth) continue;
                    const unsigned shift = 2u * unsigned(item.first.depth - node.depth);
                    if (node.depth == 0 || uint8_t(item.first.value >> shift) == node.value) return true;
                }
                return false;
            };
            std::function<void(SC::SubfacePath)> visit = [&](SC::SubfacePath node) {
                if (!has_descendant(node)) { leaves.push_back(node); return; }
                for (uint8_t child = 0; child < 4; ++child)
                    visit({uint8_t(node.depth + 1), uint8_t((node.value << 2) | child)});
            };
            visit({0, 0});
        }
        for (const SC::SubfacePath& leaf : leaves) {
            std::array<SC::Barycentric, 3> barycentric;
            if (leaf.depth == 0)
                barycentric = {{{1.f,0.f,0.f}, {0.f,1.f,0.f}, {0.f,0.f,1.f}}};
            else if (!SC::subface_vertices(leaf, barycentric)) return {};
            const SC::Color* leaf_color = nullptr;
            if (child_colors != children.end()) {
                auto found = child_colors->second.find(leaf);
                for (uint8_t depth = leaf.depth; found == child_colors->second.end() && depth > 1; --depth) {
                    const unsigned shift = 2u * unsigned(leaf.depth - depth + 1);
                    found = child_colors->second.find({uint8_t(depth - 1), uint8_t(leaf.value >> shift)});
                }
                if (found != child_colors->second.end()) leaf_color = &found->second;
            }
            if (leaf_color == nullptr && indices[f] >= 0) leaf_color = &paint[size_t(indices[f])].second;
            const unsigned base = unsigned(geometry.vertices_count());
            for (const SC::Barycentric& bary : barycentric) {
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
                const SC::Color rgb = leaf_color == nullptr
                    ? SC::Color {original[0], original[1], original[2]} : *leaf_color;
                geometry.add_vertex(position, normal, Vec2f(float(preview_rgb8(rgb[0], rgb[1], rgb[2])),
                    leaf_color == nullptr ? original[3] : -1.f));
            }
            geometry.add_triangle(base, base + 1, base + 2);
        }
    }
    return geometry;
}

struct ModelSemanticColoring::Impl {
    struct Request {
        std::shared_ptr<const Snapshot> source;
        std::vector<Color> palette, targets, portrait;
        FaceColors manual;
        std::string configuration;
        uint64_t generation {0};
        std::vector<SC::PaletteSlot> source_slots, target_slots;
        std::string slots_key;
        std::vector<SC::FaceSlotAssignment> manual_slots;
        std::vector<SC::RegionColorOverride> region_overrides;
        bool semantic_optimization {true};
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
    std::string boundary_override;
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
            const SC::Cancel cancel = [task] { return task->canceled.load(); };
            task->progress.store(2);
            const auto compose_materials = [&] {
                const auto& source = *task->request.source;
                        result->effective_manual = task->request.manual;
                        if (!task->request.target_slots.empty()) {
                            std::vector<const SC::PaletteSlot*> active;
                            std::vector<Color> labs;
                            for (const auto& slot : task->request.target_slots) if (slot.enabled) {
                                active.push_back(&slot); labs.push_back(PreviewPalette::to_lab(slot.color));
                            }
                            std::map<size_t, SC::FaceSlotAssignment> intents;
                            for (const auto& intent : task->request.manual_slots) intents[intent.face_id] = intent;
                            if (!active.empty()) for (auto& manual : result->effective_manual) {
                                auto it = intents.find(manual.first);
                                const SC::Color original = it == intents.end() ? manual.second : it->second.intended_color;
                                const SC::PaletteSlot* target = nullptr;
                                if (it != intents.end()) for (const auto* slot : active)
                                    if (slot->id == it->second.intended_slot_id) { target = slot; break; }
                                if (!target) {
                                    target = active[PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(original), labs)];
                                    if (it != intents.end()) ++result->substituted_manual_slots;
                                }
                                manual.second = target->color;
                                result->effective_manual_slots.push_back({manual.first, target->id,
                                    it == intents.end() ? target->id : it->second.intended_slot_id, original});
                            }
                        }
                        auto preview_paint = result->automatic;
                        if (!task->request.target_slots.empty()) {
                            std::vector<const SC::PaletteSlot*> active;
                            std::vector<Color> source_labs;
                            for (const auto& slot : task->request.source_slots) if (slot.enabled) {
                                const auto target = std::find_if(task->request.target_slots.begin(),task->request.target_slots.end(),
                                    [&](const auto& s) { return s.id == slot.id && s.enabled; });
                                if (target != task->request.target_slots.end()) {
                                    active.push_back(&*target); source_labs.push_back(PreviewPalette::to_lab(slot.color));
                                }
                            }
                            if (!active.empty()) {
                                const auto* semantic_analysis = result->analysis.get();
                                const SC::PaletteSlot* lip_target = nullptr;
                                const SC::PaletteSlot* skin_target = nullptr;
                                if (task->request.portrait.size() == 6) {
                                    for (const auto* slot : active) {
                                        if (slot->color == task->request.portrait[3]) lip_target = slot;
                                        if (slot->color == task->request.portrait[0]) skin_target = slot;
                                    }
                                }
                                std::vector<int> semantic(source.mesh.indices.size(), -1);
                                for (size_t i=0;i<result->slots.face_slots.size();++i)
                                    semantic[result->slots.face_slots[i].face_id]=int(i);
                                preview_paint.clear(); preview_paint.reserve(source.mesh.indices.size());
                                auto full_slots = result->slots.face_slots;
                                full_slots.reserve(source.mesh.indices.size());
                                for (size_t f=0;f<source.mesh.indices.size();++f) {
                                    const SC::PaletteSlot* target = nullptr;
                                    if (semantic[f]>=0) {
                                        const auto& assignment = result->slots.face_slots[semantic[f]];
                                        for (const auto* slot : active) if (slot->id == assignment.slot_id) { target=slot; break; }
                                    }
                                    if (!target) {
                                        Color rgb {};
                                        if (source.face_colors.size()==source.mesh.indices.size())
                                            for(size_t c=0;c<3;++c) rgb[c]=source.face_colors[f][c];
                                        else if(source.vertex_colors.size()==source.mesh.vertices.size())
                                            for(int v:source.mesh.indices[f]) for(size_t c=0;c<3;++c) rgb[c]+=source.vertex_colors[v][c]/3.f;
                                        target=active[PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(rgb),source_labs)];
                                        if (semantic_analysis && lip_target && skin_target && target == lip_target &&
                                            f < semantic_analysis->face_labels.size() &&
                                            (semantic_analysis->face_labels[f] == SC::Label::FaceSkin ||
                                             semantic_analysis->face_labels[f] == SC::Label::BodySkin ||
                                             semantic_analysis->face_labels[f] == SC::Label::Unknown))
                                            target = skin_target;
                                        full_slots.push_back({f,target->id,target->id,rgb});
                                    }
                                    preview_paint.emplace_back(f,target->color);
                                }
                                result->slots.face_slots=std::move(full_slots);
                                result->slots.faces=preview_paint;
                            }
                        }
                        if (!preview_paint.empty() || !result->automatic_subfaces.empty() || !result->effective_manual.empty())
                            result->geometry = build_semantic_colored_geometry(source,
                                SC::compose(preview_paint, result->effective_manual, true),
                                SC::compose_subfaces(result->automatic_subfaces, result->effective_manual, true), cancel);
            };
            try {
                if (!task->request.semantic_optimization) {
                    task->progress.store(90);
                    compose_materials();
                }
                else {
                if (!providers.body || !providers.face || !providers.boundary_error.empty() ||
                    providers_configuration != task->request.configuration) {
                    std::string body_id = "mediapipe.cpu.v1", face_id = body_id, boundary_id = "none";
                    const auto doc = nlohmann::json::parse(task->request.configuration);
                    body_id = doc.value("body_provider", body_id);
                    face_id = doc.value("face_provider", face_id);
                    boundary_id = doc.value("boundary_provider", boundary_id);
                    providers = SC::create_region_recognizers(body_id, face_id, boundary_id, runtime);
                    providers_configuration = task->request.configuration;
                    cached_analysis.reset();
                }
                if (!providers.error.empty()) throw std::runtime_error(providers.error);
                if (!providers.body || !providers.face) throw std::runtime_error("Local region recognizers unavailable");
                result->requested_boundary = nlohmann::json::parse(task->request.configuration).value("boundary_provider", std::string("none"));
                result->actual_boundary = providers.boundary ? providers.boundary->identity() : "none";
                result->boundary_fallback = providers.boundary_error;
                const auto& source = *task->request.source;
                const std::string boundary_identity = providers.boundary ? providers.boundary->identity() : "none";
                const auto key = SC::analysis_cache_key(source, providers.body->identity(),
                                                        providers.face->identity(), boundary_identity);
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
                        result->cache_hit = SC::decode_analysis(doc, source, providers.body->identity(),
                            providers.face->identity(), boundary_identity, *analysis, error);
                    }
                }
                if (result->cache_hit) task->progress.store(80);
                if (!result->cache_hit && !cancel()) {
                    *analysis = SC::analyze(source, *providers.body, *providers.face,
                        providers.boundary.get(), cancel,
                        [task](int value, const std::string&) { task->progress.store(2 + std::clamp(value, 0, 100) * 78 / 100); });
                    const bool transient_boundary_error = !providers.boundary_error.empty() ||
                        std::any_of(analysis->boundary_runs.begin(), analysis->boundary_runs.end(),
                            [](const auto& run) { return run.status == "error" || run.status == "canceled"; });
                    if (!analysis->canceled && analysis->error.empty() && !cancel() && !transient_boundary_error) {
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
                    task->progress.store(82);
                    result->analysis = analysis;
                    result->person_detected = analysis->person_detected;
                    result->error = analysis->error;
                    if (analysis->error.empty() && !analysis->canceled) {
                        const bool transient_boundary_error = !providers.boundary_error.empty() ||
                            std::any_of(analysis->boundary_runs.begin(), analysis->boundary_runs.end(),
                                [](const auto& run) { return run.status == "error" || run.status == "canceled"; });
                        if (!transient_boundary_error) cached_analysis = analysis;
                        if (!task->request.source_slots.empty() && analysis->person_detected) {
                            SC::SlotMappingResult original;
                            std::string mapping_error;
                            if (!SC::map_palette_slots(source, *analysis, task->request.source_slots,
                                    task->request.portrait, {}, task->request.region_overrides,
                                    original, mapping_error, cancel) ||
                                !SC::remap_palette_slots(original, task->request.target_slots,
                                    result->slots, mapping_error))
                                throw std::runtime_error(mapping_error);
                            result->automatic = result->slots.faces;
                            result->automatic_subfaces = result->slots.subfaces;
                            result->subface_added_triangles = result->slots.added_triangles;
                            result->subface_rejected_candidates = result->slots.rejected_candidates;
                        } else if (analysis->person_detected) {
                        const auto source_automatic = SC::map_palette(
                            source, *analysis, task->request.palette, task->request.portrait);
                        result->automatic = SC::remap_palette_targets(source_automatic,
                            task->request.palette, task->request.targets);
                        SC::SubfaceBudgetResult source_subfaces;
                        std::string subface_error;
                        if (!SC::map_subface_palette(source, *analysis, source_automatic, task->request.palette,
                                task->request.portrait, {}, source_subfaces, subface_error))
                            throw std::runtime_error(subface_error);
                        result->automatic_subfaces = SC::remap_subface_palette_targets(
                            source_subfaces.accepted, task->request.palette, task->request.targets);
                        result->subface_added_triangles = source_subfaces.added_triangles;
                        result->subface_rejected_candidates = source_subfaces.rejected_candidates;
                        }
                        task->progress.store(90);
                        compose_materials();
                    }
                }
                }
            } catch (const std::exception& error) { result->error = error.what(); }
              catch (...) { result->error = "Unknown local region recognition failure"; }
            result->elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            task->progress.store(99);
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
                                   std::vector<Color> portrait_card, FaceColors manual,
                                   std::vector<SC::PaletteSlot> source_slots, std::vector<SC::PaletteSlot> target_slots,
                                   std::vector<SC::FaceSlotAssignment> manual_slots, bool semantic_optimization,
                                   std::vector<SC::RegionColorOverride> region_overrides)
{
    auto& state = *m_impl;
    auto configuration = provider_configuration(state.runtime);
    if (!state.boundary_override.empty()) {
        auto doc = nlohmann::json::parse(configuration, nullptr, false);
        if (doc.is_object()) { doc["boundary_provider"] = state.boundary_override; configuration = doc.dump(); }
    }
    nlohmann::json slot_doc = nlohmann::json::array();
    for (const auto* entries : {&source_slots, &target_slots}) for (const auto& slot : *entries)
        slot_doc.push_back({slot.id, slot.color, slot.enabled});
    for (const auto& manual : manual_slots)
        slot_doc.push_back({manual.face_id, manual.intended_slot_id, manual.intended_color});
    for (const auto& region : region_overrides)
        slot_doc.push_back({region.analysis_signature, region.material_center_id, region.slot_id, region.locked});
    const std::string slots_key = slot_doc.dump();
    if (state.wanted && state.request.source == source && state.request.palette == palette &&
        state.request.targets == targets && state.request.portrait == portrait_card &&
        state.request.manual == manual && state.request.configuration == configuration &&
        state.request.slots_key == slots_key && state.request.semantic_optimization == semantic_optimization) return false;
    if (state.job) state.job->canceled.store(true);
    state.request = {std::move(source), std::move(palette), std::move(targets), std::move(portrait_card),
        std::move(manual), std::move(configuration), ++state.generation,
        std::move(source_slots), std::move(target_slots), slots_key, std::move(manual_slots),
        std::move(region_overrides), semantic_optimization};
    state.wanted = true; state.pending = true; state.start(); return true;
}
void ModelSemanticColoring::set_boundary_provider(std::string provider)
{
    if (m_impl->boundary_override == provider) return;
    cancel();
    m_impl->boundary_override = std::move(provider);
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
        if (result && (!result->error.empty() || !result->boundary_fallback.empty() ||
            (result->analysis && std::any_of(result->analysis->boundary_runs.begin(),result->analysis->boundary_runs.end(),
                [](const auto& run){return run.status=="error" || run.status=="canceled";})))) state.wanted=false;
    }
    state.start(); return result;
}
bool ModelSemanticColoring::busy() const { return m_impl->wanted && (m_impl->pending || bool(m_impl->job)); }
int ModelSemanticColoring::progress() const {
    const auto& state = *m_impl;
    if (state.pending || (state.job &&
        (state.job->canceled.load() || state.job->request.generation != state.generation))) return -1;
    return state.job ? state.job->progress.load() : 0;
}
} // namespace Slic3r::GUI
