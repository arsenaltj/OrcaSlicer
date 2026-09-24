#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Model/BeautyEyeDetail.hpp"
#include "slic3r/GUI/AI/Model/BeautyGuidance.hpp"
#include "slic3r/GUI/AI/Model/BeautyRecognition.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticGeometry.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include "libslic3r/FilamentMixer.hpp"
#include "slic3r/GUI/NativeMixedFilamentSuggestion.hpp"
#include <limits>
#include <numeric>
#include <cstring>
#include <chrono>

using namespace Slic3r;
using namespace Slic3r::AI;

TEST_CASE("Region mix targets use area weighted original faces regardless of saved paint", "[BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);const auto surface=BeautySurface::build(mesh,{});
    BeautyPuzzle p;p.geometry_id=surface->geometry_id;p.next_id=3;p.face_piece.assign(mesh.indices.size(),1);p.face_piece[0]=p.face_piece[1]=2;
    surface->areas[0]=1;surface->areas[1]=3;
    std::vector<RGBA> source(mesh.indices.size(),RGBA{0,1,0,1});source[0]={1,0,0,1};source[1]={0,0,1,1};
    p.colors[2]={0,0,0,1};const auto before=p.encode();
    const auto target=p.source_region_color(2,*surface,source);
    CHECK_THAT(target[0],Catch::Matchers::WithinAbs(.25,1e-6));CHECK_THAT(target[2],Catch::Matchers::WithinAbs(.75,1e-6));
    CHECK(target[1]==0);CHECK(p.encode()==before);
    CHECK_THROWS(p.source_region_color(3,*surface,source));source.pop_back();CHECK_THROWS(p.source_region_color(2,*surface,source));
}

TEST_CASE("Unmatched edit-region paint preserves untouched original texture until explicit filament matching", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    BeautyPuzzle puzzle;puzzle.geometry_id=surface->geometry_id;
    puzzle.face_piece.assign(surface->areas.size(),1);puzzle.next_id=2;
    const auto before=puzzle;
    puzzle.paint_faces_color({0,1},*surface,{1.f,1.f,1.f,1.f});
    CHECK(puzzle.palette.empty());CHECK(puzzle.filament_slots.empty());
    for(size_t face=0;face<puzzle.face_piece.size();++face) {
        const bool painted=puzzle.colors.count(puzzle.face_piece[face])!=0;
        CHECK(painted==(face<2));
    }
    CHECK(puzzle.face_piece!=before.face_piece);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    const auto restored=BeautyPuzzle::decode(puzzle.encode(),puzzle.geometry_id,puzzle.face_piece.size());
    CHECK(restored.same_edit(puzzle));
    auto matched=restored;
    matched.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}});
    CHECK(matched.filament_slots.size()==matched.piece_count());
    CHECK(before.colors.empty());
}

TEST_CASE("Native region proposals improve available colors without mixing material types", "[BeautyPuzzle]") {
    const std::vector<std::string> colors{"#F7E2DA","#282629","#F6F7F9","#EA9A92","#668CB6","#958B86"};
    auto types=std::vector<std::string>(6,"PLA");
    const auto result=GUI::suggest_native_mixed_filament("#BB6A65",colors,{},types);
    REQUIRE(result.valid);REQUIRE(result.components.size()>=2);REQUIRE(result.components.size()<=3);
    int total=0;for(const auto& c:result.components){CHECK(c.filament_index>=1);CHECK(c.filament_index<=6);CHECK(c.ratio>0);total+=c.ratio;}
    CHECK(total==100);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("#BB6A65",colors,{},types,result.matched_color_hex).valid);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("#926247",colors,{},types,"#907273").valid);
    CHECK_FALSE(GUI::suggest_native_mixed_filament(colors[0],colors,{},types).valid);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("invalid",colors,{},types).valid);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("#BB6A65",colors,{},{}).valid);
    for(size_t i=0;i<types.size();++i)types[i]="material-"+std::to_string(i);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("#BB6A65",colors,{},types).valid);
    types[2]=types[5]="PLA";
    const auto same=GUI::suggest_native_mixed_filament("#BB6A65",colors,{},types);
    for(const auto& c:same.components)CHECK((c.filament_index==3 || c.filament_index==6));
}

TEST_CASE("Explicit region colors receive native mix proposals without creating slots", "[.][NativeRegionMixProbe]") {
    const auto env=[](const char* key){const auto p=boost::nowide::getenv(key);return p?std::string(p):std::string{};};
    const auto input=env("ORCA_REGION_MIX_INPUT"),output=env("ORCA_REGION_MIX_OUTPUT");
    if(input.empty() || output.empty())SKIP("Explicit input and fresh output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    boost::filesystem::ifstream file(input);nlohmann::json cases;file>>cases;nlohmann::json results=nlohmann::json::array();
    for(const auto& item:cases) {
        const auto result=GUI::suggest_native_mixed_filament(item.at("target"),item.at("colors"),{},item.at("types"),item.value("current",std::string{}));
        nlohmann::json components=nlohmann::json::array();
        for(const auto& c:result.components)components.push_back({c.filament_index,c.ratio});
        results.push_back({{"sample",item.at("sample")},{"label",item.at("label")},{"target",item.at("target")},
            {"valid",result.valid},{"color",result.matched_color_hex},{"components",components}});
    }
    boost::filesystem::ofstream result(output);result<<results.dump(2);
}

TEST_CASE("Recognition distinguishes a completed empty result from a missing or failed attempt", "[BeautyWorkbench][BeautyGuidance]") {
    BeautyGuidance hints;CHECK_FALSE(hints.completed(3));
    hints.labels={-1,-1,-1};CHECK(hints.completed(3));CHECK_FALSE(hints.has_features());
    auto saved=hints.encode("geometry","source");
    CHECK(BeautyGuidance::decode(saved,"geometry","source",3).completed(3));
    saved["schema"]="orca.beauty-guidance/v1";saved.erase("names");
    CHECK(BeautyGuidance::decode(saved,"geometry","source",3).completed(3));
    const nlohmann::json failed={{"geometry","g"},{"source","s"},{"state","failed"},{"time",1000}};
    CHECK_FALSE(beauty_recognition_due(failed,"g","s",1100));
    CHECK(beauty_recognition_due(failed,"g","s",1300));
    CHECK(beauty_recognition_due(failed,"g","other",1100));
    CHECK(beauty_recognition_due(nlohmann::json{},"g","s",1100));
}

TEST_CASE("Only unchanged automatic partitions and exact filament assignments qualify for upgrade", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    auto automatic=BeautyPuzzle::create(*surface,4);
    automatic.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}});
    auto saved=automatic;saved.colors.clear();saved.filament_slots.clear();
    for(auto& id:saved.face_piece)id+=100;
    for(const auto& c:automatic.colors)saved.colors[c.first+100]=c.second;
    for(const auto& s:automatic.filament_slots)saved.filament_slots[s.first+100]=s.second;
    CHECK(same_automatic_puzzle(saved,automatic));
    saved.filament_slots.begin()->second=1-saved.filament_slots.begin()->second;
    CHECK_FALSE(same_automatic_puzzle(saved,automatic));
    saved=automatic;saved.face_piece.front()=saved.next_id++;
    CHECK_FALSE(same_automatic_puzzle(saved,automatic));
}

TEST_CASE("Small semantic pieces match their own source colors instead of a surrounding patch average", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);const auto surface=BeautySurface::build(mesh,{});
    BeautyPuzzle p;p.geometry_id=surface->geometry_id;p.next_id=3;p.face_piece.assign(mesh.indices.size(),1);
    p.face_piece.back()=2;std::vector<RGBA> colors(mesh.indices.size(),RGBA{1,1,1,1});colors.back()={0,0,0,1};
    p.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}}, {},colors);
    CHECK(p.filament_slots.at(1)==0);CHECK(p.filament_slots.at(2)==1);
    p.paint_filament(2,0);p.match_filaments(*surface,p.palette,{},colors);
    CHECK(p.filament_slots.at(2)==0); // New evidence cannot overwrite deliberate paint.
}

TEST_CASE("Dark source colors retain lightness when a matching native mixture is available", "[BeautyWorkbench][BeautyPuzzle]") {
    BeautyPuzzle p;p.palette={{0,"#F7E2DA","PLA",true},{1,"#958B86","PLA",true},{2,"#EA9A92","PLA",true}};
    const MixedColorRecipe brown{"#684331",{{0,.3},{1,.7}},6,std::string(64,'a'),true};
    const auto source=BeautyPuzzle::filament_color({0,brown.target_color,{},true});
    CHECK(p.nearest_filament(source,{brown})==6);
    CHECK(p.nearest_filament(BeautyPuzzle::filament_color(p.palette[2]),{brown})==2);
    // A physical match wins a tie, preventing gratuitous mixed-slot changes.
    auto duplicate=brown;duplicate.target_color=p.palette[1].display_color;
    CHECK(p.nearest_filament(BeautyPuzzle::filament_color(p.palette[1]),{duplicate})==1);
    auto invalid=brown;invalid.components[0].slot=99;
    CHECK(p.nearest_filament(source,{invalid})==p.nearest_filament(source));
    CHECK(p.nearest_filament(source,{brown,brown})==p.nearest_filament(source));
    auto gradient=brown;gradient.uniform_color=false;
    CHECK(p.nearest_filament(source,{gradient})==p.nearest_filament(source));
}

TEST_CASE("Fresh automatic color uses existing mixtures while saved and manual slots remain unchanged", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    const std::vector<PhysicalFilamentChannel> physical{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}};
    const MixedColorRecipe mix{"#808080",{{0,.4},{1,.6}},2,std::string(64,'a'),true};
    auto p=BeautyPuzzle::create(*surface,1);
    const std::vector<RGBA> source(surface->areas.size(),BeautyPuzzle::filament_color({0,mix.target_color,{},true}));
    p.match_filaments(*surface,physical,{mix},source);
    REQUIRE(p.mixed_recipes.size()==1);CHECK(same_native_mixed_recipe(p.mixed_recipes.front(),mix));
    for(const auto& slot:p.filament_slots)CHECK(slot.second==2);
    const auto saved=p.encode();auto restored=BeautyPuzzle::decode(saved,p.geometry_id,p.face_piece.size());
    restored.match_filaments(*surface,physical,{mix},source);CHECK(restored.same_edit(p));
    restored.paint_filament(restored.face_piece.front(),0);const auto manual=restored;
    restored.match_filaments(*surface,physical,{mix},source);CHECK(restored.same_edit(manual));
    restored=p;auto changed=mix;changed.native_settings_fingerprint=std::string(64,'b');
    restored.match_filaments(*surface,physical,{changed},source);
    for(const auto& slot:restored.filament_slots)CHECK(slot.second!=2);
    CHECK(restored.mixed_recipes.empty());
    auto bad=mix;bad.existing_virtual_slot=0;
    auto fresh=BeautyPuzzle::create(*surface,1);
    CHECK_NOTHROW(fresh.match_filaments(*surface,physical,{bad},source));
    CHECK(fresh.mixed_recipes.empty());
}

TEST_CASE("Automatic facial color keeps source contrast without changing saved paint or disconnected parts", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);const auto surface=BeautySurface::build(mesh,{});
    BeautyPuzzle p;p.geometry_id=surface->geometry_id;p.next_id=3;p.face_piece.assign(mesh.indices.size(),1);p.face_piece[0]=2;
    BeautyGuidance g;g.names={"face","lb"};g.labels.assign(mesh.indices.size(),0);g.labels[0]=1;
    std::vector<RGBA> source(mesh.indices.size(),RGBA{.6f,.6f,.6f,1});source[0]={.4f,.4f,.4f,1};
    const std::vector<PhysicalFilamentChannel> palette{{0,"#808080","PLA",true},{1,"#404040","PLA",true}};
    SECTION("adjacent darker eyebrow stays darker with bounded color deviation") {
        const auto faces=p.face_piece;
        CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==1);
        CHECK(p.filament_slots.at(1)==0);CHECK(p.filament_slots.at(2)==1);CHECK(p.face_piece==faces);
        auto saved=BeautyPuzzle::decode(p.encode(),p.geometry_id,p.face_piece.size());const auto before=saved;
        CHECK(beauty_match_feature_filaments(saved,*surface,g,source,palette)==0);CHECK(saved.same_edit(before));
    }
    SECTION("manual color is never adjusted") {
        p.match_filaments(*surface,palette,{},source);p.paint_filament(2,0);const auto before=p;
        CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);CHECK(p.same_edit(before));
    }
    SECTION("no original contrast does not invent a dark eyebrow") {
        source[0]=source[1];CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);
        CHECK(p.filament_slots.at(2)==p.filament_slots.at(1));
    }
    SECTION("already readable contrast is not washed out to fit an exact difference") {
        source[0]={.3f,.3f,.3f,1};
        CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);CHECK(p.filament_slots.at(2)==1);
    }
    SECTION("an unlabeled contour rim does not hide an otherwise unambiguous feature") {
        p.face_piece[1]=2;source[1]=source[0];g.labels[1]=-1;
        CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==1);CHECK(p.filament_slots.at(2)==1);
    }
    SECTION("a disconnected feature cannot borrow another face color") {
        for(auto& neighbors:surface->face_neighbors)for(auto& n:neighbors)if(n==0)n=-1;
        surface->face_neighbors[0]={-1,-1,-1};CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);
        CHECK(p.filament_slots.at(2)==0);
    }
    SECTION("missing recognition retains ordinary matching") {
        g.labels.clear();CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);CHECK(p.filament_slots.at(2)==0);
    }
    SECTION("invalid native slot collision cannot override a physical candidate") {
        const MixedColorRecipe bad{"#404040",{{0,.5},{1,.5}},1,std::string(64,'a'),true};
        CHECK_NOTHROW(beauty_match_feature_filaments(p,*surface,g,source,palette,{bad}));CHECK(p.mixed_recipes.empty());
    }
}

// Read-only experiment: rank native mixer colors for supplied local evidence.
// Proposals never create native slots or represent calibrated printed colors.
TEST_CASE("Explicit portrait evidence ranks existing native mixer combinations", "[.][NativePortraitMixProbe]") {
    const auto env=[](const char* key){const auto value=boost::nowide::getenv(key);return value?std::string(value):std::string();};
    const auto source=env("ORCA_PORTRAIT_GROUPS"),output=env("ORCA_PORTRAIT_MIX_OUTPUT");
    if(source.empty() || output.empty())SKIP("Explicit groups and new output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    boost::filesystem::ifstream input(source);nlohmann::json root;input>>root;
    const auto& saved=root.at("puzzle");
    auto puzzle=BeautyPuzzle::decode(saved,saved.at("geometry_id"),saved.at("face_count"));
    std::map<std::string,nlohmann::json> largest;
    for(const auto& group:root.at("groups")) {
        const std::string label=group.at("label");
        if(!largest.count(label) || group.at("area").get<double>()>largest[label].at("area").get<double>())largest[label]=group;
    }
    nlohmann::json results=nlohmann::json::array();
    for(const auto& entry:largest) {
        const auto target=entry.second.at("rgb").get<std::array<double,3>>();
        std::vector<std::pair<double,nlohmann::json>> ranked;
        auto add=[&](std::vector<size_t> slots,std::vector<int> weights) {
            std::vector<std::string> colors;for(size_t i:slots)colors.push_back(puzzle.palette[i].display_color);
            const auto hex=blend_color_multi(colors,weights);const auto rgb=BeautyPuzzle::filament_color({0,hex,{},true});
            const double error=tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(target,{rgb[0],rgb[1],rgb[2]});
            for(auto& i:slots)i=puzzle.palette[i].slot+1;
            ranked.push_back({error,{{"components_1based",slots},{"ratios_percent",weights},{"color",hex},{"delta_e",error}}});
        };
        for(size_t a=0;a<puzzle.palette.size();++a)for(size_t b=a+1;b<puzzle.palette.size();++b) {
            for(int x=10;x<=90;x+=10)add({a,b},{x,100-x});
            for(size_t c=b+1;c<puzzle.palette.size();++c)
                for(int x=10;x<=80;x+=10)for(int y=10;x+y<=90;y+=10)add({a,b,c},{x,y,100-x-y});
        }
        std::stable_sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.first<b.first;});
        nlohmann::json candidates=nlohmann::json::array();
        for(size_t i=0;i<std::min(size_t(5),ranked.size());++i)candidates.push_back(ranked[i].second);
        results.push_back({{"label",entry.first},{"source_rgb",target},{"candidates",candidates}});
    }
    boost::filesystem::ofstream result(output);result<<results.dump(2);CHECK_FALSE(results.empty());
}

TEST_CASE("Supplementing recognized features preserves paint outside the requested feature faces", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);const auto surface=BeautySurface::build(mesh,{});
    auto p=BeautyPuzzle::create(*surface,1);p.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}});
    p.paint_filament(p.face_piece.back(),1);
    BeautyGuidance g;g.labels.assign(mesh.indices.size(),-1);g.labels[0]=0;g.names={"llip"};
    auto next=beauty_supplement_features(p,*surface,g);
    for(size_t f=1;f<p.face_piece.size();++f)CHECK(next.colors.at(next.face_piece[f])==p.colors.at(p.face_piece[f]));
    CHECK_FALSE(next.colors.count(next.face_piece[0]));next.validate(*surface);
    CHECK(p.face_piece[0]!=next.face_piece[0]);
}

TEST_CASE("History beauty records win over stale adjacent drafts without capturing external models", "[BeautyWorkbench][BeautyMetadata]") {
    const auto root=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("beauty-record-%%%%-%%%%");
    boost::filesystem::create_directories(root/"library"/"downloads");
    boost::filesystem::create_directory(root/"library"/"job");
    boost::filesystem::create_directory(root/"external");
    struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code ec;boost::filesystem::remove_all(path,ec);}} cleanup{root};
    const auto model=root/"library"/"job"/"orcaslicer-ai-finish-test.glb";
    boost::filesystem::ofstream(model)<<"model";
    auto adjacent=model;adjacent.replace_extension(".json");
    boost::filesystem::ofstream(adjacent)<<"stale draft";
    CHECK(beauty_metadata_path(model,root/"library")==adjacent);
    const auto saved=root/"library"/"downloads"/adjacent.filename();
    boost::filesystem::ofstream(saved)<<"saved workbench";
    CHECK(beauty_metadata_path(model,root/"library")==saved);
    const auto external=root/"external"/model.filename();
    boost::filesystem::ofstream(external)<<"another model";
    auto external_metadata=external;external_metadata.replace_extension(".json");
    CHECK(beauty_metadata_path(external,root/"library")==external_metadata);
}

TEST_CASE("Saved recognition aids restore without inference and reject foreign or invalid surfaces", "[BeautyWorkbench][BeautyGuidance]") {
    BeautyGuidance hints;hints.labels={-1,0,0,1,1,1};hints.details={{1,2}};
    hints.eyes.push_back({"face-1","le",{0,1,2},{1,2}});
    const auto saved=hints.encode("geometry","original");
    const auto restored=BeautyGuidance::decode(saved,"geometry","original",6);
    CHECK(restored.labels==hints.labels);CHECK(restored.details==hints.details);
    REQUIRE(restored.eyes.size()==1);CHECK(restored.eyes.front().iris_faces==hints.eyes.front().iris_faces);
    CHECK_THROWS(BeautyGuidance::decode(saved,"other","original",6));
    CHECK_THROWS(BeautyGuidance::decode(saved,"geometry","changed-texture",6));
    auto invalid=saved;invalid["labels"][0][1]=7;
    CHECK_THROWS(BeautyGuidance::decode(invalid,"geometry","original",6));
    invalid=saved;invalid["eyes"][0]["iris"]={4};
    CHECK_THROWS(BeautyGuidance::decode(invalid,"geometry","original",6));
}

TEST_CASE("Native mixed puzzle assignments survive reopen and decline changed recipes", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    auto puzzle=BeautyPuzzle::create(*surface,4);
    const std::vector<PhysicalFilamentChannel> palette{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}};
    const MixedColorRecipe mix{"#808080",{{0,.4},{1,.6}},2,std::string(64,'a')};
    puzzle.match_filaments(*surface,palette,{mix});
    const auto id=puzzle.face_piece.front();const auto partition=puzzle.face_piece;
    puzzle.paint_mixed(id,mix);
    const auto saved=puzzle.encode();REQUIRE(saved["schema"]=="orca.beauty-puzzle/v3");
    auto restored=BeautyPuzzle::decode(saved,puzzle.geometry_id,partition.size());
    restored.match_filaments(*surface,palette,{mix});
    CHECK(restored.same_edit(puzzle));CHECK(restored.filament_slots.at(id)==2);
    const auto faces=restored.faces(id);
    if(faces.size()>1) {
        auto copy=restored;const auto split=copy.split(id,{faces.front()},*surface);
        copy.match_filaments(*surface,palette,{mix});CHECK(copy.filament_slots.at(split)==2);copy.validate(*surface);
    }
    auto changed=mix;changed.native_settings_fingerprint=std::string(64,'b');
    restored.match_filaments(*surface,palette,{changed});
    CHECK(restored.face_piece==partition);CHECK(restored.filament_slots.at(id)!=2);CHECK(restored.mixed_recipes.empty());
    auto invalid=saved;invalid["mixed_recipes"][0]["components"][0][0]=99;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,puzzle.geometry_id,partition.size()));
    invalid=saved;invalid["mixed_recipes"][0]["slot"]=0;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,puzzle.geometry_id,partition.size()));
}

TEST_CASE("Eye texture detail stays inside recognized eyes and declines flat or weak evidence", "[BeautyWorkbench][BeautyPuzzle]") {
    indexed_triangle_set mesh;std::vector<RGBA> colors;std::vector<size_t> eye;
    std::vector<size_t> expected;
    for(int y=0;y<8;++y)for(int x=0;x<10;++x)for(int side=0;side<2;++side) {
        const int first=int(mesh.vertices.size());const size_t f=mesh.indices.size();
        if(side==0) {
            mesh.vertices.emplace_back(float(x),float(y),0);
            mesh.vertices.emplace_back(float(x+1),float(y),0);
            mesh.vertices.emplace_back(float(x),float(y+1),0);
        } else {
            mesh.vertices.emplace_back(float(x+1),float(y+1),0);
            mesh.vertices.emplace_back(float(x),float(y+1),0);
            mesh.vertices.emplace_back(float(x+1),float(y),0);
        }
        mesh.indices.emplace_back(first,first+1,first+2);
        const bool inside=x>=2 && x<8 && y>=2 && y<6;
        const bool pupil=x>=4 && x<6 && y>=3 && y<5;
        const float intensity=inside && !pupil?.9f:.1f;
        for(int i=0;i<3;++i)colors.push_back({intensity,intensity,intensity,1});
        if(inside)eye.push_back(f);
        if(pupil)expected.push_back(f);
    }
    const auto surface=BeautySurface::build(mesh,colors);
    const auto selected=BeautyEyeDetail::dark_region(mesh,colors,*surface,eye);
    CHECK(selected==expected);
    // Black hair outside the eye cannot become a candidate.
    for(size_t f:selected)CHECK(std::find(eye.begin(),eye.end(),f)!=eye.end());
    auto flat=colors;for(auto& c:flat)c={.5f,.5f,.5f,1};
    CHECK(BeautyEyeDetail::dark_region(mesh,flat,*surface,eye).empty());
    auto weak=colors;for(auto& c:weak)if(c[0]<.5f)c={.85f,.85f,.85f,1};
    CHECK(BeautyEyeDetail::dark_region(mesh,weak,*surface,eye).empty());
    auto bad=eye;bad.push_back(mesh.indices.size());
    CHECK(BeautyEyeDetail::dark_region(mesh,colors,*surface,bad).empty());
}

TEST_CASE("Matched puzzle colors preserve separate regions and exact physical slots through edits and persistence", "[BeautyWorkbench][BeautyPuzzle]") {
    indexed_triangle_set mesh=its_make_cube(10,10,10);
    const auto surface=BeautySurface::build(mesh,{});
    auto puzzle=BeautyPuzzle::create(*surface,4);
    const auto partition=puzzle.face_piece;
    const std::vector<PhysicalFilamentChannel> palette {{0,"#FFFFFF","PLA",true},{2,"#222222","PLA",true},{4,"#222222","PLA",true}};
    puzzle.match_filaments(*surface,palette);
    REQUIRE(puzzle.face_piece==partition);
    REQUIRE(puzzle.filament_slots.size()==puzzle.piece_count());
    const uint32_t id=puzzle.face_piece.front();
    puzzle.paint_filament(id,4);
    const auto saved=puzzle.encode();
    REQUIRE(saved["schema"]=="orca.beauty-puzzle/v2");
    auto restored=BeautyPuzzle::decode(saved,puzzle.geometry_id,partition.size());
    REQUIRE(restored.same_edit(puzzle));
    restored.match_filaments(*surface,palette);
    CHECK(restored.filament_slots.at(id)==4);
    const auto selected=restored.faces(id);
    if(selected.size()>1) {
        const uint32_t created=restored.split(id,{selected.front()},*surface);
        CHECK(restored.filament_slots.at(created)==4);
        restored.validate(*surface);
    }
    auto invalid=saved;invalid["filament_slots"][0][1]=99;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,puzzle.geometry_id,partition.size()));
    invalid=saved;invalid["palette"][0]["compatible"]=false;
    // Force this unavailable channel to be referenced, independent of matching.
    invalid["filament_slots"][0][1]=0;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,puzzle.geometry_id,partition.size()));
}

TEST_CASE("A saved full color puzzle migrates to current filaments without changing the original surface", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);
    const auto surface=BeautySurface::build(mesh,std::vector<std::array<float,4>>(mesh.vertices.size(),{1,1,1,1}));
    auto original=BeautyPuzzle::create(*surface,4);
    original.paint(original.face_piece.front(),{1,0,0,1});
    const auto old_record=original.encode();
    REQUIRE(old_record["schema"]=="orca.beauty-puzzle/v1");
    auto matched=BeautyPuzzle::decode(old_record,original.geometry_id,original.face_piece.size());
    matched.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#FF0000","PLA",true},{2,"#000000","PLA",false}});
    CHECK(matched.face_piece==original.face_piece);
    CHECK(matched.filament_slots.at(original.face_piece.front())==1);
    CHECK_FALSE(matched.same_edit(original));
    CHECK(original.encode()==old_record);
    for(const auto& item:matched.filament_slots)CHECK(item.second!=2);
    matched.restore_source_color(original.face_piece.front(),*surface);
    CHECK(matched.filament_slots.at(original.face_piece.front())==0);
}

namespace {
std::shared_ptr<BeautySurface> puzzle_grid(int width = 6, int height = 4) {
    indexed_triangle_set mesh;
    for (int y = 0; y <= height; ++y)
        for (int x = 0; x <= width; ++x) mesh.vertices.emplace_back(float(x), float(y), 0.f);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int a = y * (width + 1) + x;
        mesh.indices.emplace_back(a, a + 1, a + width + 2);
        mesh.indices.emplace_back(a, a + width + 2, a + width + 1);
    }
    return BeautySurface::build(mesh, {});
}

BeautyPuzzle columns(const BeautySurface& surface, double boundary) {
    BeautyPuzzle result;
    result.geometry_id = surface.geometry_id;
    result.next_id = 3;
    for (const auto& center : surface.centers) result.face_piece.push_back(center.x() < boundary ? 1 : 2);
    return result;
}

template<class Predicate>
std::vector<size_t> matching(const BeautySurface& surface, Predicate predicate) {
    std::vector<size_t> result;
    for (size_t f = 0; f < surface.centers.size(); ++f) if (predicate(surface.centers[f])) result.push_back(f);
    return result;
}

const std::array<float, 4> blue {.1f, .2f, .8f, 1.f};
const std::array<float, 4> red {.8f, .2f, .1f, 1.f};

// A connected strip with distinct source-color halves is small enough to make
// the intended color boundary unambiguous, independently of triangulation.
BeautySurface color_strip() {
    BeautySurface surface;
    surface.geometry_id = "six-face-color-strip";
    for (size_t f = 0; f < 6; ++f) {
        surface.face_patch.push_back(uint32_t(f));
        surface.face_neighbors.push_back({f == 0 ? -1 : int32_t(f - 1), f == 5 ? -1 : int32_t(f + 1), -1});
        surface.centers.emplace_back(double(f), 0., 0.);
        surface.normals.emplace_back(0., 0., 1.);
        surface.areas.push_back(1.);
        BeautyPatch patch;
        patch.faces = {f};
        patch.area = 1.;
        patch.center = surface.centers.back();
        patch.normal = surface.normals.back();
        patch.mean_color = f < 3 ? std::array<double, 3>{.8, .2, .1} : std::array<double, 3>{.1, .2, .8};
        surface.patches.push_back(patch);
    }
    return surface;
}

size_t boundary_edges(const BeautyPuzzle& puzzle, const BeautySurface& surface) {
    size_t result = 0;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f)
        for (int32_t n : surface.face_neighbors[f])
            if (n >= 0 && size_t(n) > f && puzzle.face_piece[f] != puzzle.face_piece[n]) ++result;
    return result;
}
}

TEST_CASE("Mouth texture separates coherent light detail and refuses weak or unrelated evidence", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=puzzle_grid(12,8);BeautyGuidance g;g.names={"face","imouth"};
    g.labels.assign(surface->areas.size(),0);std::vector<RGBA> colors(surface->areas.size(),{.95f,.95f,.95f,1});
    std::vector<size_t> expected;
    for(size_t f=0;f<colors.size();++f) {
        const auto& p=surface->centers[f];
        if(p.x()>=2 && p.x()<10 && p.y()>=2 && p.y()<6) {
            g.labels[f]=1;colors[f]={.2f,.2f,.2f,1};
            if(p.x()>=3 && p.x()<9 && p.y()>=3 && p.y()<5) {colors[f]={.9f,.88f,.85f,1};expected.push_back(f);}
        }
    }
    const auto original=g;
    SECTION("only coherent source light area is separated and persistence retains it") {
        CHECK(beauty_refine_mouth_guidance(*surface,colors,g)==expected.size());
        for(size_t f=0;f<colors.size();++f)CHECK(g.labels[f]==(std::find(expected.begin(),expected.end(),f)!=expected.end()?2:original.labels[f]));
        auto restored=BeautyGuidance::decode(g.encode(surface->geometry_id,"source"),surface->geometry_id,"source",colors.size());
        CHECK(restored.labels==g.labels);CHECK(restored.names==g.names);
        CHECK(beauty_refine_mouth_guidance(*surface,colors,restored)==0);
    }
    SECTION("closed mouth and missing recognition do not infer a light region") {
        g.names[1]="ulip";CHECK(beauty_refine_mouth_guidance(*surface,colors,g)==0);
        g.labels.clear();CHECK(beauty_refine_mouth_guidance(*surface,colors,g)==0);
    }
    SECTION("dim, flat and saturated textures remain untouched") {
        for(const auto value:std::vector<RGBA>{{.4f,.4f,.4f,1},{.21f,.21f,.21f,1},{1.f,.65f,.2f,1}}) {
            auto variant=colors;for(size_t f:expected)variant[f]=value;
            auto next=original;CHECK(beauty_refine_mouth_guidance(*surface,variant,next)==0);CHECK(next.labels==original.labels);
        }
    }
    SECTION("isolated highlights do not form one fabricated region") {
        for(size_t f:expected)colors[f]={.2f,.2f,.2f,1};
        for(size_t f=0;f<colors.size();++f)if(g.labels[f]==1 && f%4==0)colors[f]={.95f,.95f,.95f,1};
        CHECK(beauty_refine_mouth_guidance(*surface,colors,g)==0);
    }
    SECTION("illumination and scale variation preserve the same visible split") {
        for(float gain:{.9f,1.f,1.05f}) {
            auto variant=colors;for(auto& c:variant)for(size_t k=0;k<3;++k)c[k]*=gain;
            auto next=original;auto scaled=*surface;for(auto& a:scaled.areas)a*=100.;
            CHECK(beauty_refine_mouth_guidance(scaled,variant,next)==expected.size());
            for(size_t f:expected)CHECK(next.labels[f]==2);
        }
    }
}

TEST_CASE("Puzzle initialization covers the unchanged surface with deterministic connected pieces", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(18, 12);
    const auto original_patches = surface->face_patch;
    const auto original_neighbors = surface->face_neighbors;
    const auto a = BeautyPuzzle::create(*surface, 12);
    const auto b = BeautyPuzzle::create(*surface, 12);
    REQUIRE(a.face_piece == b.face_piece);
    REQUIRE(a.face_piece.size() == surface->face_patch.size());
    REQUIRE(a.piece_count() == 12);
    REQUIRE(a.colors.empty());
    REQUIRE_NOTHROW(a.validate(*surface));
    REQUIRE(surface->face_patch == original_patches);
    REQUIRE(surface->face_neighbors == original_neighbors);
    size_t covered = 0;
    for (uint32_t id = 1; id < a.next_id; ++id) covered += a.faces(id).size();
    REQUIRE(covered == surface->face_patch.size());
    REQUIRE_THROWS(BeautyPuzzle::create(*surface, 0));
    REQUIRE_THROWS(BeautyPuzzle::create(*surface, 12, [] { return true; }));
    size_t checkpoints = 0;
    REQUIRE_THROWS(BeautyPuzzle::create(*surface, 12, [&] { return ++checkpoints >= 5; }));
    REQUIRE(checkpoints >= 5);
}

TEST_CASE("Boundary drags ignore disconnected visible samples and keep the shared boundary connected", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = color_strip();
    const bool outward = GENERATE(true, false);
    auto puzzle = columns(surface, 3.);
    puzzle.paint(1, red); puzzle.paint(2, blue);
    const auto selected = outward ? std::vector<size_t>{3, 5} : std::vector<size_t>{2, 0};
    puzzle.resize_boundary(1, selected, outward, surface);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    CHECK(puzzle.face_piece == (outward ? std::vector<uint32_t>{1, 1, 1, 1, 2, 2}
                                      : std::vector<uint32_t>{1, 1, 2, 2, 2, 2}));
    CHECK(puzzle.colors.at(1) == red);
    CHECK(puzzle.colors.at(2) == blue);
    const auto saved = puzzle.encode();
    REQUIRE_THROWS(puzzle.resize_boundary(1, {outward ? size_t(5) : size_t(0)}, outward, surface));
    CHECK(puzzle.encode() == saved);
}

TEST_CASE("Saved colored regions lose staircase teeth without erasing small pieces or color intent", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 24);
    auto puzzle = columns(*surface, 20.);
    puzzle.next_id = 4;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.x() < 22. && int(c.y()) % 4 < 2) puzzle.face_piece[f] = 1;
        if (c.x() > 4. && c.x() < 6. && c.y() > 4. && c.y() < 5.) puzzle.face_piece[f] = 3;
    }
    puzzle.paint(1, blue); puzzle.paint(2, red); puzzle.paint(3, red);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    const auto small_eye = puzzle.faces(3);
    const auto old_colors = puzzle.colors;
    const auto old_patches = surface->face_patch;
    const auto old_centers = surface->centers;
    const size_t original_boundary = boundary_edges(puzzle, *surface);
    puzzle = BeautyPuzzle::decode(puzzle.encode(), surface->geometry_id, surface->face_patch.size());
    puzzle.smooth_boundaries(*surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(boundary_edges(puzzle, *surface) < original_boundary);
    CHECK(puzzle.faces(3) == small_eye);
    CHECK(puzzle.piece_count() == 3);
    CHECK(puzzle.next_id == 4);
    CHECK(puzzle.colors == old_colors);
    CHECK(puzzle.face_piece.size() == surface->face_patch.size());
    CHECK(surface->face_patch == old_patches);
    CHECK(surface->centers == old_centers);
    const auto saved = puzzle.encode();
    auto foreign = *surface; foreign.geometry_id = "different-surface";
    REQUIRE_THROWS(puzzle.smooth_boundaries(foreign));
    CHECK(puzzle.encode() == saved);
}

TEST_CASE("Smoothing preserves narrow connecting necks and sparse saved identities", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(30, 16);
    auto puzzle = columns(*surface, 15.);
    puzzle.next_id = BeautyPuzzle::max_id;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        const bool left = c.x() < 8.;
        const bool right = c.x() > 20.;
        const bool neck = c.y() > 7. && c.y() < 8.;
        puzzle.face_piece[f] = left || right || neck ? BeautyPuzzle::max_id - 1 : (c.y() < 8. ? 2 : 3);
    }
    puzzle.paint(BeautyPuzzle::max_id - 1, blue);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    const size_t original_boundary = boundary_edges(puzzle, *surface);
    puzzle.smooth_boundaries(*surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 3);
    CHECK(puzzle.next_id == BeautyPuzzle::max_id);
    CHECK(puzzle.colors.at(BeautyPuzzle::max_id - 1) == blue);
    CHECK(boundary_edges(puzzle, *surface) <= original_boundary);
}

TEST_CASE("Physical boundary diffusion rounds long stairs beyond single triangle teeth", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(160, 96);
    auto puzzle = columns(*surface, 80.);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        puzzle.face_piece[f] = c.x() < (int(c.y() / 12.) % 2 ? 86. : 74.) ? 1 : 2;
    }
    puzzle.paint(1, blue); puzzle.paint(2, red);
    const auto original = puzzle.face_piece;
    const size_t original_boundary = boundary_edges(puzzle, *surface);
    std::vector<int> depth(original.size(), -1);
    std::vector<size_t> queue;
    for (size_t f = 0; f < original.size(); ++f)
        for (int32_t n : surface->face_neighbors[f])
            if (n >= 0 && original[f] != original[n]) { depth[f] = 0; queue.push_back(f); break; }
    for (size_t at = 0; at < queue.size(); ++at)
        for (int32_t n : surface->face_neighbors[queue[at]])
            if (n >= 0 && depth[n] < 0) { depth[n] = depth[queue[at]] + 1; queue.push_back(size_t(n)); }
    puzzle.smooth_boundaries(*surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(boundary_edges(puzzle, *surface) < original_boundary * .85);
    CHECK(puzzle.piece_count() == 2);
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    int deepest_change = 0;
    for (size_t f = 0; f < original.size(); ++f)
        if (puzzle.face_piece[f] != original[f]) deepest_change = std::max(deepest_change, depth[f]);
    CHECK(deepest_change >= 3);
}

TEST_CASE("Physical boundary smoothing is invariant to a uniform change of model units", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(72, 48);
    auto puzzle = columns(*surface, 36.);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        puzzle.face_piece[f] = c.x() < (int(c.y() / 6.) % 2 ? 39. : 33.) ? 1 : 2;
    }
    auto scaled_surface = *surface;
    for (auto& center : scaled_surface.centers) center *= 1024.;
    for (auto& area : scaled_surface.areas) area *= 1024. * 1024.;
    for (auto& patch : scaled_surface.patches) { patch.center *= 1024.; patch.area *= 1024. * 1024.; }
    auto scaled = puzzle;
    puzzle.smooth_boundaries(*surface);
    scaled.smooth_boundaries(scaled_surface);
    CHECK(puzzle.face_piece == scaled.face_piece);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    REQUIRE_NOTHROW(scaled.validate(scaled_surface));
}

TEST_CASE("A rejected boundary fragment does not roll back useful smoothing on a distant shell", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = *puzzle_grid(64, 48);
    const size_t first_count = surface.face_patch.size();
    BeautyPuzzle puzzle;
    puzzle.geometry_id = surface.geometry_id;
    puzzle.next_id = 5;
    for (const auto& c : surface.centers)
        puzzle.face_piece.push_back(c.x() < (int(c.y() / 8.) % 2 ? 36. : 28.) ? 1 : 2);
    // A separate, larger shell has a strong texture boundary. A gray notch
    // permits one diffusion transfer that lengthens its outline, while the
    // adjacent contrasting texture blocks the rest of that local proposal.
    // Its rejected energy must not cancel smoothing on the first shell.
    for (size_t f = 0; f < first_count; ++f) {
        surface.centers.push_back((surface.centers[f] * 1000. + Vec3d(200000., 0., 0.)).eval());
        surface.normals.push_back(Vec3d(0., 0., 1.));
        surface.areas.push_back(surface.areas[f] * 1000000.);
        auto neighbors = surface.face_neighbors[f];
        for (auto& n : neighbors) if (n >= 0) n += int32_t(first_count);
        surface.face_neighbors.push_back(neighbors);
        puzzle.face_piece.push_back(puzzle.face_piece[f] + 2);
    }
    surface.face_patch.resize(puzzle.face_piece.size());
    std::iota(surface.face_patch.begin(), surface.face_patch.end(), 0);
    surface.patches.assign(puzzle.face_piece.size(), {});
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        auto& patch = surface.patches[f];
        patch.faces = {f}; patch.area = surface.areas[f]; patch.center = surface.centers[f];
        patch.normal = surface.normals[f];
        patch.mean_color = f < first_count ? std::array<double, 3>{.5, .5, .5} :
            puzzle.face_piece[f] == 3 ? std::array<double, 3>{1., 0., 0.} : std::array<double, 3>{0., 0., 1.};
    }
    const size_t bad_tip = first_count + 1990, neighbor = first_count + 1993;
    surface.patches[bad_tip].mean_color = surface.patches[neighbor].mean_color = {.5, .5, .5};
    puzzle.paint(1, blue); puzzle.paint(3, red);
    const auto original = puzzle.face_piece;
    const auto colors = puzzle.colors;
    puzzle.smooth_boundaries(surface);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    CHECK(puzzle.face_piece[bad_tip] == original[bad_tip]);
    CHECK(puzzle.face_piece[neighbor] == original[neighbor]);
    CHECK(puzzle.piece_count() == 4);
    CHECK(puzzle.colors == colors);
    size_t useful_changes = 0;
    for (size_t f = 0; f < first_count; ++f) if (puzzle.face_piece[f] != original[f]) ++useful_changes;
    CHECK(useful_changes > 50);
}

TEST_CASE("A boundary reshape grows and shrinks one piece while ignoring distant visible samples", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 24);
    auto puzzle = columns(*surface, 20.);
    puzzle.paint(1, blue); puzzle.paint(2, red);
    auto added = matching(*surface, [](const Vec3d& c) {
        return c.x() > 20. && c.x() < 23. && c.y() > 2. && c.y() < 10.;
    });
    const auto remote = matching(*surface, [](const Vec3d& c) { return c.x() > 38. && c.y() > 22.; });
    added.insert(added.end(), remote.begin(), remote.end());
    const auto removed = matching(*surface, [](const Vec3d& c) {
        return c.x() > 17. && c.x() < 20. && c.y() > 14. && c.y() < 22.;
    });
    puzzle.reshape_region(1, added, removed, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 2);
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    for (size_t f : remote) CHECK(puzzle.face_piece[f] == 2);
    for (size_t f : matching(*surface, [](const Vec3d& c) {
             return c.x() > 20. && c.x() < 22. && c.y() > 4. && c.y() < 8.;
         })) CHECK(puzzle.face_piece[f] == 1);
    for (size_t f : matching(*surface, [](const Vec3d& c) {
             return c.x() > 18. && c.x() < 20. && c.y() > 16. && c.y() < 20.;
         })) CHECK(puzzle.face_piece[f] == 2);
    const auto saved = puzzle.encode();
    puzzle.reshape_region(1, remote, {}, *surface);
    CHECK(puzzle.encode() == saved);
    puzzle.reshape_region(1, {}, {}, *surface);
    CHECK(puzzle.encode() == saved);
    REQUIRE_THROWS(puzzle.reshape_region(1, {}, {surface->face_patch.size()}, *surface));
    CHECK(puzzle.encode() == saved);
}

TEST_CASE("Boundary reshaping closes single missing samples without flooding unsampled areas", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = color_strip();
    auto puzzle = columns(surface, 1.);
    puzzle.paint(1, blue); puzzle.paint(2, red);
    // Faces 1 and 3 are stroke samples separated by exactly one missed face.
    puzzle.reshape_region(1, {1, 3}, {}, surface);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    CHECK(puzzle.face_piece == (std::vector<uint32_t>{1, 1, 1, 1, 2, 2}));
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
}

TEST_CASE("Recognized boundaries guide local fairing without blocking an explicit correction", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=puzzle_grid(40,24);
    const auto original=columns(*surface,20.);
    const auto added=matching(*surface,[](const Vec3d& p){return p.x()>=20. && p.x()<23. && p.y()>5. && p.y()<15.;});
    std::vector<int32_t> labels;
    for(uint32_t id:original.face_piece)labels.push_back(int32_t(id));
    auto guided=original;
    guided.reshape_region(1,added,{},*surface,labels);
    REQUIRE_NOTHROW(guided.validate(*surface));
    // The face parser calls the entire new area a different part. A deliberate
    // center movement must still win, so recognition never locks a wrong mask.
    for(size_t f:matching(*surface,[](const Vec3d& p){return p.x()>20. && p.x()<22. && p.y()>8. && p.y()<12.;}))CHECK(guided.face_piece[f]==1);
    auto unknown=original,plain=original;
    plain.reshape_region(1,added,{},*surface);
    unknown.reshape_region(1,added,{},*surface,std::vector<int32_t>(labels.size(),-1));
    CHECK(unknown.same_edit(plain));
    CHECK(guided.face_piece!=plain.face_piece);
    const auto saved=guided.encode();
    CHECK_THROWS(guided.reshape_region(1,added,{},*surface,{0}));
    CHECK(guided.encode()==saved);
    for(size_t f:matching(*surface,[](const Vec3d& p){return p.y()>20.;}))CHECK(guided.face_piece[f]==original.face_piece[f]);
}

TEST_CASE("A reshape keeps donor island colors and does not smooth unrelated boundaries", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 24);
    auto puzzle = columns(*surface, 3.);
    puzzle.paint(1, blue); puzzle.paint(2, red);
    const auto band = matching(*surface, [](const Vec3d& c) { return c.y() > 10. && c.y() < 13.; });
    const auto before = puzzle.face_piece;
    puzzle.reshape_region(1, band, {}, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 3);
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    CHECK(puzzle.colors.at(3) == red);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.y() > 11. && c.y() < 12.) CHECK(puzzle.face_piece[f] == 1);
        if (c.y() < 6.) CHECK(puzzle.colors.at(puzzle.face_piece[f]) == (before[f] == 1 ? blue : red));
    }
}

TEST_CASE("Dragging a boundary fills tiny new remnants without swallowing existing small regions", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(80, 40);
    const bool outward = GENERATE(true, false);
    const double boundary = outward ? 20. : 60.;
    auto puzzle = columns(*surface, boundary);
    puzzle.next_id = 4;
    const double start = outward ? 20. : 54.;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.x() > start + 2. && c.x() < start + 4. && c.y() > 13. && c.y() < 15.) puzzle.face_piece[f] = 3;
    }
    puzzle.paint(1, blue); puzzle.paint(2, red); puzzle.paint(3, red);
    const auto existing_small_piece = puzzle.faces(3);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    std::vector<size_t> stroke, missed;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.x() <= start || c.x() >= start + 6. || c.y() <= 8. || c.y() >= 16.) continue;
        if (puzzle.face_piece[f] == 3) continue;
        if (c.x() > start + 2. && c.x() < start + 4. && c.y() > 10. && c.y() < 12.) missed.push_back(f);
        else stroke.push_back(f);
    }
    if (outward) puzzle.reshape_region(1, stroke, {}, *surface);
    else puzzle.reshape_region(1, {}, stroke, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 3);
    CHECK(puzzle.next_id == 4);
    // Growing piece 1 must not sample or consume the untouched piece 3.
    // When shrinking piece 1, however, its explicitly removed faces touch
    // both 2 and 3; each is a valid geodesic recipient. Protect the original
    // small piece without incorrectly freezing its receiving boundary.
    if (outward) CHECK(puzzle.faces(3) == existing_small_piece);
    for (size_t f : existing_small_piece) CHECK(puzzle.face_piece[f] == 3);
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    CHECK(puzzle.colors.at(3) == red);
    const std::set<uint32_t> shrink_recipients {2, 3};
    for (size_t f : missed) {
        if (outward) CHECK(puzzle.face_piece[f] == 1);
        else CHECK(shrink_recipients.count(puzzle.face_piece[f]) == 1);
    }
}

TEST_CASE("Puzzle aggregation follows adjacent color regions and preserves disconnected shells", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = color_strip();
    auto puzzle = BeautyPuzzle::create(surface, 2);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    CHECK(puzzle.face_piece[0] == puzzle.face_piece[2]);
    CHECK(puzzle.face_piece[3] == puzzle.face_piece[5]);
    CHECK(puzzle.face_piece[2] != puzzle.face_piece[3]);
    surface.face_neighbors[2][1] = -1;
    surface.face_neighbors[3][0] = -1;
    puzzle = BeautyPuzzle::create(surface, 1);
    CHECK(puzzle.piece_count() == 2);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    // Even a saved micro-patch label crossing disconnected shells is separated.
    surface.face_patch.assign(6, 0);
    puzzle = BeautyPuzzle::create(surface, 1);
    CHECK(puzzle.piece_count() == 2);
    REQUIRE_NOTHROW(puzzle.validate(surface));
}

TEST_CASE("Growing a puzzle piece transfers a shared boundary and preserves donor island colors", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 1.);
    puzzle.paint(1, blue);
    puzzle.paint(2, red);
    const auto band = matching(*surface, [](const Vec3d& c) { return c.y() > 1. && c.y() < 2.; });
    puzzle.grow(1, band, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 3);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.x() < 1. || (c.y() > 1. && c.y() < 2.)) CHECK(puzzle.face_piece[f] == 1);
        else if (c.y() > 2.) CHECK(puzzle.face_piece[f] == 2);
        else CHECK(puzzle.face_piece[f] == 3);
    }
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    CHECK(puzzle.colors.at(3) == red);

    auto unchanged = columns(*surface, 1.);
    const auto before = unchanged.encode();
    const auto remote = matching(*surface, [](const Vec3d& c) { return c.x() > 4. && c.y() > 2.; });
    REQUIRE_THROWS(unchanged.grow(1, remote, *surface));
    CHECK(unchanged.encode() == before);
    REQUIRE_THROWS(unchanged.grow(1, {surface->face_patch.size()}, *surface));
    CHECK(unchanged.encode() == before);
}

TEST_CASE("Shrinking a puzzle piece hands faces to adjacent pieces and rejects enclosed holes atomically", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 3.);
    puzzle.paint(1, blue);
    puzzle.paint(2, red);
    const auto enclosed = matching(*surface, [](const Vec3d& c) { return c.x() > 1. && c.x() < 2. && c.y() > 1. && c.y() < 2.; });
    const auto before = puzzle.encode();
    REQUIRE_THROWS(puzzle.shrink(1, enclosed, *surface));
    CHECK(puzzle.encode() == before);

    const auto border = matching(*surface, [](const Vec3d& c) { return c.x() > 2. && c.x() < 3.; });
    puzzle.shrink(1, border, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f)
        CHECK(puzzle.face_piece[f] == (surface->centers[f].x() < 2. ? 1 : 2));
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    CHECK(puzzle.piece_count() == 2);
    puzzle.shrink(1, puzzle.faces(1), *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 1);
    CHECK(puzzle.colors.count(1) == 0);
    CHECK(puzzle.colors.at(2) == red);
}

TEST_CASE("Splitting and merging puzzle pieces preserve connected IDs and explicit color intent", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = BeautyPuzzle::create(*surface, 1);
    puzzle.paint(1, red);
    const auto band = matching(*surface, [](const Vec3d& c) { return c.x() > 2. && c.x() < 3.; });
    const uint32_t created = puzzle.split(1, band, *surface);
    REQUIRE(created == 2);
    REQUIRE(puzzle.piece_count() == 3);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    for (uint32_t id = 1; id < puzzle.next_id; ++id) CHECK(puzzle.colors.at(id) == red);
    const auto before = puzzle.encode();
    REQUIRE_THROWS(puzzle.merge(1, 3, *surface));
    CHECK(puzzle.encode() == before);
    puzzle.paint(created, blue);
    puzzle.merge(created, 1, *surface);
    CHECK(puzzle.faces(1).empty());
    CHECK(puzzle.colors.count(1) == 0);
    CHECK(puzzle.colors.at(created) == blue);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    puzzle.clear_color(3);
    puzzle.merge(3, created, *surface);
    CHECK(puzzle.colors.empty());
    CHECK(puzzle.piece_count() == 1);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    REQUIRE_THROWS(puzzle.split(3, puzzle.faces(3), *surface));
}

TEST_CASE("Painting a cross-color edit group changes only its selected faces", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 3.);
    puzzle.palette = {{0, "#FF0000", "PLA", true}, {1, "#0000FF", "PLA", true}, {2, "#00FF00", "PLA", true}};
    puzzle.paint_filament(1, 0);
    puzzle.paint_filament(2, 1);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    const auto before = puzzle;
    const auto selected = matching(*surface, [](const Vec3d& c) { return c.y() >= 1. && c.y() < 3.; });
    std::vector<uint8_t> mask(puzzle.face_piece.size(), 0);
    for (size_t f : selected) mask[f] = 1;

    puzzle.paint_faces_filament(selected, *surface, 2);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    std::set<uint32_t> painted_ids;
    for (size_t f : selected) painted_ids.insert(puzzle.face_piece[f]);
    CHECK(painted_ids.size() == 1);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto slot = puzzle.filament_slots.at(puzzle.face_piece[f]);
        CHECK(slot == (mask[f] ? 2 : before.filament_slots.at(before.face_piece[f])));
        if (!mask[f]) CHECK(puzzle.colors.at(puzzle.face_piece[f]) == before.colors.at(before.face_piece[f]));
    }
    const auto reopened = BeautyPuzzle::decode(puzzle.encode(), puzzle.geometry_id, puzzle.face_piece.size());
    REQUIRE_NOTHROW(reopened.validate(*surface));
    CHECK(reopened.same_edit(puzzle));
    const auto painted = puzzle.encode();
    REQUIRE_THROWS(puzzle.paint_faces_filament({}, *surface, 2));
    REQUIRE_THROWS(puzzle.paint_faces_filament({surface->face_patch.size()}, *surface, 2));
    REQUIRE_THROWS(puzzle.paint_faces_filament(selected, *surface, 99));
    CHECK(puzzle.encode() == painted);
    puzzle = before; // The workbench stores the pre-edit puzzle as one undo snapshot.
    CHECK(puzzle.same_edit(before));
}

TEST_CASE("Puzzle persistence rejects invalid identity coverage capacity and color records", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 3.);
    puzzle.paint(2, red);
    const auto json = puzzle.encode();
    const auto decode = [&](const nlohmann::json& value) { return BeautyPuzzle::decode(value, surface->geometry_id, surface->face_patch.size()); };
    const auto loaded = decode(json);
    CHECK(loaded.face_piece == puzzle.face_piece);
    CHECK(loaded.colors == puzzle.colors);
    CHECK(loaded.next_id == puzzle.next_id);
    REQUIRE_NOTHROW(loaded.validate(*surface));
    REQUIRE_THROWS(BeautyPuzzle::decode(json, "unrelated", surface->face_patch.size()));
    REQUIRE_THROWS(BeautyPuzzle::decode(json, surface->geometry_id, surface->face_patch.size() - 1));
    REQUIRE_THROWS(BeautyPuzzle::decode(json, surface->geometry_id, BeautyPuzzle::max_faces + 1));

    auto corrupt = json;
    corrupt["piece_runs"][0][1] = surface->face_patch.size() + 1;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["piece_runs"] = nlohmann::json::array({{1, 1}});
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["piece_runs"][0][0] = 0;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["piece_runs"][0][0] = 1.5;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["piece_runs"][0][0] = -1;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["next_id"] = 2;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["next_id"] = std::numeric_limits<uint64_t>::max();
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"][0]["id"] = 42;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"].push_back(corrupt["colors"][0]);
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"][0]["rgba"][0] = 1.2;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"][0]["rgba"][0] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"][0]["rgba"] = {0, 1, 0};
    REQUIRE_THROWS(decode(corrupt));

    auto disconnected = loaded;
    const auto far_corner = matching(*surface, [](const Vec3d& c) { return c.x() > 5. && c.y() > 3.; });
    for (size_t f : far_corner) disconnected.face_piece[f] = 1;
    const auto structurally_valid = decode(disconnected.encode());
    REQUIRE_THROWS(structurally_valid.validate(*surface));
    REQUIRE_THROWS(puzzle.paint(1, {1.f, 0.f, std::numeric_limits<float>::infinity(), 1.f}));
    REQUIRE_THROWS(puzzle.paint(999, blue));
    CHECK(puzzle.encode() == json);
}

TEST_CASE("Unpainted puzzle colors use surface area and painted colors remain exact", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = color_strip();
    surface.areas[0] = 4.;
    const auto puzzle = BeautyPuzzle::create(surface, 1);
    const auto color = puzzle.representative_color(1, surface);
    // Red half has area 6; blue half has area 3.
    CHECK_THAT(color[0], Catch::Matchers::WithinAbs((.8 * 6 + .1 * 3) / 9., 1e-6));
    CHECK_THAT(color[2], Catch::Matchers::WithinAbs((.1 * 6 + .8 * 3) / 9., 1e-6));
    auto edited = puzzle;
    edited.paint(1, blue);
    CHECK(edited.representative_color(1, surface) == blue);
    edited.clear_color(1);
    CHECK(edited.representative_color(1, surface) == color);
    auto wrong_surface = surface;
    wrong_surface.geometry_id = "other";
    REQUIRE_THROWS(edited.representative_color(1, wrong_surface));
    wrong_surface = surface;
    wrong_surface.face_neighbors[0][1] = -1;
    REQUIRE_THROWS(BeautyPuzzle::create(wrong_surface, 2));
}

TEST_CASE("Coarse editing regions cover the surface with fewer connected areas and unchanged source data", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 30);
    const auto original_partition = surface->face_patch;
    const auto original_centers = surface->centers;
    const auto detailed = BeautyPuzzle::create(*surface);
    const auto coarse = BeautyPuzzle::create_regions(*surface);
    REQUIRE_NOTHROW(coarse.validate(*surface));
    CHECK(coarse.face_piece.size() == surface->face_patch.size());
    CHECK(coarse.piece_count() <= 35);
    CHECK(coarse.piece_count() > 1);
    CHECK(coarse.piece_count() < detailed.piece_count());
    CHECK(coarse.colors.empty());
    CHECK(coarse.face_piece == BeautyPuzzle::create_regions(*surface, std::vector<int32_t>(surface->face_patch.size(), -1)).face_piece);
    CHECK(surface->face_patch == original_partition);
    CHECK(surface->centers == original_centers);
    REQUIRE_THROWS(BeautyPuzzle::create_regions(*surface, {1}));
    auto invalid = std::vector<int32_t>(surface->face_patch.size(), -1); invalid.back() = -2;
    REQUIRE_THROWS(BeautyPuzzle::create_regions(*surface, invalid));
    REQUIRE_THROWS(BeautyPuzzle::create_regions(*surface, {}, [] { return true; }));
}

TEST_CASE("Boundary regularization changes the real partition and removes uniform-color staircase edges", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = puzzle_grid(56, 20);
    surface->patches.assign(28, {});
    for (size_t f = 0; f < surface->face_patch.size(); ++f) {
        const auto& c = surface->centers[f];
        const int shift = int(c.y()) % 4 < 2 ? 0 : 1;
        const uint32_t id = uint32_t(std::min(27, (int(c.x()) + shift) / 2));
        surface->face_patch[f] = id;
        auto& patch = surface->patches[id];
        patch.faces.push_back(f); patch.area += surface->areas[f];
        patch.center += surface->centers[f] * surface->areas[f];
        patch.normal = Vec3d(0., 0., 1.);
        patch.mean_color = {.5, .5, .5};
    }
    for (auto& patch : surface->patches) patch.center /= patch.area;
    const auto jagged = BeautyPuzzle::create(*surface, 28);
    const auto smooth = BeautyPuzzle::create_regions(*surface);
    REQUIRE_NOTHROW(smooth.validate(*surface));
    CHECK(smooth.face_piece != jagged.face_piece);
    CHECK(boundary_edges(smooth, *surface) < boundary_edges(jagged, *surface));
    CHECK(smooth.piece_count() <= jagged.piece_count());
}

TEST_CASE("Sparse semantic hints preserve separate regions and cannot flood an unrelated coarse block", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 30);
    const auto coarse = BeautyPuzzle::create_regions(*surface);
    uint32_t largest = 1;
    for (uint32_t id = 2; id < coarse.next_id; ++id)
        if (coarse.faces(id).size() > coarse.faces(largest).size()) largest = id;
    const auto region = coarse.faces(largest);
    REQUIRE(region.size() > 10);
    const auto first = *std::min_element(region.begin(), region.end(), [&](size_t a, size_t b) {
        return surface->centers[a].x() < surface->centers[b].x();
    });
    const auto second = *std::max_element(region.begin(), region.end(), [&](size_t a, size_t b) {
        return (surface->centers[a] - surface->centers[first]).squaredNorm() <
               (surface->centers[b] - surface->centers[first]).squaredNorm();
    });
    std::vector<int32_t> labels(surface->face_patch.size(), -1);
    labels[first] = 4; labels[second] = 9;
    const auto result = BeautyPuzzle::create_regions(*surface, labels);
    REQUIRE_NOTHROW(result.validate(*surface));
    const auto eye = result.face_piece[first], nose = result.face_piece[second];
    CHECK(eye != nose);
    CHECK(result.faces(eye).size() > 1);
    CHECK(result.faces(eye).size() < region.size());
    for (size_t f : result.faces(eye)) CHECK(coarse.face_piece[f] == largest);
    for (size_t f : result.faces(nose)) CHECK(coarse.face_piece[f] == largest);
    CHECK(result.colors.empty());
}

TEST_CASE("One semantic part preserves substantial contrasting source pigments", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface=color_strip();std::vector<int32_t> labels(surface.areas.size(),0);
    const auto result=BeautyPuzzle::create_regions(surface,labels);
    CHECK(result.face_piece.front()!=result.face_piece.back());
    for(size_t f=0;f<3;++f)CHECK(result.face_piece[f]==result.face_piece[0]);
    for(size_t f=3;f<6;++f)CHECK(result.face_piece[f]==result.face_piece[3]);
    REQUIRE_NOTHROW(result.validate(surface));
    for(auto& patch:surface.patches)patch.mean_color={.5,.4,.3};
    const auto uniform=BeautyPuzzle::create_regions(surface,labels);
    CHECK(uniform.piece_count()==1);
}

TEST_CASE("Unknown feature growth spends less reach across a fold and preserves reliable seeds", "[BeautyWorkbench][BeautyPuzzle]") {
    BeautySurface flat;flat.geometry_id="fold-growth";flat.patches.resize(1);
    auto& patch=flat.patches.front();patch.mean_color={.5,.5,.5};patch.normal=Vec3d(0,0,1);
    for(size_t f=0;f<100;++f) {
        flat.face_patch.push_back(0);flat.areas.push_back(.01);patch.area+=.01;patch.faces.push_back(f);
        flat.centers.emplace_back(.1*double(f),0,0);flat.normals.emplace_back(0,0,1);
        flat.face_neighbors.push_back({f?int32_t(f-1):-1,f<99?int32_t(f+1):-1,-1});
    }
    auto folded=flat;
    for(size_t f=50;f<100;++f) {
        const double x=.1*double(f-50);
        folded.centers[f]=Vec3d(5.+.5*x,0.,std::sqrt(.75)*x);
        folded.normals[f]=Vec3d(-std::sqrt(.75),0.,.5);
    }
    std::vector<int32_t> labels(100,-1);for(size_t f=40;f<49;++f)labels[f]=0;
    const auto plain=BeautyPuzzle::create_regions(flat,labels,{}, {"nose"}), bent=BeautyPuzzle::create_regions(folded,labels,{}, {"nose"});
    auto gentle=flat;
    for(size_t f=0;f<100;++f)gentle.normals[f]=Vec3d(-std::sin(.01*f),0.,std::cos(.01*f));
    CHECK(BeautyPuzzle::create_regions(gentle,labels,{}, {"nose"}).face_piece==plain.face_piece);
    const auto legacy=BeautyPuzzle::create_regions(folded,labels);
    for(const auto& name:{"hair","face","neck","clothing"})
        CHECK(BeautyPuzzle::create_regions(folded,labels,{}, {name}).face_piece==legacy.face_piece);
    const auto extent=[](const BeautyPuzzle& p){size_t last=40;for(size_t f=40;f<100;++f)if(p.face_piece[f]==p.face_piece[40])last=f;return last;};
    CHECK(extent(bent)<extent(plain));
    for(size_t f=40;f<49;++f)CHECK(bent.face_piece[f]==bent.face_piece[40]);
    CHECK(bent.face_piece.back()!=bent.face_piece[40]);
    // Rigid rotation and a change of length units must not change membership.
    auto scaled=folded;const Eigen::AngleAxisd rotation(.7,Vec3d(1,2,3).normalized());
    for(auto& c:scaled.centers)c=rotation*c*10.;
    for(auto& n:scaled.normals)n=rotation*n;
    for(auto& a:scaled.areas)a*=100.;
    for(auto& p:scaled.patches){p.area*=100.;p.center=rotation*p.center*10.;p.normal=rotation*p.normal;}
    CHECK(BeautyPuzzle::create_regions(scaled,labels,{}, {"nose"}).face_piece==bent.face_piece);
    REQUIRE_NOTHROW(bent.validate(folded));
}

TEST_CASE("Semantic region interiors fill unknown holes while retaining seed boundaries", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(24, 20);
    const auto coarse = BeautyPuzzle::create_regions(*surface);
    size_t hole = surface->face_patch.size();
    for (size_t f = 0; f < coarse.face_piece.size(); ++f) {
        bool interior = true;
        for (int32_t n : surface->face_neighbors[f])
            if (n < 0 || coarse.face_piece[n] != coarse.face_piece[f]) interior = false;
        if (interior) { hole = f; break; }
    }
    REQUIRE(hole < surface->face_patch.size());
    const auto region = coarse.faces(coarse.face_piece[hole]);
    std::vector<int32_t> labels(surface->face_patch.size(), -1);
    for (size_t f : region) labels[f] = 3;
    labels[hole] = -1;
    const auto result = BeautyPuzzle::create_regions(*surface, labels);
    REQUIRE_NOTHROW(result.validate(*surface));
    const uint32_t id = result.face_piece[hole];
    for (size_t f : region) CHECK(result.face_piece[f] == id);
    CHECK(result.faces(id).size() == region.size());
}

TEST_CASE("Eye detail refinement keeps the existing piece and removes only newly stranded fragments", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(24, 20);
    auto puzzle = columns(*surface, 18.);
    const auto eye = matching(*surface, [](const Vec3d& c) { return c.x()>6. && c.x()<9. && c.y()>7. && c.y()<10.; });
    const auto owner = puzzle.assign_region(eye, *surface);
    puzzle.paint(owner, blue); puzzle.paint(2, red);
    const auto before = puzzle;
    auto detail = matching(*surface, [](const Vec3d& c) { return c.x()>5. && c.x()<11. && c.y()>6. && c.y()<11.; });
    const auto hole = *std::find_if(detail.begin(), detail.end(), [&](size_t f) { return surface->centers[f].x()>9. && surface->centers[f].x()<10. && surface->centers[f].y()>8. && surface->centers[f].y()<9.; });
    detail.erase(std::find(detail.begin(), detail.end(), hole));
    CHECK(puzzle.refine_detail_region(detail, *surface) == owner);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == before.piece_count());
    CHECK(puzzle.face_piece[hole] == owner);
    CHECK(puzzle.colors.at(owner) == blue);
    CHECK(puzzle.faces(2) == before.faces(2));
    CHECK(puzzle.colors.at(2) == red);
    for (size_t f : detail) CHECK(puzzle.face_piece[f] == owner);
    const auto refined = puzzle.encode();
    CHECK(puzzle.refine_detail_region(detail, *surface) == owner);
    CHECK(puzzle.encode() == refined);
    REQUIRE_THROWS(puzzle.refine_detail_region({}, *surface));
}

TEST_CASE("Facial outlines shrink old eye seeds and recover missing lips without rewriting evidence", "[BeautyWorkbench][BeautyPuzzle]") {
    GUI::LocalSemanticEvidence::Evidence evidence;
    AI::PrintColorRegion eye,skin;
    eye.subject_id=skin.subject_id="person";eye.label="re";skin.label="face";
    eye.faces={0,1,2};skin.faces={3,4};
    evidence.regions={eye,skin};evidence.face_regions={0,0,0,1,1,-1};
    evidence.feature_details={{"person","re",{1,2},{2},2},{"person","ulip",{4,5},{},2}};
    const auto guidance=beauty_feature_guidance(evidence);
    CHECK(guidance.names.at(guidance.labels[0])=="face");
    CHECK(guidance.names.at(guidance.labels[1])=="re");
    CHECK(guidance.names.at(guidance.labels[4])=="ulip");
    CHECK(guidance.names.at(guidance.labels[5])=="ulip");
    REQUIRE(guidance.eyes.size()==1);
    CHECK(guidance.eyes[0].aperture_faces==std::vector<size_t>{1,2});
    CHECK(guidance.eyes[0].iris_faces==std::vector<size_t>{2});
    CHECK(evidence.face_regions==std::vector<int32_t>{0,0,0,1,1,-1});
    const auto hash=std::string(64,'a');
    CHECK(BeautyGuidance::decode(guidance.encode(hash,hash),hash,hash,6).labels==guidance.labels);
}

TEST_CASE("Facial seed cleanup fills local pinholes without erasing a small feature or distant labels", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=puzzle_grid(24,20);
    std::vector<int32_t> labels(surface->centers.size(),0);
    std::vector<uint8_t> band(labels.size(),0);
    for(size_t f=0;f<labels.size();++f) {
        const auto& p=surface->centers[f];
        if(p.x()>6 && p.x()<16 && p.y()>5 && p.y()<15)labels[f]=1;
        band[f]=p.x()>3 && p.x()<19 && p.y()>3 && p.y()<17;
    }
    const auto hole=matching(*surface,[](const Vec3d& p){return p.x()>9 && p.x()<10 && p.y()>8 && p.y()<9;});
    const auto speck=matching(*surface,[](const Vec3d& p){return p.x()>4 && p.x()<5 && p.y()>8 && p.y()<9;});
    const auto remote=matching(*surface,[](const Vec3d& p){return p.x()<1 && p.y()<1;});
    const auto small=matching(*surface,[](const Vec3d& p){return p.x()>17 && p.x()<18 && p.y()>8 && p.y()<9;});
    for(size_t f:hole)labels[f]=0;for(size_t f:speck)labels[f]=1;
    for(size_t f:remote)labels[f]=1;for(size_t f:small)labels[f]=2;
    const auto before=labels;
    beauty_clean_feature_seeds(labels,*surface,band,{0,1,2});
    for(size_t f:hole)CHECK(labels[f]==1);
    for(size_t f:speck)CHECK(labels[f]==0);
    for(size_t f:remote)CHECK(labels[f]==1);
    for(size_t f:small)CHECK(labels[f]==2);
    for(size_t f=0;f<labels.size();++f)if(!band[f])CHECK(labels[f]==before[f]);
}

TEST_CASE("Automatic same-filament specks lose seams while facial parts and different slots survive", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=puzzle_grid(24,20);
    BeautyPuzzle p;p.geometry_id=surface->geometry_id;p.next_id=2;p.face_piece.assign(surface->areas.size(),1);
    p.match_filaments(*surface,{{0,"#282629","PLA",true},{1,"#282629","PLA",true}});
    BeautyGuidance g;g.labels.assign(p.face_piece.size(),0);g.names={"hair","iris"};
    const size_t seed=200;
    REQUIRE(std::all_of(surface->face_neighbors[seed].begin(),surface->face_neighbors[seed].end(),[](int n){return n>=0;}));
    const auto id=p.assign_region({seed},*surface);p.paint_filament(id,0);
    surface->areas[seed]=1e-6; // A tiny triangle inside a much larger surface.
    const auto before=p;
    SECTION("equal physical color preserves every triangle assignment") {
        CHECK(beauty_coalesce_matched_specks(p,*surface,g)==1);
        CHECK(p.piece_count()==1);
        REQUIRE_NOTHROW(p.validate(*surface));
        for(size_t f=0;f<p.face_piece.size();++f) {
            CHECK(p.filament_slots.at(p.face_piece[f])==before.filament_slots.at(before.face_piece[f]));
            CHECK(p.colors.at(p.face_piece[f])==before.colors.at(before.face_piece[f]));
        }
        CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);
    }
    SECTION("a small facial feature is retained even with the same color") {
        g.labels[seed]=1;CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);CHECK(p.same_edit(before));
    }
    SECTION("duplicate RGB does not merge distinct physical slots") {
        p.paint_filament(id,1);CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);CHECK(p.piece_count()==2);
    }
    SECTION("an open or disconnected fragment is not joined across missing edges") {
        surface->face_neighbors[seed]={-1,-1,-1};CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);
    }
    SECTION("a substantial region is retained") {
        surface->areas[seed]=1.;CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);
    }
}

TEST_CASE("Automatic garment seams disappear without changing paint or crossing protected boundaries", "[BeautyPuzzle]") {
    const auto surface=puzzle_grid(24,20);auto p=columns(*surface,12.);
    p.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#FFFFFF","PLA",true}});
    BeautyGuidance g;g.labels.assign(p.face_piece.size(),-1);g.names={"cloth","hair","iris"};
    for(size_t f=0;f<g.labels.size();++f)if(p.face_piece[f]==1)g.labels[f]=0;
    std::vector<RGBA> source(g.labels.size(),RGBA{.8,.8,.8,1});const auto before=p;
    SECTION("connected same pigment becomes one piece and round trips unchanged") {
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==1);CHECK(p.piece_count()==1);
        for(size_t f=0;f<source.size();++f) {
            CHECK(p.colors.at(p.face_piece[f])==before.colors.at(before.face_piece[f]));
            CHECK(p.filament_slots.at(p.face_piece[f])==before.filament_slots.at(before.face_piece[f]));
        }
        REQUIRE_NOTHROW(p.validate(*surface));
        CHECK(BeautyPuzzle::decode(p.encode(),p.geometry_id,source.size()).same_edit(p));
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("hair and clothing remain independently editable") {
        for(size_t f=0;f<source.size();++f)if(p.face_piece[f]!=1)g.labels[f]=1;
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("a facial feature remains protected") {
        g.labels.back()=2;CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("a true source color boundary survives a collapsed palette") {
        for(size_t f=0;f<source.size();++f)if(p.face_piece[f]!=1)source[f]={.15,.15,.15,1};
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("equal RGB with distinct physical slots remains separate") {
        p.paint_filament(2,1);CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("disconnected surfaces remain separate") {
        for(size_t f=0;f<source.size();++f)for(auto& n:surface->face_neighbors[f])if(n>=0 && p.face_piece[f]!=p.face_piece[size_t(n)])n=-1;
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("a sharp junction retains its seam") {
        for(size_t f=0;f<source.size();++f)if(p.face_piece[f]!=1)surface->normals[f]=-surface->normals[f];
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("unrecognized surfaces do not invent clothing") {
        std::fill(g.labels.begin(),g.labels.end(),-1);CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
}

// Explicit local files only; reads a saved result without changing its history.
// Experimental mask-confirmed simplification. Kept in this probe until visual
// and performance gates show that it should enter the normal workbench path.
static size_t probe_coalesce_mask_regions(BeautyPuzzle& puzzle,const BeautySurface& surface,
    const BeautyGuidance& guidance,const std::vector<RGBA>& source,const std::vector<int32_t>& mask_region) {
    const size_t count=puzzle.face_piece.size();
    if(mask_region.size()!=count || source.size()!=count || !guidance.completed(count))throw std::runtime_error("Mask candidate belongs to another surface.");
    struct Part {double area=0,covered=0;std::array<double,3> rgb{};std::map<int32_t,double> mask;std::set<std::string> names;bool protected_part=false;};
    std::map<uint32_t,Part> parts;
    std::set<std::pair<uint32_t,uint32_t>> adjacency;
    for(size_t f=0;f<count;++f) {
        const auto id=puzzle.face_piece[f];auto& part=parts[id];
        const double area=surface.areas[f];part.area+=area;
        for(size_t c=0;c<3;++c)part.rgb[c]+=area*source[f][c];
        if(mask_region[f]>=0){part.mask[mask_region[f]]+=area;part.covered+=area;}
        if(guidance.labels[f]>=0) {
            const auto& name=guidance.names.at(size_t(guidance.labels[f]));
            if(name=="hair" || name=="cloth")part.names.insert(name);else part.protected_part=true;
        }
        for(int32_t n:surface.face_neighbors[f])if(n>=0 && puzzle.face_piece[size_t(n)]!=id)
            adjacency.emplace(std::minmax(id,puzzle.face_piece[size_t(n)]));
    }
    std::map<uint32_t,int32_t> dominant;
    for(auto& entry:parts) {
        auto& part=entry.second;
        if(part.area<=0 || part.protected_part || part.mask.empty())continue;
        for(auto& value:part.rgb)value/=part.area;
        const auto best=std::max_element(part.mask.begin(),part.mask.end(),[](const auto& a,const auto& b){return a.second<b.second;});
        if(best->second>=.8*part.area)dominant[entry.first]=best->first;
    }
    struct Edge {uint32_t a,b;double distance;};std::vector<Edge> edges;
    for(const auto& [a,b]:adjacency) {
        if(!dominant.count(a) || !dominant.count(b) || dominant.at(a)!=dominant.at(b))continue;
        if(parts[a].names.size()>1 || parts[b].names.size()>1)continue;
        auto names=parts[a].names;names.insert(parts[b].names.begin(),parts[b].names.end());
        if(names.size()!=1)continue;
        if(puzzle.filament_slots.at(a)!=puzzle.filament_slots.at(b) || puzzle.colors.at(a)!=puzzle.colors.at(b))continue;
        const double distance=tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(parts[a].rgb,parts[b].rgb);
        if(distance<=12.)edges.push_back({a,b,distance});
    }
    std::sort(edges.begin(),edges.end(),[](const auto& a,const auto& b){return std::tie(a.distance,a.a,a.b)<std::tie(b.distance,b.a,b.b);});
    std::map<uint32_t,uint32_t> parent;std::map<uint32_t,std::vector<uint32_t>> members;
    for(const auto& [id,part]:parts){parent[id]=id;members[id]={id};}
    const auto root=[&](uint32_t id){while(parent[id]!=id){parent[id]=parent[parent[id]];id=parent[id];}return id;};
    size_t merged=0;
    for(const auto& edge:edges) {
        auto a=root(edge.a),b=root(edge.b);if(a==b)continue;
        auto names=parts[a].names;names.insert(parts[b].names.begin(),parts[b].names.end());
        if(names.size()!=1 || dominant.at(a)!=dominant.at(b))continue;
        bool similar=true;
        for(uint32_t x:members[a])for(uint32_t y:members[b])if(
            tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(parts[x].rgb,parts[y].rgb)>12.)similar=false;
        if(!similar)continue;
        if(a>b)std::swap(a,b);
        parent[b]=a;parts[a].names=std::move(names);
        members[a].insert(members[a].end(),members[b].begin(),members[b].end());members[b].clear();
        puzzle.colors.erase(b);puzzle.filament_slots.erase(b);++merged;
    }
    for(auto& id:puzzle.face_piece)id=root(id);
    return merged;
}

TEST_CASE("A saved portrait measures seam simplification with unchanged per-face printing colors", "[.][BeautyPortraitProbe]") {
    const auto env=[](const char* key){const auto p=boost::nowide::getenv(key);return p?std::string(p):std::string();};
    const auto source=env("ORCA_PORTRAIT_SOURCE"),record=env("ORCA_PORTRAIT_RECORD"),output=env("ORCA_PORTRAIT_OUTPUT");
    if(source.empty() || record.empty() || output.empty())SKIP("Explicit source, saved record and new output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    const auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);
    boost::filesystem::ifstream input(record);nlohmann::json root;input>>root;
    const auto& w=root.contains("beauty_workbench")?root.at("beauty_workbench"):root.at("beauty_puzzle_draft");
    const auto& saved=w.at("guidance");
    auto g=BeautyGuidance::decode(saved,surface->geometry_id,saved.at("source"),mesh.its.indices.size());
    // Offline replay of complete worker evidence through the actual native
    // geometry proof and guidance consumer. This never changes an asset record.
    const auto replay=env("ORCA_PORTRAIT_REPLAY");
    if(!replay.empty()) {
        namespace E=Slic3r::GUI::LocalSemanticEvidence;
        namespace G=Slic3r::GUI::LocalSemanticGeometry;
        const auto read=[&](const std::string& name){boost::filesystem::ifstream file(boost::filesystem::path(replay)/name,std::ios::binary);
            REQUIRE(file.good());return std::string(std::istreambuf_iterator<char>(file),{});};
        const auto hash=[](const std::string& value){const auto raw=G::detail::digest(value.data(),value.size(),nullptr);
            std::string result;for(unsigned char c:raw){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;};
        const auto spec=nlohmann::json::parse(read("request.json")),response=nlohmann::json::parse(read("result.json"));
        const auto source_hash=model_artifact_sha256(source);
        REQUIRE(response.at("status")=="ok");REQUIRE(spec.at("source_sha256")==source_hash);
        REQUIRE(spec.at("runtime_fingerprint")==hash(response.at("identity").dump()));
        REQUIRE(spec.at("policy_sha256")==response.at("policy_sha256"));
        const auto packet_bytes=read("rendered.bin"),evidence_bytes=read("evidence.json");
        REQUIRE(response.at("files").at("rendered.bin").at("sha256")==hash(packet_bytes));
        REQUIRE(response.at("files").at("evidence.json").at("sha256")==hash(evidence_bytes));
        G::Packet packet;REQUIRE(G::decode(packet_bytes,source_hash,packet,error));
        E::VerifiedFaceBinding binding;REQUIRE(E::prove_ordered_faces(source_hash,mesh.its,packet.mesh,binding,error));
        E::ExpectedIdentity expected;expected.request_id=spec.at("request_id");expected.source_sha256=source_hash;
        expected.geometry_id=surface->geometry_id;expected.face_count=mesh.its.indices.size();
        expected.weights_sha256=hash(response.at("identity").at("probe_identity").at("weights").dump());
        expected.runtime_sha256=spec.at("runtime_fingerprint");expected.policy_sha256=spec.at("policy_sha256");
        E::Evidence evidence;const bool decoded=E::decode(evidence_bytes,expected,binding,evidence,error);INFO(error);REQUIRE(decoded);
        g=beauty_feature_guidance(evidence,surface.get());
    }
    const auto mouth_faces=beauty_refine_mouth_guidance(*surface,beauty_source_face_colors(mesh.its,colors.vertex_colors),g);
    std::cout<<"Mouth light faces: "<<mouth_faces<<"\n";
    auto p=BeautyPuzzle::decode(w.at("puzzle"),surface->geometry_id,mesh.its.indices.size());const auto before=p;
    const auto removed=beauty_coalesce_matched_specks(p,*surface,g);
    REQUIRE_NOTHROW(p.validate(*surface));
    size_t changed_slots=0,changed_colors=0;
    for(size_t f=0;f<p.face_piece.size();++f) {
        changed_slots+=p.filament_slots.at(p.face_piece[f])!=before.filament_slots.at(before.face_piece[f]);
        changed_colors+=p.colors.at(p.face_piece[f])!=before.colors.at(before.face_piece[f]);
    }
    CHECK(changed_slots==0);CHECK(changed_colors==0);
    const auto coarse=BeautyPuzzle::create_regions(*surface);
    nlohmann::json groups=nlohmann::json::array();
    struct Mean {size_t count=0;double area=0;std::array<double,3> rgb{};};
    std::map<std::pair<int32_t,uint32_t>,Mean> means;
    const auto source_faces=beauty_source_face_colors(mesh.its,colors.vertex_colors);
    if(!env("ORCA_PORTRAIT_EXPORT_SURFACE").empty()) {
        const auto write_binary=[&](const std::string& suffix,const auto& values){
            const auto path=output+suffix;REQUIRE_FALSE(boost::filesystem::exists(path));
            boost::filesystem::ofstream file(path,std::ios::binary);
            file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(values[0])));REQUIRE(file.good());};
        write_binary(".patch.u32",surface->face_patch);write_binary(".neighbors.i32",surface->face_neighbors);
        write_binary(".areas.f64",surface->areas);
        std::vector<float> normals,rgb;normals.reserve(source_faces.size()*3);rgb.reserve(source_faces.size()*3);
        for(size_t f=0;f<source_faces.size();++f)for(size_t c=0;c<3;++c){normals.push_back(float(surface->normals[f][c]));rgb.push_back(source_faces[f][c]);}
        write_binary(".normals.f32",normals);write_binary(".rgb.f32",rgb);
    }
    {auto updated=root;auto& record=updated.contains("beauty_workbench")?updated["beauty_workbench"]:updated["beauty_puzzle_draft"];
        record["guidance"]=g.encode(surface->geometry_id,saved.at("source"));
        boost::filesystem::ofstream file(output+".guidance.json");file<<updated.dump();}
    for(size_t f=0;f<source_faces.size();++f)if(g.labels[f]>=0) {
        auto& mean=means[{g.labels[f],coarse.face_piece[f]}];++mean.count;mean.area+=surface->areas[f];
        for(size_t c=0;c<3;++c)mean.rgb[c]+=surface->areas[f]*source_faces[f][c];
    }
    for(auto& entry:means) {
        for(auto& c:entry.second.rgb)c/=entry.second.area;
        groups.push_back({{"label",g.names.at(size_t(entry.first.first))},{"coarse",entry.first.second},
            {"area",entry.second.area},{"faces",entry.second.count},{"rgb",entry.second.rgb}});
    }
    boost::filesystem::ofstream result(output);
    result<<nlohmann::json({{"before",before.piece_count()},{"after",p.piece_count()},{"removed",removed},
        {"changed_slots",changed_slots},{"changed_colors",changed_colors},{"puzzle",p.encode()},
        {"coarse",coarse.encode()},{"groups",groups}}).dump();
    auto palette=before.palette;auto mixtures=before.mixed_recipes;
    const auto palette_file=env("ORCA_PORTRAIT_PALETTE");
    if(!palette_file.empty()) {
        boost::filesystem::ifstream file(palette_file);nlohmann::json values;file>>values;
        const auto saved_palette=BeautyPuzzle::decode(values.at("puzzle"),surface->geometry_id,mesh.its.indices.size());
        palette=saved_palette.palette;mixtures=saved_palette.mixed_recipes;
        const auto uniform=values.at("uniform_slots").get<std::set<size_t>>();
        for(auto& mix:mixtures)mix.uniform_color=uniform.count(*mix.existing_virtual_slot)!=0;
    }
    auto automatic=BeautyPuzzle::create_regions(*surface,g.labels,{},g.names);auto nearest=automatic;
    nearest.match_filaments(*surface,palette,mixtures,source_faces);
    boost::filesystem::ofstream baseline(output+".nearest.json");baseline<<nearest.encode().dump();
    nlohmann::json audit=nlohmann::json::array();
    std::map<uint32_t,Mean> piece_means;
    std::map<uint32_t,std::map<std::string,double>> piece_labels;
    std::map<uint32_t,std::set<uint32_t>> piece_neighbors;
    for(size_t f=0;f<source_faces.size();++f) {
        const auto id=nearest.face_piece[f];auto& mean=piece_means[id];++mean.count;mean.area+=surface->areas[f];
        for(size_t c=0;c<3;++c)mean.rgb[c]+=surface->areas[f]*source_faces[f][c];
        if(g.labels[f]>=0)piece_labels[id][g.names.at(size_t(g.labels[f]))]+=surface->areas[f];
        for(int32_t n:surface->face_neighbors[f])if(n>=0 && nearest.face_piece[size_t(n)]!=id)piece_neighbors[id].insert(nearest.face_piece[size_t(n)]);
    }
    for(auto& entry:piece_means) {
        for(auto& c:entry.second.rgb)c/=entry.second.area;
        audit.push_back({{"id",entry.first},{"area",entry.second.area},{"rgb",entry.second.rgb},
            {"labels",piece_labels[entry.first]},{"neighbors",piece_neighbors[entry.first]}});
    }
    boost::filesystem::ofstream audit_file(output+".regions.json");audit_file<<audit.dump();
    const auto adjusted=beauty_match_feature_filaments(automatic,*surface,g,source_faces,palette,mixtures);
    std::cout<<"Contrast-adjusted regions: "<<adjusted<<"\n";
    beauty_coalesce_matched_specks(automatic,*surface,g);
    beauty_coalesce_body_regions(automatic,*surface,g,source_faces);REQUIRE_NOTHROW(automatic.validate(*surface));
    const auto mask_file=env("ORCA_PORTRAIT_MASK_REGIONS");
    if(!mask_file.empty()) {
        namespace G=Slic3r::GUI::LocalSemanticGeometry;
        const auto digest=[](const char* data,size_t size){const auto raw=G::detail::digest(data,size,nullptr);
            std::string result;for(unsigned char c:raw){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;};
        boost::filesystem::ifstream file(mask_file,std::ios::binary);
        REQUIRE(file.good());const std::string bytes(std::istreambuf_iterator<char>(file),{});
        REQUIRE(bytes.size()==surface->patches.size()*sizeof(int32_t));
        REQUIRE(digest(bytes.data(),bytes.size())==env("ORCA_PORTRAIT_MASK_SHA256"));
        const auto& patch=surface->face_patch;
        REQUIRE(digest(reinterpret_cast<const char*>(patch.data()),patch.size()*sizeof(uint32_t))==env("ORCA_PORTRAIT_PATCH_SHA256"));
        std::vector<int32_t> patch_region(surface->patches.size());
        std::memcpy(patch_region.data(),bytes.data(),bytes.size());
        std::vector<int32_t> face_region(patch.size());
        for(size_t f=0;f<patch.size();++f)face_region[f]=patch_region[patch[f]];
        const auto before_mask=automatic;
        const size_t joined=probe_coalesce_mask_regions(automatic,*surface,g,source_faces,face_region);
        REQUIRE_NOTHROW(automatic.validate(*surface));
        size_t slot_changes=0,color_changes=0;
        for(size_t f=0;f<automatic.face_piece.size();++f) {
            slot_changes+=automatic.filament_slots.at(automatic.face_piece[f])!=before_mask.filament_slots.at(before_mask.face_piece[f]);
            color_changes+=automatic.colors.at(automatic.face_piece[f])!=before_mask.colors.at(before_mask.face_piece[f]);
        }
        CHECK(slot_changes==0);CHECK(color_changes==0);
        std::cout<<"Mask-confirmed merged regions: "<<joined<<"\n";
    }
    boost::filesystem::ofstream fresh(output+".automatic.json");fresh<<automatic.encode().dump();
}

TEST_CASE("A frozen portrait edit group paints atomically without changing other print faces", "[.][BeautyEditGroupProbe]") {
    const auto env=[](const char* key){const auto p=boost::nowide::getenv(key);return p?std::string(p):std::string();};
    const auto source=env("ORCA_EDIT_SOURCE"),puzzle_file=env("ORCA_EDIT_PUZZLE"),group_file=env("ORCA_EDIT_GROUPS"),output=env("ORCA_EDIT_OUTPUT");
    if(source.empty() || puzzle_file.empty() || group_file.empty() || output.empty())SKIP("Explicit frozen inputs and fresh output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    const auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);
    namespace G=Slic3r::GUI::LocalSemanticGeometry;
    const auto digest=[](const char* data,size_t size){const auto raw=G::detail::digest(data,size,nullptr);
        std::string result;for(unsigned char c:raw){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;};
    REQUIRE(model_artifact_sha256(source)==env("ORCA_EDIT_SOURCE_SHA256"));
    REQUIRE(digest(reinterpret_cast<const char*>(surface->face_patch.data()),surface->face_patch.size()*sizeof(uint32_t))==env("ORCA_EDIT_PATCH_SHA256"));
    boost::filesystem::ifstream saved(puzzle_file);nlohmann::json encoded;saved>>encoded;
    auto puzzle=BeautyPuzzle::decode(encoded,surface->geometry_id,mesh.its.indices.size());
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    boost::filesystem::ifstream mask_input(group_file,std::ios::binary);
    REQUIRE(mask_input.good());
    const std::string bytes(std::istreambuf_iterator<char>(mask_input),{});
    REQUIRE(bytes.size()==surface->patches.size()*sizeof(int32_t));
    REQUIRE(digest(bytes.data(),bytes.size())==env("ORCA_EDIT_GROUP_SHA256"));
    std::vector<int32_t> patch_groups(surface->patches.size());
    std::memcpy(patch_groups.data(),bytes.data(),bytes.size());
    std::map<int32_t,double> group_area;
    for(size_t f=0;f<puzzle.face_piece.size();++f) {
        const int32_t group=patch_groups[surface->face_patch[f]];
        if(group>=0)group_area[group]+=surface->areas[f];
    }
    REQUIRE_FALSE(group_area.empty());
    const auto largest=std::max_element(group_area.begin(),group_area.end(),[](const auto& a,const auto& b){return a.second<b.second;});
    std::vector<size_t> selected;
    for(size_t f=0;f<puzzle.face_piece.size();++f)
        if(patch_groups[surface->face_patch[f]]==largest->first)selected.push_back(f);
    std::map<size_t,size_t> original_slots;
    for(size_t f:selected)++original_slots[puzzle.filament_slots.at(puzzle.face_piece[f])];
    const auto common=std::max_element(original_slots.begin(),original_slots.end(),[](const auto& a,const auto& b){return a.second<b.second;});
    const std::string requested_slot=env("ORCA_EDIT_TARGET_SLOT");
    auto target=std::find_if(puzzle.palette.begin(),puzzle.palette.end(),[&](const auto& channel){
        return channel.compatible && channel.slot!=common->first &&
            (requested_slot.empty() || std::to_string(channel.slot)==requested_slot);
    });
    REQUIRE(target!=puzzle.palette.end());
    const size_t target_slot=target->slot;
    const auto before=puzzle;
    const auto start=std::chrono::steady_clock::now();
    puzzle.paint_faces_filament(selected,*surface,target_slot);
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::vector<uint8_t> chosen(puzzle.face_piece.size(),0);
    for(size_t f:selected)chosen[f]=1;
    size_t untouched_changes=0,selected_changes=0;
    for(size_t f=0;f<puzzle.face_piece.size();++f) {
        const auto old_id=before.face_piece[f],new_id=puzzle.face_piece[f];
        const auto old_slot=before.filament_slots.at(old_id),new_slot=puzzle.filament_slots.at(new_id);
        if(chosen[f]) {CHECK(new_slot==target_slot);selected_changes+=new_slot!=old_slot;}
        else untouched_changes+=new_slot!=old_slot || puzzle.colors.at(new_id)!=before.colors.at(old_id);
    }
    CHECK(untouched_changes==0);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(BeautyPuzzle::decode(puzzle.encode(),puzzle.geometry_id,puzzle.face_piece.size()).same_edit(puzzle));
    boost::filesystem::ofstream result(output);
    result<<nlohmann::json({{"group",largest->first},{"selected_faces",selected.size()},{"original_slots",original_slots},
        {"target_slot",target_slot},{"selected_slot_changes",selected_changes},{"untouched_changes",untouched_changes},
        {"before_pieces",before.piece_count()},{"after_pieces",puzzle.piece_count()},{"paint_seconds",seconds}}).dump(2);
    boost::filesystem::ofstream painted(output+".puzzle.json");painted<<puzzle.encode().dump();
}

TEST_CASE("An unseen portrait builds a native baseline from verified worker evidence", "[.][BeautyHoldoutProbe]") {
    const auto env=[](const char* key){const auto p=boost::nowide::getenv(key);return p?std::string(p):std::string();};
    const auto source=env("ORCA_HOLDOUT_SOURCE"),replay=env("ORCA_HOLDOUT_REPLAY"),palette_file=env("ORCA_HOLDOUT_PALETTE"),output=env("ORCA_HOLDOUT_OUTPUT");
    if(source.empty() || replay.empty() || palette_file.empty() || output.empty())SKIP("Explicit unseen source, worker result, palette and fresh output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    const auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);
    namespace E=Slic3r::GUI::LocalSemanticEvidence;
    namespace G=Slic3r::GUI::LocalSemanticGeometry;
    const auto digest=[](const char* data,size_t size){const auto raw=G::detail::digest(data,size,nullptr);
        std::string result;for(unsigned char c:raw){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;};
    const auto read=[&](const std::string& name){boost::filesystem::ifstream file(boost::filesystem::path(replay)/name,std::ios::binary);
        REQUIRE(file.good());return std::string(std::istreambuf_iterator<char>(file),{});};
    const auto source_hash=model_artifact_sha256(source);
    REQUIRE(source_hash==env("ORCA_HOLDOUT_SOURCE_SHA256"));
    const auto spec=nlohmann::json::parse(read("request.json")),response=nlohmann::json::parse(read("result.json"));
    REQUIRE(response.at("status")=="ok");REQUIRE(spec.at("source_sha256")==source_hash);
    const auto identity_text=response.at("identity").dump();
    REQUIRE(spec.at("runtime_fingerprint")==digest(identity_text.data(),identity_text.size()));
    REQUIRE(spec.at("policy_sha256")==response.at("policy_sha256"));
    const auto packet_bytes=read("rendered.bin"),evidence_bytes=read("evidence.json");
    REQUIRE(response.at("files").at("rendered.bin").at("sha256")==digest(packet_bytes.data(),packet_bytes.size()));
    REQUIRE(response.at("files").at("evidence.json").at("sha256")==digest(evidence_bytes.data(),evidence_bytes.size()));
    G::Packet packet;REQUIRE(G::decode(packet_bytes,source_hash,packet,error));
    E::VerifiedFaceBinding binding;REQUIRE(E::prove_ordered_faces(source_hash,mesh.its,packet.mesh,binding,error));
    E::ExpectedIdentity expected;expected.request_id=spec.at("request_id");expected.source_sha256=source_hash;
    expected.geometry_id=surface->geometry_id;expected.face_count=mesh.its.indices.size();
    const auto weights_text=response.at("identity").at("probe_identity").at("weights").dump();
    expected.weights_sha256=digest(weights_text.data(),weights_text.size());
    expected.runtime_sha256=spec.at("runtime_fingerprint");expected.policy_sha256=spec.at("policy_sha256");
    E::Evidence evidence;const bool decoded=E::decode(evidence_bytes,expected,binding,evidence,error);INFO(error);REQUIRE(decoded);
    auto guidance=beauty_feature_guidance(evidence,surface.get());
    const auto source_faces=beauty_source_face_colors(mesh.its,colors.vertex_colors);
    beauty_refine_mouth_guidance(*surface,source_faces,guidance);
    boost::filesystem::ifstream palette_input(palette_file);REQUIRE(palette_input.good());
    const std::string palette_bytes(std::istreambuf_iterator<char>(palette_input),{});
    REQUIRE(digest(palette_bytes.data(),palette_bytes.size())==env("ORCA_HOLDOUT_PALETTE_SHA256"));
    const auto palette_json=nlohmann::json::parse(palette_bytes).at("palette");
    std::vector<PhysicalFilamentChannel> palette;
    for(const auto& entry:palette_json)palette.push_back({entry.at("slot"),entry.at("color"),entry.at("material"),entry.at("compatible")});
    REQUIRE(is_valid_physical_channel_set(palette));
    auto puzzle=BeautyPuzzle::create_regions(*surface,guidance.labels,{},guidance.names);
    beauty_match_feature_filaments(puzzle,*surface,guidance,source_faces,palette,{});
    beauty_coalesce_matched_specks(puzzle,*surface,guidance);
    beauty_coalesce_body_regions(puzzle,*surface,guidance,source_faces);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(BeautyPuzzle::decode(puzzle.encode(),puzzle.geometry_id,puzzle.face_piece.size()).same_edit(puzzle));
    const auto write_binary=[&](const std::string& suffix,const auto& values){
        const auto path=output+suffix;REQUIRE_FALSE(boost::filesystem::exists(path));
        boost::filesystem::ofstream file(path,std::ios::binary);
        file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(values[0])));REQUIRE(file.good());};
    write_binary(".patch.u32",surface->face_patch);write_binary(".neighbors.i32",surface->face_neighbors);
    write_binary(".areas.f64",surface->areas);
    std::vector<float> normals,rgb;normals.reserve(source_faces.size()*3);rgb.reserve(source_faces.size()*3);
    for(size_t f=0;f<source_faces.size();++f)for(size_t c=0;c<3;++c){normals.push_back(float(surface->normals[f][c]));rgb.push_back(source_faces[f][c]);}
    write_binary(".normals.f32",normals);write_binary(".rgb.f32",rgb);
    boost::filesystem::ofstream saved(output+".guidance.json");saved<<nlohmann::json({{"beauty_workbench",{{"guidance",guidance.encode(surface->geometry_id,source_hash)}}}}).dump();
    boost::filesystem::ofstream automatic(output+".automatic.json");automatic<<puzzle.encode().dump();
    boost::filesystem::ofstream result(output);result<<nlohmann::json({{"face_count",puzzle.face_piece.size()},{"piece_count",puzzle.piece_count()},
        {"geometry_id",surface->geometry_id},{"source_sha256",source_hash},{"palette_sha256",env("ORCA_HOLDOUT_PALETTE_SHA256")}}).dump(2);
}

TEST_CASE("Drawing a region spans old pieces and keeps original texture with independent disconnected selections", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 3.);
    puzzle.paint(1, blue); puzzle.paint(2, red);
    const auto crossing = matching(*surface, [](const Vec3d& c) { return c.y() > 1. && c.y() < 3.; });
    const uint32_t created = puzzle.assign_region(crossing, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.faces(created).size() == crossing.size());
    CHECK(puzzle.colors.count(created) == 0);
    for (size_t f : crossing) CHECK(puzzle.face_piece[f] == created);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f)
        if (surface->centers[f].y() < 1. || surface->centers[f].y() > 3.)
            CHECK(puzzle.colors.at(puzzle.face_piece[f]) == (surface->centers[f].x() < 3. ? blue : red));

    const auto before = puzzle.encode();
    REQUIRE_THROWS(puzzle.assign_region({}, *surface));
    CHECK(puzzle.encode() == before);
    REQUIRE_THROWS(puzzle.assign_region({0, surface->face_patch.size()}, *surface));
    CHECK(puzzle.encode() == before);
    const auto corners = matching(*surface, [](const Vec3d& c) {
        return (c.x() < 1. && c.y() < 1.) || (c.x() > 4. && c.y() > 3.);
    });
    const uint32_t drawn = puzzle.assign_region(corners, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.faces(drawn).size() == 4);
    std::set<uint32_t> new_ids;
    for (size_t f : corners) { new_ids.insert(puzzle.face_piece[f]); CHECK(puzzle.colors.count(puzzle.face_piece[f]) == 0); }
    CHECK(new_ids.size() == 2);
    auto exhausted = puzzle; exhausted.next_id = BeautyPuzzle::max_id;
    const auto full = exhausted.encode();
    REQUIRE_THROWS(exhausted.assign_region(crossing, *surface));
    CHECK(exhausted.encode() == full);
}
