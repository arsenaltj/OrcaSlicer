#include "slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.hpp"
#include "legacy/Application/SmartSlicingEngine.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
namespace N = Slic3r::AI::SmartSlicing;
namespace O = Slic3r::AI::LegacySmartSlicing;
void check(bool ok) { if(!ok) throw std::runtime_error("old/new behavioral oracle failed"); }
void equal(double a,double b) {check(std::abs(a-b)<1e-9);}
template<class Mesh> Mesh cube(double z) {
    Mesh m; m.vertices={{0,0,z},{10,0,z},{10,10,z},{0,10,z},{0,0,z+10},{10,0,z+10},{10,10,z+10},{0,10,z+10}};
    m.triangles={{{0,2,1}},{{0,3,2}},{{4,5,6}},{{4,6,7}},{{0,1,5}},{{0,5,4}},{{1,2,6}},{{1,6,5}},{{2,3,7}},{{2,7,6}},{{3,0,4}},{{3,4,7}}};return m;
}
template<class A,class B> void emit(const char* name,const A& old,const B& now) {
    check(old.completed && now.completed);
    for(size_t i=0;i<3;++i) {
        const auto& a=old.recommendations[i];const auto& b=now.recommendations[i];
        std::cout<<name<<" mode="<<i<<" old(layer,support,type,angle,adaptive)="<<a.layer_height_mm<<","<<a.enable_support<<","<<a.support_type<<","<<a.support_threshold_angle_deg<<","<<a.adaptive_layer_height
                 <<" new="<<b.layer_height_mm<<","<<b.enable_support<<","<<b.support_type<<","<<b.support_threshold_angle_deg<<","<<b.adaptive_layer_height<<"\n";
    }
}
int main() {try {
    O::MachinePreset om; N::MachinePreset nm; om.nozzle_diameter_mm=nm.nozzle_diameter_mm=.4;
    om.minimum_layer_height_mm=nm.minimum_layer_height_mm=.04;om.maximum_layer_height_mm=nm.maximum_layer_height_mm=.28;
    O::FilamentPreset of; N::FilamentPreset nf;
    for(double z:{0.,4.}) {
        auto old=O::SmartSlicingEngine().AnalyzeAndPrecompute(cube<O::TriangleMeshSnapshot>(z),om,of);
        auto now=N::SmartSlicingEngine().AnalyzeAndPrecompute(cube<N::TriangleMeshSnapshot>(z),nm,nf);
        emit(z==0?"bed-cube":"floating-cube",old,now);
        equal(old.recommendations[2].layer_height_mm,.12);equal(now.recommendations[2].layer_height_mm,.16);
        check(old.recommendations[2].adaptive_layer_height);check(!now.recommendations[2].adaptive_layer_height);
        equal(now.recommendations[0].support_threshold_angle_deg,38);equal(now.recommendations[1].support_threshold_angle_deg,35);
        for(const auto& plan:now.recommendations) {check(plan.enable_support==(z>0));if(z>0)check(plan.support_type=="normal(auto)");}
        equal(now.metrics.slope_detail_area_ratio,0.);
    }
    // Keep both roofs: a 10mm square slope is valid but falls below the
    // adaptive threshold, while a 20mm square has enough sloped surface.
    // Analytic roof area = depth*hypot(width,rise); total includes the flat
    // bottom and four vertical walls. This oracle is independent of triangulation.
    for(double width:{10.,20.}) {
        auto slopeOld=cube<O::TriangleMeshSnapshot>(0.);auto slopeNew=cube<N::TriangleMeshSnapshot>(0.);
        for(auto& v:slopeOld.vertices){v.x*=width/10.;v.y*=width/10.;}
        for(auto& v:slopeNew.vertices){v.x*=width/10.;v.y*=width/10.;}
        for(size_t i:{size_t(5),size_t(6)}) {slopeOld.vertices[i].z+=5.;slopeNew.vertices[i].z+=5.;}
        const double roof=width*std::hypot(width,5.);
        const double fraction=roof/(roof+width*width+2.*width*25.);
        auto measuredOld=O::SmartSlicingEngine().AnalyzeAndPrecompute(slopeOld,om,of);
        auto measuredNew=N::SmartSlicingEngine().AnalyzeAndPrecompute(slopeNew,nm,nf);
        emit(width==10.?"actual-roof-10mm":"actual-roof-20mm",measuredOld,measuredNew);
        std::cout<<"roof width="<<width<<" analytic slope fraction="<<fraction<<" measured="<<measuredNew.metrics.slope_detail_area_ratio<<"\n";
        equal(measuredNew.metrics.slope_detail_area_ratio,fraction);
        check(measuredNew.metrics.closed_mesh);
        equal(measuredNew.recommendations[0].layer_height_mm,.16);equal(measuredNew.recommendations[1].layer_height_mm,.22);
        check(measuredNew.recommendations[2].adaptive_layer_height==(fraction>.16));
        check((width==10. && fraction>.12 && fraction<.16) || (width==20. && fraction>.16));
    }
    O::ModelMetrics a;N::ModelMetrics b;
    a.bounding_box={0,0,0,20,20,10};b.bounding_box={0,0,0,20,20,10};
    a.volume_mm3=b.volume_mm3=1000;a.surface_area_mm2=b.surface_area_mm2=1000;
    a.top_micro_slope_surface_ratio=b.top_micro_slope_surface_ratio=.42;b.slope_detail_area_ratio=.42;
    a.closed_mesh=b.closed_mesh=true;
    auto old=O::SmartSlicingEngine().AnalyzeAndPrecompute(a,om,of);auto now=N::SmartSlicingEngine().AnalyzeAndPrecompute(b,nm,nf);
    emit("slope-metrics-0.42",old,now);
    equal(old.recommendations[0].layer_height_mm,.20);equal(old.recommendations[1].layer_height_mm,.26);
    equal(now.recommendations[0].layer_height_mm,.16);equal(now.recommendations[1].layer_height_mm,.22);equal(now.recommendations[2].layer_height_mm,.12);
    check(now.recommendations[2].adaptive_layer_height);
    nm.minimum_layer_height_mm=.24;nm.maximum_layer_height_mm=.28;
    now=N::SmartSlicingEngine().AnalyzeAndPrecompute(b,nm,nf);check(now.completed);check(!now.recommendations[2].adaptive_layer_height);
    b.closed_mesh=false;b.top_micro_slope_surface_ratio=b.slope_detail_area_ratio=0;nm.baseline_support_enabled=true;
    now=N::SmartSlicingEngine().AnalyzeAndPrecompute(b,nm,nf);for(const auto& p:now.recommendations)check(p.enable_support);
    nm.minimum_layer_height_mm=.29;nm.maximum_layer_height_mm=.28;check(!N::SmartSlicingEngine().AnalyzeAndPrecompute(b,nm,nf).completed);
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
