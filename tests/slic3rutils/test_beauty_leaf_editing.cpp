#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/Model/BeautyLeafEditing.hpp"
#include "slic3r/GUI/AI/Model/BeautyLeafEdits.hpp"

using namespace Slic3r;
using namespace Slic3r::AI;
namespace {
struct Fixture {
    indexed_triangle_set mesh;
    ShapeLockSet locks;
    std::shared_ptr<VertexColorRegionEditor> canonical;
    std::shared_ptr<BeautyLeafEditing> editing;
    Fixture() {
        mesh.vertices = {Vec3f(0,0,0),Vec3f(1,0,0),Vec3f(0,1,0),Vec3f(1,1,0)};
        mesh.indices = {{0,1,2},{1,3,2}};
        for (int i = 2; i < 200; ++i) {
            const auto base = int(mesh.vertices.size());
            mesh.vertices.insert(mesh.vertices.end(),{Vec3f(float(i)*10,0,0),Vec3f(float(i)*10+1,0,0),Vec3f(float(i)*10,1,0)});
            mesh.indices.emplace_back(base,base+1,base+2);
        }
        locks.geometry_id = SurfaceSelectionPersistence::geometry_fingerprint(mesh);
        locks.face_count = mesh.indices.size(); locks.source_sha256 = std::string(64,'a'); locks.evidence_sha256 = std::string(64,'b');
        locks.runtime_sha256 = std::string(64,'c'); locks.policy_sha256 = std::string(64,'d');
        locks.baseline_sha256 = std::string(64,'e'); locks.boundary_policy_sha256 = std::string(64,'f');
        locks.leaf_domain = BeautyLeafDomain{locks.geometry_id,locks.face_count,{{0,1,0},{0,1,1},{0,1,2},{0,1,3}}};
        ShapeLock eye; eye.subject_id = "person"; eye.label = eye.parent_label = "le"; eye.status = "VALID_SHAPE"; eye.view_support = 2;
        eye.locked_faces = eye.nested_faces = {0}; eye.locked_leaves = {{0,1,0},{0,1,3}}; eye.nested_leaves = {{0,1,3}};
        locks.locks = {eye};
        canonical = std::make_shared<VertexColorRegionEditor>(); std::string error;
        REQUIRE(canonical->initialize(mesh,std::vector<RGBA>(mesh.vertices.size(),RGBA{.9f,.8f,.7f,1.f}),error));
        editing = BeautyLeafEditing::build(locks,canonical,std::vector<RGBA>(mesh.indices.size(),RGBA{.9f,.8f,.7f,1.f}));
    }
};
}
TEST_CASE("Derived picking returns barycentric source leaves without changing the canonical mesh", "[BeautyLeafEditing]") {
    Fixture f;
    const auto hit = f.editing->editor->pick_surface(Vec3d(.1,.1,1),Vec3d(0,0,-1));
    REQUIRE(hit.has_value());
    REQUIRE(f.editing->hit_key(*hit) == BeautyLeafKey{0,1,0});
    REQUIRE(f.canonical->mesh().indices.size() == 200);
    REQUIRE(SurfaceSelectionPersistence::geometry_fingerprint(f.canonical->mesh()) == f.locks.geometry_id);
    REQUIRE(f.editing->surface->face_neighbors[4].size() == 2);
}
TEST_CASE("Parent selections exclude locked leaves while nested eye selections stay independent", "[BeautyLeafEditing]") {
    Fixture f;
    SurfaceSelectionPersistence::SelectionState roots;
    roots.selected.assign(200,0); roots.selected[0] = roots.selected[1] = 1;
    auto selected = f.editing->expand(roots); f.editing->constrain(selected,true);
    REQUIRE(selected.selected[0] == 0); REQUIRE(selected.selected[1] == 1); REQUIRE(selected.selected[3] == 0);
    REQUIRE(f.editing->collapse(selected).selected[0] == 0);
}
TEST_CASE("Derived lock pieces reject mixed recoloring and preserve every sibling boundary", "[BeautyLeafEditing][BeautyPuzzle]") {
    Fixture f;
    auto puzzle = BeautyPuzzle::create(*f.editing->surface);
    f.editing->editing_locks.isolate(puzzle,*f.editing->surface);
    REQUIRE(puzzle.face_piece[0] != puzzle.face_piece[3]);
    REQUIRE(f.editing->colorable(puzzle,puzzle.face_piece[0]));
    REQUIRE_FALSE(f.editing->boundary_segments(puzzle.face_piece).empty());
    REQUIRE_FALSE(f.editing->colorable(puzzle,puzzle.face_piece[1]));
    auto changed = puzzle; changed.face_piece[1] = changed.face_piece[0];
    REQUIRE_FALSE(f.editing->editing_locks.preserves(puzzle,changed));
    REQUIRE_FALSE(f.editing->colorable(changed,changed.face_piece[0]));
    changed = puzzle; changed.colors[puzzle.face_piece[0]] = {.1f,.2f,.3f,1};
    REQUIRE(f.editing->editing_locks.preserves(puzzle,changed));
}
TEST_CASE("Derived editors reject foreign geometry before assigning leaf identities", "[BeautyLeafEditing]") {
    Fixture f; f.locks.geometry_id = std::string(64,'9');
    REQUIRE_THROWS(BeautyLeafEditing::build(f.locks,f.canonical,std::vector<RGBA>(200,RGBA{1,1,1,1})));
}
TEST_CASE("Parent leaf partitions retain exact feature identity through draft history and manual color restoration", "[BeautyLeafEditing][PortraitSurfaceOwnership]") {
    Fixture f;
    // Expand only an unlocked root, preserving every reviewed feature leaf.
    for (int i=200;i<500;++i) {
        const auto base=int(f.mesh.vertices.size());
        f.mesh.vertices.insert(f.mesh.vertices.end(),{Vec3f(float(i)*10,0,0),Vec3f(float(i)*10+1,0,0),Vec3f(float(i)*10,1,0)});
        f.mesh.indices.emplace_back(base,base+1,base+2);
    }
    f.locks.geometry_id=SurfaceSelectionPersistence::geometry_fingerprint(f.mesh); f.locks.face_count=500;
    f.locks.leaf_domain->canonical_geometry_id=f.locks.geometry_id; f.locks.leaf_domain->source_face_count=500;
    f.canonical=std::make_shared<VertexColorRegionEditor>(); std::string error;
    REQUIRE(f.canonical->initialize(f.mesh,std::vector<RGBA>(f.mesh.vertices.size(),RGBA{1,1,1,1}),error));
    auto combined=*f.locks.leaf_domain;
    for(uint8_t i=0;i<4;++i) combined.split_leaves.push_back({1,1,i});
    const auto editing=BeautyLeafEditing::build(f.locks,f.canonical,std::vector<RGBA>(500,RGBA{1,1,1,1}),&combined);
    SurfaceSelectionPersistence::SelectionState selection;
    selection.selected.assign(editing->keys.size(),0); selection.selected[editing->index({1,1,0})]=1;
    const SemanticColoring::Color manual{0,1,1};
    const auto draft=BeautyLeafEdits::capture(*editing,f.locks,{{{1,1,0},manual}},selection);
    const auto reopened=BeautyLeafEdits::decode(draft.encode(*editing,f.locks),*editing,f.locks);
    REQUIRE(reopened.colors==draft.colors); REQUIRE(reopened.selection.selected==selection.selected);
    REQUIRE(editing->owners[editing->index({0,1,0})]==0);
    auto before=BeautyPuzzle::create(*editing->surface);editing->editing_locks.isolate(before,*editing->surface);
    auto after=before;after.face_piece[editing->index({1,1,0})]=after.face_piece[editing->index({0,1,0})];
    REQUIRE_FALSE(editing->editing_locks.preserves(before,after));
    auto changed=f.locks;changed.boundary_policy_sha256=std::string(64,'1');
    REQUIRE_THROWS(BeautyLeafEdits::capture(*editing,changed,draft.colors,selection));
}
TEST_CASE("Exact leaf drafts round trip partial selections and manual colors without promoting a root", "[BeautyLeafEditing]") {
    Fixture f;
    SurfaceSelectionPersistence::SelectionState selection;
    selection.selected.assign(f.editing->keys.size(),0);
    selection.protected_faces=selection.foreground=selection.domain=selection.selected;
    selection.selected[0]=1; selection.protected_faces[1]=1;
    const SemanticColoring::Color rgb{.2f,.3f,.4f};
    const auto draft=BeautyLeafEdits::capture(*f.editing,f.locks,{{{0,1,0},rgb}},selection);
    const auto value=draft.encode(*f.editing,f.locks);
    const auto decoded=BeautyLeafEdits::decode(value,*f.editing,f.locks);
    REQUIRE(decoded.selection.selected==selection.selected);
    REQUIRE(decoded.selection.protected_faces==selection.protected_faces);
    REQUIRE(decoded.colors==draft.colors);
    SemanticColoring::FaceColors roots{{0,{.9f,.8f,.7f}}};
    SemanticColoring::SubfaceColors children{{0,{2,0},{1,0,0},1},{0,{1,1},{0,1,0},1}};
    compose_leaf_colors(roots,children,decoded.colors);
    REQUIRE(roots.front().second==SemanticColoring::Color{.9f,.8f,.7f});
    REQUIRE(children.size()==2);
    REQUIRE(children.front().path==SemanticColoring::SubfacePath{1,0});
    REQUIRE(children.front().color==rgb);
    auto drift=value; drift["mapping_sha256"]=std::string(64,'0');
    REQUIRE_THROWS(BeautyLeafEdits::decode(drift,*f.editing,f.locks));
    drift=value; drift["colors"].push_back(drift["colors"][0]);
    REQUIRE_THROWS(BeautyLeafEdits::decode(drift,*f.editing,f.locks));
    drift=value; drift["selected"]={{0,2,0}};
    REQUIRE_THROWS(BeautyLeafEdits::decode(drift,*f.editing,f.locks));
    auto foreign=f.locks;
    foreign.leaf_domain->split_leaves.clear();
    REQUIRE_THROWS(BeautyLeafEdits::capture(*f.editing,foreign,draft.colors,selection));
    selection.protected_faces.clear(); selection.foreground.clear(); selection.domain.clear();
    REQUIRE(BeautyLeafEdits::capture(*f.editing,f.locks,draft.colors,selection).selection.protected_faces.size()==f.editing->keys.size());
}
TEST_CASE("Primary slot edits preserve locked leaf colors and leave the unlocked underlayer editable", "[BeautyLeafEditing]") {
    Fixture f;
    const SemanticColoring::Color skin{.9f,.8f,.7f}, dark{.1f,.1f,.1f},warm{1.f,.5f,.5f},manual{.2f,.3f,.4f};
    SemanticColoring::FaceColors before{{0,skin}},after{{0,warm}};
    SemanticColoring::SubfaceColors children_before{{0,{1,0},dark,1},{0,{1,3},dark,1}};
    SemanticColoring::SubfaceColors children_after{{0,{1,0},warm,1},{0,{1,1},warm,1},{0,{1,3},warm,1}};
    preserve_locked_leaf_colors(f.locks,before,children_before,after,children_after);
    REQUIRE(after.front().second==warm);
    REQUIRE(children_after[0].color==dark);
    REQUIRE(children_after[1].color==warm);
    REQUIRE(children_after[2].color==dark);
    compose_leaf_colors(after,children_after,{{{0,1,0},manual}});
    REQUIRE(children_after[0].color==manual);
    REQUIRE(children_after[2].color==dark);
}
TEST_CASE("Sparse locked underlayers restore descendants without assigning an unrelated root", "[BeautyLeafEditing]") {
    Fixture f;
    const SemanticColoring::Color red{1.f,0.f,0.f},blue{0.f,0.f,1.f};
    SemanticColoring::FaceColors before,after{{0,blue}};
    SemanticColoring::SubfaceColors previous{{0,{2,0},red,.7f},{0,{2,12},red,.6f}};
    SemanticColoring::SubfaceColors changed{{0,{1,0},blue,1},{0,{2,0},blue,1},{0,{1,1},blue,1},{0,{2,12},blue,1}};
    preserve_locked_leaf_colors(f.locks,before,previous,after,changed);
    REQUIRE(after.empty());
    REQUIRE(changed.size()==3);
    REQUIRE(changed[0].path==SemanticColoring::SubfacePath{1,1});
    REQUIRE(changed[0].color==blue);
    REQUIRE(changed[1].color==red);
    REQUIRE(changed[1].confidence==.7f);
    REQUIRE(changed[2].color==red);
    REQUIRE(changed[2].confidence==.6f);
}
