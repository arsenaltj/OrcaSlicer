#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/AI/Contracts/ColorIntent.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitSuiteBinding.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PrinterFilamentPreview.hpp"
#include "slic3r/GUI/AI/Model/ColorTrialState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchTransactionController.hpp"
#include <boost/nowide/cstdlib.hpp>

namespace PS = Slic3r::GUI::PortraitSuite;
namespace P = Slic3r::GUI::PreviewPalette;
namespace SC = Slic3r::AI::SemanticColoring;
namespace {
const std::vector<P::Color> targets {{0,0,0}, {.686f,.431f,.369f}, {.925f,.765f,.698f}, {1,1,1}, {.655f,.647f,.643f}};
P::AutomaticColorBasis basis(size_t count) {
    P::AutomaticColorBasis b; b.geometry_id = std::string(64, 'a'); b.source_color_signature = "raw-original";
    b.requested_count = count; b.source_palette = targets;
    b.source_palette.push_back({.60f,.59f,.58f}); b.source_palette.resize(count);
    b.source_area.assign(count, 1); b.unit_area.assign(count, 1);
    for (size_t i = 0; i < count; ++i) b.unit_source_slots.push_back(i);
    b.basis_signature = P::automatic_basis_signature(b); return b;
}
PS::Constraints reliable(size_t count) {
    PS::Constraints c; c.reliable = true; c.labels.resize(count);
    const std::vector<SC::Label> labels {SC::Label::Hair, SC::Label::Lips, SC::Label::FaceSkin,
        SC::Label::EyeSclera, SC::Label::Clothes, SC::Label::Clothes};
    for (size_t i = 0; i < count; ++i) c.labels[i].insert(labels[i]);
    return c;
}
}
TEST_CASE("Portrait suites retain four five and six source regions while excluding support", "[PortraitSuiteBinding][ModelPreviewPalette]")
{
    for (size_t count : {4, 5, 6}) {
        DYNAMIC_SECTION("AI recommends " << count << " source colours") {
            const auto source = basis(count); const auto original = source.unit_source_slots;
            const auto result = PS::match(source, targets, reliable(count), "people-six", "v1");
            REQUIRE(result.reason.empty()); REQUIRE(result.binding.valid(count));
            CHECK(source.unit_source_slots == original);
            CHECK(result.binding.basis_signature == source.basis_signature);
            for (size_t slot : result.binding.source_to_model_slot) CHECK(slot < 5);
            const auto again = PS::match(source, targets, reliable(count), "people-six", "v1");
            CHECK(result.binding.source_to_model_slot == again.binding.source_to_model_slot);
            if (count <= 5) {
                const auto generic = P::minimum_cost_color_card_remap(source, targets);
                CHECK(result.binding.source_to_model_slot == generic.source_to_target_slot);
            } else {
                CHECK(result.binding.source_to_model_slot[4] == result.binding.source_to_model_slot[5]);
                CHECK_FALSE(P::minimum_cost_color_card_remap(source, targets).valid(count));
            }
        }
    }
}
TEST_CASE("Partial front evidence detects a portrait and preserves trustworthy facial leaves", "[PortraitSuiteBinding][SemanticRegionEvidence]")
{
    auto b = basis(6);
    b.unit_area = {0.2, 0.2633, 0.1, 0.1, 0.2, 0.1367};
    for (size_t i=0;i<6;++i) b.source_area[i] = b.unit_area[i];
    b.basis_signature = P::automatic_basis_signature(b);
    Slic3r::GUI::SemanticRegionEvidence evidence;
    evidence.geometry_id=b.geometry_id; evidence.source_content_id=std::string(64,'b');
    evidence.analysis_signature="analysis";evidence.body_identity="body";evidence.face_identity="face";evidence.runtime_identity="runtime";
    evidence.labels={SC::Label::FaceSkin,SC::Label::Hair,SC::Label::Unknown,SC::Label::Unknown,SC::Label::Unknown,SC::Label::Unknown};
    evidence.confidence={.95f,.95f,0,0,0,0};
    evidence.subfaces={{4,{2,3},SC::Label::Eyebrow,.9f,4}};
    REQUIRE(evidence.valid());
    const auto c=PS::constraints(b,&evidence,evidence.source_content_id);
    CHECK(c.person_detected); CHECK(c.reliable); CHECK(c.partial);
    CHECK_THAT(c.known_area_ratio,Catch::Matchers::WithinAbs(.4633,1e-8));
    CHECK_FALSE(c.known_boundary_allows_share(4,5));
    CHECK(c.labels[4].count(SC::Label::Eyebrow)==1);
    CHECK_FALSE(PS::constraints(b,&evidence,std::string(64,'c')).person_detected);
    std::string error;
    const auto restored=Slic3r::GUI::SemanticRegionEvidence::decode(evidence.encode(),b.geometry_id,6,"runtime",error);
    REQUIRE(restored); CHECK(restored->subfaces==evidence.subfaces);
    evidence.person_detected=false;
    CHECK_FALSE(PS::constraints(b,&evidence).reliable);
}
TEST_CASE("Six source regions cannot share critical features or skin garment boundaries", "[PortraitSuiteBinding]")
{
    auto c = reliable(6); auto source = basis(6);
    c.labels[5] = {SC::Label::BodySkin};
    auto result = PS::match(source, targets, c, "people-six", "v1");
    CHECK_FALSE(result.binding.valid(6)); CHECK_FALSE(result.reason.empty()); CHECK_FALSE(result.conflicts.empty());
    c.labels[4] = c.labels[5] = {SC::Label::Eyebrow};
    CHECK_FALSE(PS::match(source, targets, c, "people-six", "v1").binding.valid(6));
    c.labels[4] = {SC::Label::Clothes, SC::Label::FaceSkin}; c.labels[5] = c.labels[4];
    CHECK_FALSE(PS::match(source, targets, c, "people-six", "v1").binding.valid(6));
}
TEST_CASE("Unreliable semantics require explicit fallback and preserve the fallback label", "[PortraitSuiteBinding]")
{
    auto b = basis(6); PS::Constraints c;
    CHECK_FALSE(PS::match(b, targets, c, "people-six", "v1").binding.valid(6));
    const auto confirmed = PS::match(b, targets, c, "people-six", "v1", true);
    REQUIRE(confirmed.binding.valid(6)); CHECK(confirmed.binding.near_color_fallback);
    auto roundtrip = nlohmann::json(confirmed.binding).get<PS::Binding>();
    CHECK(roundtrip.near_color_fallback);
    CHECK(roundtrip.source_to_model_slot == confirmed.binding.source_to_model_slot);
}
TEST_CASE("Area weighted six to five assignment minimizes total source error", "[PortraitSuiteBinding]")
{
    auto b = basis(6); b.source_area[5] = 100;
    const auto result = PS::match(b, targets, reliable(6), "people-six", "v1");
    REQUIRE(result.binding.valid(6));
    double error = 0;
    for (size_t i = 0; i < 6; ++i) error += b.source_area[i] * double(P::distance(P::to_lab(b.source_palette[i]),
        P::to_lab(targets[result.binding.source_to_model_slot[i]])));
    CHECK_THAT(result.binding.weighted_error, Catch::Matchers::WithinAbs(error, 1e-10));
    // Independently enumerate all 5^6 assignments, enforcing the only safe
    // shared pair, instead of reproducing the production 5! search.
    double oracle = std::numeric_limits<double>::infinity();
    for (size_t encoded = 0; encoded < 15625; ++encoded) {
        auto value = encoded; std::vector<size_t> slots(6); std::set<size_t> used;
        for (auto& slot : slots) { slot = value % 5; value /= 5; used.insert(slot); }
        if (used.size() != 5 || slots[4] != slots[5]) continue;
        double candidate = 0;
        for (size_t i = 0; i < 6; ++i) candidate += b.source_area[i] * double(P::distance(
            P::to_lab(b.source_palette[i]), P::to_lab(targets[slots[i]])));
        oracle = std::min(oracle, candidate);
    }
    CHECK_THAT(result.binding.weighted_error, Catch::Matchers::WithinAbs(oracle, 1e-10));
}

TEST_CASE("Fallback preserves known protected boundaries and refuses distant source pairs", "[PortraitSuiteBinding]")
{
    auto b = basis(6); auto c = reliable(6); c.reliable = false;
    c.labels[5] = {SC::Label::MouthInterior};
    CHECK_FALSE(PS::match(b, targets, c, "people-six", "v1", true).binding.valid(6));
    c = reliable(6); b.source_palette[5] = {0, 0, 1};
    CHECK_FALSE(PS::match(b, targets, c, "people-six", "v1").binding.valid(6));
}
TEST_CASE("Saved suite bindings retain repeated target identities through filament binding", "[PortraitSuiteBinding][ColorTrialState][PrinterFilamentPreview]")
{
    const auto b = basis(6);
    const auto binding = PS::match(b, targets, reliable(6), "people-six", "v1").binding;
    namespace T = Slic3r::AI::ColorTrialPersistence;
    T::State state; state.source = 2; state.enabled = true; state.count = 6;
    state.mapping_colors = b.source_palette; state.automatic_basis_signature = b.basis_signature;
    state.suite_id = binding.suite_id; state.suite_revision = binding.suite_revision; state.suite_region_binding = binding;
    for (size_t slot : binding.source_to_model_slot) state.colors.push_back(targets[slot]);
    T::State reopened; std::string error;
    REQUIRE(T::decode(T::encode(state, 6, b.geometry_id), 6, b.geometry_id, reopened, error));
    REQUIRE(reopened.suite_region_binding.has_value());
    CHECK(reopened.suite_region_binding->source_to_model_slot == binding.source_to_model_slot);
    CHECK(reopened.source_to_target_slot.empty());
    REQUIRE(Slic3r::GUI::PrinterFilamentPreview::apply_bound(reopened, binding.source_to_model_slot, state.colors, "six-physical-slots"));
    CHECK(reopened.project_color_slots[4] == reopened.project_color_slots[5]);
    CHECK(reopened.suite_region_binding.has_value());
    const auto project = T::encode(reopened, 6, b.geometry_id);
    REQUIRE(T::decode(project, 6, b.geometry_id, reopened, error));
    CHECK(reopened.project_color_slots == binding.source_to_model_slot);
    using Transactions = Slic3r::GUI::BeautyWorkbenchTransactionController;
    Transactions transactions;
    const auto before = T::encode(state, 6, b.geometry_id);
    Transactions::Entry entry; entry.kind = Transactions::OperationKind::AppearanceRecolor;
    entry.restore_undo = [&] { return T::decode(before, 6, b.geometry_id, reopened, error); };
    entry.restore_redo = [&] { return T::decode(project, 6, b.geometry_id, reopened, error); };
    transactions.record(std::move(entry));
    REQUIRE(transactions.undo()); CHECK(reopened.project_color_slots.empty());
    REQUIRE(reopened.suite_region_binding.has_value());
    CHECK(reopened.mapping_colors == b.source_palette);
    REQUIRE(transactions.redo());
    CHECK(reopened.project_color_slots == binding.source_to_model_slot);
    CHECK(reopened.mapping_colors == b.source_palette);
    auto invalid = project; invalid["suite_region_binding"]["source_to_model_slot"][0] = 5;
    CHECK_FALSE(T::decode(invalid, 6, b.geometry_id, reopened, error));
}

TEST_CASE("Frozen portrait evidence retains partial detection on its original geometry", "[.][PortraitEvidenceReplay]")
{
    // nowide getenv may reuse its Windows conversion buffer on the next call.
    const auto environment_value = [](const char* key) {
        const auto* value = boost::nowide::getenv(key);
        return value ? std::string(value) : std::string{};
    };
    const auto source = environment_value("ORCA_PORTRAIT_REPLAY_MODEL");
    const auto input = environment_value("ORCA_PORTRAIT_REPLAY_EVIDENCE");
    const auto output = environment_value("ORCA_PORTRAIT_REPLAY_REPORT");
    if (source.empty() || input.empty() || output.empty()) SKIP("Select a frozen local model, evidence and a new report path.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    Slic3r::TriangleMesh mesh; Slic3r::ObjInfo colors; std::string error;
    const bool loaded = Slic3r::AI::load_model_artifact(source, mesh, colors, error);
    INFO(error); REQUIRE(loaded);
    REQUIRE(colors.vertex_colors.size() == mesh.its.vertices.size());
    boost::filesystem::ifstream stream(input); const auto doc = nlohmann::json::parse(stream);
    const auto geometry = Slic3r::AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    const auto evidence = Slic3r::GUI::SemanticRegionEvidence::decode(doc, geometry,
        mesh.its.indices.size(), doc.at("runtime_identity"), error);
    INFO(error); REQUIRE(evidence); REQUIRE(evidence->person_detected);
    P::Histogram histogram; P::AutomaticColorUnits units;
    for (const auto& face : mesh.its.indices) {
        const double area = double((mesh.its.vertices[face[1]] - mesh.its.vertices[face[0]])
            .cross(mesh.its.vertices[face[2]] - mesh.its.vertices[face[0]]).norm());
        P::Color mean {};
        for (size_t corner = 0; corner < 3; ++corner) {
            const auto& rgb = colors.vertex_colors[face[int(corner)]];
            uint32_t packed = 0;
            for (size_t channel = 0; channel < 3; ++channel) {
                const auto value = uint32_t(std::lround(std::clamp(rgb[channel], 0.f, 1.f) * 255.f));
                packed = (packed << 8) | value; mean[channel] += float(value) / (3.f * 255.f);
            }
            histogram.add(packed, area);
        }
        units.colors.push_back(mean); units.area.push_back(area * .5);
    }
    nlohmann::json outcomes = nlohmann::json::array();
    for (size_t count : {4, 5, 6}) {
        const auto b = P::make_automatic_color_basis(histogram, count, geometry, true, &units);
        REQUIRE(b.valid()); REQUIRE(b.source_palette.size() == count);
        const auto c = PS::constraints(b, evidence.get(), evidence->source_content_id);
        CHECK(c.person_detected); CHECK(c.reliable); CHECK(c.partial);
        CHECK_THAT(c.known_area_ratio, Catch::Matchers::WithinAbs(.46330382789, .00001));
        size_t eyebrows = 0;
        for (size_t slot = 0; slot < c.labels.size(); ++slot)
            if (c.labels[slot].count(SC::Label::Eyebrow)) {
                ++eyebrows; CHECK_FALSE(c.known_boundary_allows_share(slot, (slot + 1) % count));
            }
        CHECK(eyebrows > 0);
        const auto matched = PS::match(b, targets, c, "people-six", "frozen-replay");
        if (count < 6) REQUIRE(matched.binding.valid(count));
        if (matched.binding.valid(count)) {
            CHECK_FALSE(matched.binding.near_color_fallback);
            for (auto slot : matched.binding.source_to_model_slot) CHECK(slot < 5);
        } else CHECK_FALSE(matched.conflicts.empty());
        outcomes.push_back({{"count", count}, {"person_detected", c.person_detected}, {"partial", c.partial},
            {"reliable", c.reliable}, {"known_area_ratio", c.known_area_ratio}, {"eyebrow_source_slots", eyebrows},
            {"mapping", matched.binding.source_to_model_slot}, {"pause_reason", matched.reason}});
    }
    boost::filesystem::ofstream report(output);
    report << nlohmann::json{{"model_sha256", Slic3r::AI::model_artifact_sha256(source)},
        {"geometry_id", geometry}, {"faces", mesh.its.indices.size()}, {"subfaces", evidence->subfaces.size()},
        {"outcomes", outcomes}}.dump(2);
    report.close(); REQUIRE(bool(report));
}
