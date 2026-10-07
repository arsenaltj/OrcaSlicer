#include <catch2/catch_all.hpp>
#include "slic3r/AI/AppearanceEditing/BeautyPuzzle.hpp"
#include "slic3r/AI/AppearanceEditing/BeautyEditRegions.hpp"
#include <numeric>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdlib.hpp>
#include "slic3r/AI/ModelArtifacts/ModelArtifact.hpp"

using namespace Slic3r;
using namespace Slic3r::AI;

namespace {
indexed_triangle_set curve_grid(float scale=1.f,bool bent=false) {
    indexed_triangle_set mesh;const int width=80,height=60;
    for(int y=0;y<=height;++y)for(int x=0;x<=width;++x)
        mesh.vertices.emplace_back(scale*x,scale*y,bent?scale*.004f*(x-40)*(x-40):0.f);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        const int a=y*(width+1)+x;
        mesh.indices.emplace_back(a,a+1,a+width+2);mesh.indices.emplace_back(a,a+width+2,a+width+1);
    }
    return mesh;
}
BeautyEditRegions lip_curve_region(const BeautySurface& surface,float scale=1.f) {
    BeautyEditRegions regions{surface.geometry_id,std::string(64,'a'),{}};
    for(const auto& center:surface.centers) {
        const double x=center.x()/scale-40.,y=center.y()/scale-30.;
        regions.face_region.push_back(x*x/400.+y*y/64.<1.?2:1);
    }
    return regions;
}
BeautyPuzzle curve_printing(const BeautySurface& surface) {
    BeautyPuzzle result;result.geometry_id=surface.geometry_id;
    result.face_piece.assign(surface.areas.size(),1);result.next_id=2;
    result.palette={{0,"#E8B49A","PLA",true},{1,"#B9514A","PLA",true}};
    result.paint_filament(1,0);return result;
}
}

TEST_CASE("A fitted lip curve changes actual selected-face paint and survives region and material saving", "[BeautyRegionCurves]") {
    const auto mesh=curve_grid();const auto surface=BeautySurface::build_for_appearance(mesh,{});
    auto regions=lip_curve_region(*surface);auto printing=curve_printing(*surface);
    regions.paint_filament(printing,*surface,regions.source_sha256,2,1);
    const auto old_regions=regions;const auto old_print=printing.encode();
    const size_t changed=regions.smooth_curve_boundary(printing,mesh,*surface,regions.source_sha256,2);
    REQUIRE(changed>0);CHECK(printing.encode()==old_print);
    regions.paint_filament(printing,*surface,regions.source_sha256,2,1);
    // A second explicit assignment restores the adjacent skin region, including
    // faces released by the lip curve. Both assignments use new ownership.
    regions.paint_filament(printing,*surface,regions.source_sha256,1,0);
    size_t material_changes=0;
    const auto before=BeautyPuzzle::decode(old_print,surface->geometry_id,mesh.indices.size());
    for(size_t f=0;f<mesh.indices.size();++f) {
        const auto slot=printing.filament_slots.at(printing.face_piece[f]);
        CHECK(slot==(regions.face_region[f]==2?1:0));
        material_changes+=slot!=before.filament_slots.at(before.face_piece[f]);
        if(regions.face_region[f]!=old_regions.face_region[f])CHECK(std::abs(surface->centers[f].y()-30.)<11.);
    }
    CHECK(material_changes==changed);CHECK_NOTHROW(printing.validate(*surface));
    const auto saved=BeautyEditRegions::decode(regions.encode(),surface->geometry_id,regions.source_sha256,mesh.indices.size());
    const auto restored=BeautyPuzzle::decode(printing.encode(),surface->geometry_id,mesh.indices.size());
    CHECK(saved.face_region==regions.face_region);CHECK(restored.same_edit(printing));
    auto corrected=regions;
    size_t face=0;while(face<regions.face_region.size() && regions.face_region[face]==old_regions.face_region[face])++face;
    REQUIRE(face<regions.face_region.size());
    // Hand correction after fitting remains ordinary editable region state.
    corrected.face_region[face]=old_regions.face_region[face];
    CHECK(BeautyEditRegions::decode(corrected.encode(),surface->geometry_id,regions.source_sha256,mesh.indices.size()).face_region==corrected.face_region);
}

TEST_CASE("Explicit curve color extension updates only transferred faces and preserves eye detail holes", "[BeautyRegionCurves]") {
    const auto mesh=curve_grid(1.f,true);const auto surface=BeautySurface::build_for_appearance(mesh,{});
    auto regions=lip_curve_region(*surface);auto printing=curve_printing(*surface);
    std::vector<uint8_t> anchors(mesh.indices.size(),0);
    for(size_t f=0;f<anchors.size();++f) {
        const auto& c=surface->centers[f];
        if(std::abs(c.x()-40)<3 && std::abs(c.y()-30)<2){regions.face_region[f]=3;anchors[f]=1;}
    }
    regions.paint_filament(printing,*surface,regions.source_sha256,2,1);
    const auto before_regions=regions;const auto before_print=printing;
    REQUIRE(regions.smooth_curve_boundary(printing,mesh,*surface,regions.source_sha256,2,
        BeautyEditRegions::BoundaryPaintMode::ExtendAdjacentColors,anchors)>0);
    for(size_t f=0;f<anchors.size();++f) {
        const auto slot=printing.filament_slots.at(printing.face_piece[f]);
        if(anchors[f])CHECK(regions.face_region[f]==3);
        if(regions.face_region[f]==before_regions.face_region[f])
            CHECK(slot==before_print.filament_slots.at(before_print.face_piece[f]));
        else CHECK(slot==(regions.face_region[f]==2?1:0));
    }
    CHECK_NOTHROW(printing.validate(*surface));
}

TEST_CASE("Curve ownership does not depend on uniform model scale or face ordinal order", "[BeautyRegionCurves]") {
    std::vector<uint32_t> reference;
    for(float scale:{.1f,1.f,10.f}) {
        auto mesh=curve_grid(scale);const auto surface=BeautySurface::build_for_appearance(mesh,{});
        auto regions=lip_curve_region(*surface,scale);auto printing=curve_printing(*surface);
        REQUIRE(regions.smooth_curve_boundary(printing,mesh,*surface,regions.source_sha256,2)>0);
        if(reference.empty())reference=regions.face_region;else CHECK(regions.face_region==reference);
        const size_t shift=173;
        std::rotate(mesh.indices.begin(),mesh.indices.begin()+shift,mesh.indices.end());
        const auto reordered=BeautySurface::build_for_appearance(mesh,{});
        auto permuted=lip_curve_region(*reordered,scale);auto other=curve_printing(*reordered);
        REQUIRE(permuted.smooth_curve_boundary(other,mesh,*reordered,permuted.source_sha256,2)>0);
        auto expected=reference;std::rotate(expected.begin(),expected.begin()+shift,expected.end());
        CHECK(permuted.face_region==expected);
    }
}

TEST_CASE("Curve fitting protects anchors and folds and rejects stale surfaces without partial edits", "[BeautyRegionCurves]") {
    const auto mesh=curve_grid();const auto surface=BeautySurface::build_for_appearance(mesh,{});
    auto regions=lip_curve_region(*surface);auto printing=curve_printing(*surface);
    const auto old_regions=regions.encode(),old_print=printing.encode();
    CHECK(regions.smooth_curve_boundary(printing,mesh,*surface,regions.source_sha256,2,
        BeautyEditRegions::BoundaryPaintMode::PreserveColors,std::vector<uint8_t>(mesh.indices.size(),1))==0);
    auto stale=*surface;stale.geometry_id="different";
    CHECK_THROWS(regions.smooth_curve_boundary(printing,mesh,stale,regions.source_sha256,2));
    CHECK_THROWS(regions.smooth_curve_boundary(printing,mesh,*surface,regions.source_sha256,2,
        BeautyEditRegions::BoundaryPaintMode::PreserveColors,{1}));
    size_t checkpoints=0;
    CHECK_THROWS(regions.smooth_curve_boundary(printing,mesh,*surface,regions.source_sha256,2,
        BeautyEditRegions::BoundaryPaintMode::PreserveColors,{},[&]{return ++checkpoints>8;}));
    CHECK(regions.encode()==old_regions);CHECK(printing.encode()==old_print);
    const auto cube=its_make_cube(10,10,10);const auto folded=BeautySurface::build_for_appearance(cube,{});
    auto fold_print=curve_printing(*folded);
    BeautyEditRegions fold_regions{folded->geometry_id,regions.source_sha256,std::vector<uint32_t>(cube.indices.size(),1)};
    fold_regions.face_region[0]=2;
    CHECK(fold_regions.smooth_curve_boundary(fold_print,cube,*folded,regions.source_sha256,2)==0);
}

TEST_CASE("Fragmented manual curve selections retain a local paint donor during joint inward and outward moves", "[BeautyRegionCurves]") {
    const auto mesh=curve_grid();const auto surface=BeautySurface::build_for_appearance(mesh,{});
    for(unsigned seed=1;seed<=12;++seed) {
        auto regions=lip_curve_region(*surface);auto printing=curve_printing(*surface);
        uint32_t random=seed;
        for(size_t f=0;f<regions.face_region.size();++f) {
            random=random*1664525u+1013904223u;
            const auto& c=surface->centers[f];
            if(c.x()>17 && c.x()<63 && c.y()>19 && c.y()<41 && (random>>24)<35)
                regions.face_region[f]=regions.face_region[f]==2?1:2;
        }
        regions.paint_filament(printing,*surface,regions.source_sha256,2,1);
        REQUIRE_NOTHROW(regions.smooth_curve_boundary(printing,mesh,*surface,regions.source_sha256,2,
            BeautyEditRegions::BoundaryPaintMode::ExtendAdjacentColors));
        for(size_t f=0;f<regions.face_region.size();++f)
            CHECK(printing.filament_slots.at(printing.face_piece[f])==(regions.face_region[f]==2?1:0));
        CHECK_NOTHROW(printing.validate(*surface));
    }
}

TEST_CASE("Selected curve fitting keeps multi-region junction ownership and identical UV seam partitions", "[BeautyRegionCurves]") {
    const auto mesh=curve_grid();const auto surface=BeautySurface::build_for_appearance(mesh,{});
    auto regions=lip_curve_region(*surface);auto printing=curve_printing(*surface);
    for(size_t f=0;f<regions.face_region.size();++f) {
        const auto& c=surface->centers[f];if(c.x()>52 && c.y()>30)regions.face_region[f]=3;
    }
    const auto original=regions;
    REQUIRE(regions.smooth_curve_boundary(printing,mesh,*surface,regions.source_sha256,2)>0);
    for(size_t f=0;f<regions.face_region.size();++f) {
        std::set<uint32_t> owners{original.face_region[f]};
        for(int32_t n:surface->face_neighbors[f])if(n>=0)owners.insert(original.face_region[size_t(n)]);
        if(owners.size()>2)CHECK(regions.face_region[f]==original.face_region[f]);
    }
    indexed_triangle_set seams;
    for(const auto& triangle:mesh.indices) {
        const int base=int(seams.vertices.size());
        for(int c=0;c<3;++c)seams.vertices.push_back(mesh.vertices[size_t(triangle[c])]);
        seams.indices.emplace_back(base,base+1,base+2);
    }
    const auto seam_surface=BeautySurface::build_for_appearance(seams,{});auto seam_print=curve_printing(*seam_surface);
    auto seam_regions=original;seam_regions.geometry_id=seam_surface->geometry_id;
    REQUIRE(seam_regions.smooth_curve_boundary(seam_print,seams,*seam_surface,seam_regions.source_sha256,2)>0);
    CHECK(seam_regions.face_region==regions.face_region);
}

namespace {
const std::string source_hash(64,'a');
indexed_triangle_set grid(int width=40,int height=30) {
    indexed_triangle_set mesh;
    for(int y=0;y<=height;++y)for(int x=0;x<=width;++x)mesh.vertices.emplace_back(float(x),float(y),0);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        const int a=y*(width+1)+x;mesh.indices.emplace_back(a,a+1,a+width+2);mesh.indices.emplace_back(a,a+width+2,a+width+1);
    }
    return mesh;
}
BeautyPuzzle matched(const BeautySurface& surface) {
    BeautyPuzzle p;p.geometry_id=surface.geometry_id;p.next_id=2;p.face_piece.assign(surface.areas.size(),1);
    p.palette={{7,"#FF8080","PLA",true},{41,"#202020","PLA",true}};
    p.paint_filament(1,7);p.target_colors[1]=BeautyPuzzle::filament_color(p.palette[0]);return p;
}
size_t color_edges(const BeautyPuzzle& p,const BeautySurface& s) {
    size_t edges=0;
    for(size_t f=0;f<p.face_piece.size();++f)for(int32_t n:s.face_neighbors[f])
        if(n>=0 && size_t(n)>f && p.filament_slots.at(p.face_piece[f])!=p.filament_slots.at(p.face_piece[size_t(n)]))++edges;
    return edges;
}
}

TEST_CASE("Moving an edit boundary preserves all print colors until local color extension is requested", "[BeautyRegionRefinement][BeautyEditRegions]") {
    BeautySurface s;s.geometry_id="eye-color-strip";s.face_patch.assign(8,0);s.patches.resize(1);s.areas.assign(8,1);
    for(int i=0;i<8;++i){s.centers.emplace_back(i,0,0);s.normals.emplace_back(0,0,1);s.face_neighbors.push_back({i?i-1:-1,i<7?i+1:-1,-1});}
    auto p=matched(s);p.face_piece={1,1,1,1,1,2,3,3};p.next_id=4;p.paint_filament(2,41);p.paint_filament(3,7);
    BeautyEditRegions editing{s.geometry_id,source_hash,{5,5,5,5,5,5,6,6}};const auto before=p.encode();
    auto labels_only=editing;
    REQUIRE(labels_only.reshape(p,s,source_hash,5,{6},{}));
    CHECK(labels_only.at(6)==5);CHECK(p.encode()==before);
    auto extended=editing;
    REQUIRE(extended.reshape(p,s,source_hash,5,{6},{},BeautyEditRegions::BoundaryPaintMode::ExtendAdjacentColors));
    // The receiving group is mostly pink. Its actual touching rim is black.
    CHECK(p.filament_slots.at(p.face_piece[6])==41);CHECK(p.filament_slots.at(p.face_piece[7])==7);
    CHECK_NOTHROW(p.validate(s));
    CHECK(BeautyPuzzle::decode(p.encode(),s.geometry_id,8).same_edit(p));
    const auto frozen=labels_only.encode();const auto printed=p.encode();
    CHECK_THROWS(labels_only.reshape(p,s,source_hash,5,{999},{}));CHECK(labels_only.encode()==frozen);CHECK(p.encode()==printed);
}

TEST_CASE("Drawing an editing group retains its multi-color print pieces and original texture intent", "[BeautyRegionRefinement][BeautyEditRegions]") {
    const auto s=BeautySurface::build(grid(),{});auto p=matched(*s);
    p.paint_faces_filament({100,101},*s,41);const auto frozen=p.encode();
    BeautyEditRegions editing{p.geometry_id,source_hash,p.face_piece};
    const auto id=editing.assign_region(p,source_hash,{99,100,101,102});
    CHECK(editing.faces(id)==std::vector<size_t>{99,100,101,102});CHECK(p.encode()==frozen);
    auto restored=BeautyEditRegions::decode(editing.encode(),p.geometry_id,source_hash,p.face_piece.size());
    CHECK(restored.face_region==editing.face_region);
}

TEST_CASE("Local cleanup removes automatic speckles while protecting pupils hand paint unknown regions and source geometry", "[BeautyRegionRefinement]") {
    const auto mesh=grid();const auto s=BeautySurface::build(mesh,{});auto p=matched(*s);
    const size_t speck=1230,pupil=1250,manual=1270;
    for(size_t f:{speck,pupil,manual})p.paint_faces_filament({f},*s,41);
    for(size_t f:{speck,pupil})p.target_colors[p.face_piece[f]]=BeautyPuzzle::filament_color(p.palette[1]);
    std::vector<size_t> all(p.face_piece.size());std::iota(all.begin(),all.end(),0);
    std::vector<int32_t> labels(all.size(),0);labels[pupil]=1;
    const auto protection=BeautyPuzzle::refinement_protection(*s,all,labels,{"face","iris"},false);
    const auto original=p;const auto patches=s->face_patch;
    CHECK(p.clean_color_islands(*s,all,protection)==1);
    CHECK(p.filament_slots.at(p.face_piece[speck])==7);CHECK(p.filament_slots.at(p.face_piece[pupil])==41);
    CHECK(p.filament_slots.at(p.face_piece[manual])==41);CHECK(s->face_patch==patches);
    auto unknown=original;const auto unknown_protection=BeautyPuzzle::refinement_protection(*s,all,{}, {},false);
    CHECK(unknown.clean_color_islands(*s,all,unknown_protection)==0);CHECK(unknown.same_edit(original));
    auto canceled=original;CHECK_THROWS(canceled.clean_color_islands(*s,all,protection,[]{return true;}));CHECK(canceled.same_edit(original));
    const auto saved=BeautyPuzzle::decode(p.encode(),s->geometry_id,all.size());CHECK(saved.same_edit(p));
}

TEST_CASE("Local color boundary repair changes real face slots and retains protected features and distant colors", "[BeautyRegionRefinement]") {
    const auto mesh=grid();const auto s=BeautySurface::build(mesh,{});auto p=matched(*s);p.next_id=4;
    for(size_t f=0;f<p.face_piece.size();++f) {
        const auto& c=s->centers[f];p.face_piece[f]=c.x()<20?1:2;
        if(c.x()<22 && int(c.y())%4<2)p.face_piece[f]=1;
        if(c.x()>4 && c.x()<6 && c.y()>4 && c.y()<5)p.face_piece[f]=3;
    }
    p.paint_filament(1,41);p.target_colors[1]=BeautyPuzzle::filament_color(p.palette[1]);
    p.paint_filament(2,7);p.target_colors[2]=BeautyPuzzle::filament_color(p.palette[0]);
    p.paint_filament(3,7);const auto pupil=p.faces(3);
    const auto before=p;const auto old_edges=color_edges(p,*s);const auto selected=p.faces(1);
    REQUIRE(p.smooth_selected_boundaries(*s,selected)>0);CHECK(color_edges(p,*s)<old_edges);
    CHECK(p.faces(3)==pupil);CHECK(p.same_palette(before.palette));CHECK(p.colors==before.colors);
    for(size_t f=0;f<p.face_piece.size();++f)if(s->centers[f].x()>30)
        CHECK(p.filament_slots.at(p.face_piece[f])==before.filament_slots.at(before.face_piece[f]));
    CHECK_NOTHROW(p.validate(*s));CHECK(BeautyPuzzle::decode(p.encode(),p.geometry_id,p.face_piece.size()).same_edit(p));
    auto canceled=before;CHECK_THROWS(canceled.smooth_selected_boundaries(*s,selected,{},[]{return true;}));CHECK(canceled.same_edit(before));
}

TEST_CASE("Explicit RGB targets and repaired historical intent remain protected using the immutable original", "[BeautyRegionRefinement]") {
    const auto s=BeautySurface::build(grid(),{});auto p=matched(*s);
    std::vector<RGBA> original(p.face_piece.size(),BeautyPuzzle::filament_color(p.palette[0]));
    const size_t f=1230;p.paint_faces_target({f},*s,BeautyPuzzle::filament_color(p.palette[1]));
    std::vector<size_t> all(original.size());std::iota(all.begin(),all.end(),0);
    const auto protected_faces=p.protect_color_intent(*s,original,std::vector<uint8_t>(original.size(),0));
    CHECK(protected_faces[f]==1);
    CHECK(p.clean_color_islands(*s,all,protected_faces)==0);
    auto invalid=original;invalid.pop_back();CHECK_THROWS(p.protect_color_intent(*s,invalid,protected_faces));
}

TEST_CASE("Recovering original color details preserves editing groups manual colors outside slots and saved source targets", "[BeautyRegionRefinement][SourceColorDetails]") {
    const auto s=BeautySurface::build(grid(),{});auto p=matched(*s);p.palette.push_back({93,"#FFFFFF","PLA",true});
    std::vector<RGBA> original(p.face_piece.size(),BeautyPuzzle::filament_color(p.palette[0]));
    std::vector<size_t> selected,detail;
    for(size_t f=0;f<original.size();++f) {
        const auto& c=s->centers[f];if(c.x()>10 && c.x()<30 && c.y()>8 && c.y()<20)selected.push_back(f);
        if(c.x()>17 && c.x()<20 && c.y()>12 && c.y()<14){original[f]=BeautyPuzzle::filament_color(p.palette[2]);detail.push_back(f);}
    }
    double sum[3]{};for(const auto& c:original)for(size_t i=0;i<3;++i)sum[i]+=c[i];
    p.target_colors[1]={float(sum[0]/original.size()),float(sum[1]/original.size()),float(sum[2]/original.size()),1};
    const size_t manual=detail.front();p.paint_faces_filament({manual},*s,41);
    // Recompute the remaining automatic target after explicit material paint.
    const auto remaining=p.faces(1);std::fill(std::begin(sum),std::end(sum),0.);
    for(size_t f:remaining)for(size_t i=0;i<3;++i)sum[i]+=original[f][i];
    p.target_colors[1]={float(sum[0]/remaining.size()),float(sum[1]/remaining.size()),float(sum[2]/remaining.size()),1};
    const auto before=p;BeautyEditRegions groups{p.geometry_id,source_hash,p.face_piece};const auto grouped=groups.encode();
    REQUIRE(p.recover_source_color_details(*s,selected,original)>0);
    CHECK(groups.encode()==grouped);CHECK(p.filament_slots.at(p.face_piece[manual])==41);
    for(size_t f=0;f<p.face_piece.size();++f)if(std::find(selected.begin(),selected.end(),f)==selected.end())
        CHECK(p.filament_slots.at(p.face_piece[f])==before.filament_slots.at(before.face_piece[f]));
    CHECK_NOTHROW(p.validate(*s));CHECK(BeautyPuzzle::decode(p.encode(),p.geometry_id,p.face_piece.size()).same_edit(p));
    auto canceled=before;CHECK_THROWS(canceled.recover_source_color_details(*s,selected,original,{},[]{return true;}));
    CHECK(canceled.same_edit(before));
    // Same-palette reopen does not repaint the explicitly recovered result.
    auto restored=p;restored.match_filaments(*s,p.palette,{},original);CHECK(restored.same_edit(p));
}

TEST_CASE("Explicit portrait material correction survives save recovery and rematch without overwriting manual or editing intent", "[PortraitColorConstraints][BeautyRegionRefinement]") {
    const auto mesh=grid();const auto s=BeautySurface::build(mesh,{});auto p=matched(*s);
    p.palette={{7,"#E8B49A","PLA",true},{41,"#B9514A","PLA",true},{93,"#F6F7F9","PLA",true},{105,"#70533E","PLA",true},{117,"#282629","PLA",true}};
    p.paint_filament(1,41);
    std::vector<RGBA> original(p.face_piece.size(),BeautyPuzzle::filament_color(p.palette[3]));
    std::vector<int32_t> labels(original.size(),0);std::vector<size_t> selected;
    for(size_t f=0;f<original.size();++f) {
        const auto& c=s->centers[f];if(c.x()>10 && c.x()<30 && c.y()>8 && c.y()<20)selected.push_back(f);
        if(c.x()>17 && c.x()<20 && c.y()>12 && c.y()<14){labels[f]=1;original[f]=BeautyPuzzle::filament_color(p.palette[1]);}
    }
    const auto manual=selected.front();p.face_piece[manual]=2;p.next_id=3;p.paint_filament(2,41);
    std::array<double,3> sum{};double area=0;
    for(size_t f:p.faces(1)){area+=s->areas[f];for(size_t c=0;c<3;++c)sum[c]+=s->areas[f]*original[f][c];}
    p.target_colors[1]={float(sum[0]/area),float(sum[1]/area),float(sum[2]/area),1};
    const auto before=p;BeautyEditRegions editing{p.geometry_id,source_hash,p.face_piece};const auto grouped=editing.encode();
    REQUIRE(p.constrain_selected_colors(*s,selected,original,labels,{"face","ulip"})>0);
    std::vector<size_t> corrected;
    for(size_t f=0;f<original.size();++f) {
        const bool changed=p.filament_slots.at(p.face_piece[f])!=before.filament_slots.at(before.face_piece[f]);
        if(changed){corrected.push_back(f);CHECK(std::find(selected.begin(),selected.end(),f)!=selected.end());CHECK(labels[f]!=1);CHECK_FALSE(p.target_colors.count(p.face_piece[f]));}
    }
    CHECK(p.filament_slots.at(p.face_piece[manual])==41);CHECK(editing.encode()==grouped);CHECK(s->face_patch.size()==original.size());
    CHECK_NOTHROW(p.validate(*s));CHECK(BeautyPuzzle::decode(p.encode(),p.geometry_id,original.size()).same_edit(p));
    CHECK(p.constrain_selected_colors(*s,selected,original,labels,{"face","ulip"})==0);
    p.recover_source_color_details(*s,selected,original);
    for(size_t f:corrected)CHECK(p.filament_slots.at(p.face_piece[f])==105);
    auto restored=p;restored.match_filaments(*s,p.palette,{},original);CHECK(restored.same_edit(p));
    auto unknown=before;CHECK(unknown.constrain_selected_colors(*s,selected,original,std::vector<int32_t>(original.size(),-1),{})==0);CHECK(unknown.same_edit(before));
    auto canceled=before;CHECK_THROWS(canceled.constrain_selected_colors(*s,selected,original,labels,{"face","ulip"},{},{},[]{return true;}));CHECK(canceled.same_edit(before));
    auto bad=labels;bad[0]=5;CHECK_THROWS(canceled.constrain_selected_colors(*s,selected,original,bad,{"face","ulip"}));CHECK(canceled.same_edit(before));
    auto invalid_metrics=*s;invalid_metrics.face_edge_lengths.pop_back();
    auto invalid=before;CHECK_THROWS(invalid.constrain_selected_colors(invalid_metrics,selected,original,labels,{"face","ulip"}));CHECK(invalid.same_edit(before));
    auto legacy=before;ColorMatching::PortraitColorConstraintOptions policy;policy.geometric_coherence=false;
    CHECK(legacy.constrain_selected_colors(*s,selected,original,labels,{"face","ulip"},policy)>0);
}

TEST_CASE("A synthetic facial editing sample exports actual cleanup and repaired face colors", "[.][BeautyRefinementVisualProbe]") {
    const char* raw=boost::nowide::getenv("ORCA_REFINEMENT_OUTPUT");
    if(!raw)SKIP("Explicit new diagnostic output directory required.");
    const boost::filesystem::path output{std::string(raw)};
    REQUIRE_FALSE(boost::filesystem::exists(output));REQUIRE(boost::filesystem::create_directories(output));
    const auto mesh=grid(120,80);const auto surface=BeautySurface::build_for_appearance(mesh,{});
    auto p=matched(*surface);p.palette.push_back({91,"#C8445B","PLA",true});p.palette.push_back({103,"#FFFFFF","PLA",true});p.next_id=10;
    std::vector<int32_t> labels(p.face_piece.size(),0);
    std::vector<size_t> all(p.face_piece.size());std::iota(all.begin(),all.end(),0);
    const auto ellipse=[](double x,double y,double cx,double cy,double rx,double ry) {
        return std::pow((x-cx)/rx,2)+std::pow((y-cy)/ry,2)<1.;
    };
    for(size_t f=0;f<p.face_piece.size();++f) {
        const auto& c=surface->centers[f];const double x=c.x(),y=c.y();
        if(ellipse(x,y,35,54,12+((int(y)/2)%2),7)){p.face_piece[f]=2;labels[f]=1;}
        if(ellipse(x,y,85,54,12+((int(y)/2)%2),7)){p.face_piece[f]=3;labels[f]=1;}
        if(ellipse(x,y,60,29,24+((int(y)/2)%2),6)){p.face_piece[f]=4;labels[f]=2;}
        if(ellipse(x,y,60,29,18,2.5)){p.face_piece[f]=5;labels[f]=3;}
    }
    const std::array<size_t,3> specks{size_t((20*120+20)*2),size_t((15*120+60)*2),size_t((20*120+100)*2)};
    for(size_t i=0;i<specks.size();++i)p.face_piece[specks[i]]=uint32_t(6+i);
    for(uint32_t id:{2,3,6,7,8})p.paint_filament(id,41);
    p.paint_filament(4,91);p.paint_filament(5,103);
    for(const auto& entry:p.colors)p.target_colors[entry.first]=entry.second;
    REQUIRE_NOTHROW(p.validate(*surface));
    const auto initial=p;std::vector<RGBA> original_faces;
    for(uint32_t id:p.face_piece)original_faces.push_back(p.colors.at(id));
    const auto protection=p.protect_color_intent(*surface,original_faces,
        BeautyPuzzle::refinement_protection(*surface,all,labels,{"face","iris","ulip","imouth"},false));
    const size_t cleaned=p.clean_color_islands(*surface,all,protection);CHECK(cleaned==3);
    const auto cleaned_puzzle=p;
    const auto rim_protection=p.protect_color_intent(*surface,original_faces,
        BeautyPuzzle::refinement_protection(*surface,all,labels,{"face","iris","ulip","imouth"},true));
    const size_t repaired=p.smooth_selected_boundaries(*surface,all,rim_protection);CHECK(repaired>0);
    size_t protected_changes=0;
    for(size_t f=0;f<labels.size();++f)if(protection[f])protected_changes+=initial.filament_slots.at(initial.face_piece[f])!=cleaned_puzzle.filament_slots.at(cleaned_puzzle.face_piece[f]);
    CHECK(protected_changes==0);
    const auto write=[&](const std::string& name,const auto& values) {
        boost::filesystem::ofstream file(output/name,std::ios::binary);
        file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(values[0])));file.close();REQUIRE(file.good());
    };
    std::vector<float> vertices;std::vector<int32_t> triangles;
    for(const auto& v:mesh.vertices)for(size_t c=0;c<3;++c)vertices.push_back(v[c]);
    for(const auto& f:mesh.indices)for(size_t c=0;c<3;++c)triangles.push_back(f[c]);
    write("vertices.f32",vertices);write("triangles.i32",triangles);write("labels.i32",labels);
    nlohmann::json stages=nlohmann::json::object();
    for(const auto& entry:std::vector<std::pair<std::string,BeautyPuzzle>>{{"initial",initial},{"cleaned",cleaned_puzzle},{"repaired",p}}) {
        const auto& puzzle=entry.second;std::vector<float> rgb;std::vector<size_t> slots;
        for(uint32_t id:puzzle.face_piece){for(size_t c=0;c<3;++c)rgb.push_back(puzzle.colors.at(id)[c]);slots.push_back(puzzle.filament_slots.at(id));}
        write(entry.first+".f32",rgb);
        const auto restored=BeautyPuzzle::decode(puzzle.encode(),puzzle.geometry_id,puzzle.face_piece.size());CHECK(restored.same_edit(puzzle));
        boost::filesystem::ofstream record(output/(entry.first+".json"));record<<puzzle.encode().dump();record.close();REQUIRE(record.good());
        stages[entry.first]={{"color_edges",color_edges(puzzle,*surface)},{"pieces",puzzle.piece_count()},{"color_sha256",model_artifact_sha256(output/(entry.first+".f32"))}};
    }
    boost::filesystem::ofstream report(output/"report.json");
    report<<nlohmann::json({{"fixture","synthetic facial mask on fixed planar mesh; not a real portrait"},{"faces",mesh.indices.size()},
        {"cleaned_faces",cleaned},{"repaired_faces",repaired},{"protected_cleanup_changes",protected_changes},{"stages",stages}}).dump(2);
    report.close();REQUIRE(report.good());
}
