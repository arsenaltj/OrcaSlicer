#include <catch2/catch_all.hpp>

// MultiMaterialSegmentation.hpp declares boost::polygon traits for ColoredLine, so its
// geometry/boost dependencies must be included first.
#include <boost/polygon/polygon.hpp>
#include "libslic3r/Line.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PaintedLineProcessing.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/MutablePolygon.hpp"
#include <algorithm>
#include <array>
#include <mutex>
#include <set>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>

using namespace Slic3r;

TEST_CASE("Contours without projected paint retain complete default boundaries", "[MultiMaterialSegmentation][MissingContourColor][Regression]")
{
    const Points outer{Point::new_scale(0,0),Point::new_scale(10,0),Point::new_scale(10,10),Point::new_scale(0,10)};
    const Points hole{Point::new_scale(4,4),Point::new_scale(4,6),Point::new_scale(6,6),Point::new_scale(6,4)};
    const std::vector<EdgeGrid::Contour> contours{EdgeGrid::Contour(outer,false),EdgeGrid::Contour(hole,false)};
    const auto empty_projection=SegmentationDetail::post_process_painted_lines(contours,{});
    REQUIRE(empty_projection.empty());
    const auto colored=SegmentationDetail::colorize_contours(contours,empty_projection);
    REQUIRE(colored.size()==contours.size());
    for(size_t c=0;c<contours.size();++c){
        REQUIRE(colored[c].size()==contours[c].num_segments());
        for(size_t i=0;i<colored[c].size();++i){
            CHECK(colored[c][i].color==0);
            CHECK(colored[c][i].line.a==contours[c].segment_start(i));
            CHECK(colored[c][i].line.b==contours[c].segment_end(i));
            CHECK(colored[c][i].poly_idx==int(c));
            CHECK(colored[c][i].local_line_idx==int(i));
        }
    }
    CHECK(SegmentationDetail::colorize_contours({},{}).empty());
}

TEST_CASE("Missing contour colors require nearby exclusive source evidence", "[MultiMaterialSegmentation][MissingContourColor][Regression]")
{
    const double angle=GENERATE(0.,.23,1.11);
    const bool reverse=GENERATE(false,true);
    const int scenario=GENERATE(0,1,2,3,4,5);
    const auto point=[&](double x,double y){return Point::new_scale(x*std::cos(angle)-y*std::sin(angle),x*std::sin(angle)+y*std::cos(angle));};
    const auto line=[&](double ax,double ay,double bx,double by,int color){return ColoredLine{Line(point(ax,ay),point(bx,by)),color,0,0};};
    std::vector<ColoredLines> contours{{line(-.05,0,.05,0,0),line(.05,0,.06,0,3)}};
    const auto explicit_line=contours[0].back();
    ColoredLines source{line(-.1,.001,.1,.001,2)};
    if(scenario==1)source.push_back(line(-.1,.0005,.1,.0005,0));
    if(scenario==2)source.push_back(line(-.1,-.001,.1,-.001,1));
    if(scenario==3)source={line(-.1,.006,.1,.006,2)};
    if(scenario==4)source={line(0,-.1,0,.1,2)};
    if(scenario==5)source.clear();
    if(reverse){std::reverse(source.begin(),source.end());for(auto &s:source)std::swap(s.line.a,s.line.b);}
    const bool changed=SegmentationDetail::restore_missing_contour_colors(contours,source,[]{});
    CHECK(changed==(scenario==0));
    REQUIRE(contours[0].size()==2);
    CHECK(contours[0][0].color==(scenario==0?2:0));
    CHECK(contours[0].back().line.a==explicit_line.line.a);
    CHECK(contours[0].back().line.b==explicit_line.line.b);
    CHECK(contours[0].back().color==explicit_line.color);
}

TEST_CASE("Unpainted source intervals survive contour recovery and cancellation", "[MultiMaterialSegmentation][MissingContourColor][Regression]")
{
    std::vector<ColoredLines> contour{{{Line(Point::new_scale(0,0),Point::new_scale(.1,0)),0,0,0}}};
    const ColoredLines source{{Line(Point::new_scale(-.1,.001),Point::new_scale(.2,.001)),2},
                             {Line(Point::new_scale(.04,.0001),Point::new_scale(.06,.0001)),0}};
    auto interrupted=contour;
    CHECK_THROWS_AS(SegmentationDetail::restore_missing_contour_colors(interrupted,source,[]{throw std::runtime_error("cancel");}),std::runtime_error);
    REQUIRE(interrupted[0].size()==1);
    CHECK(interrupted[0][0].color==0);
    REQUIRE(SegmentationDetail::restore_missing_contour_colors(contour,source,[]{}));
    bool preserved=false,recovered=false;
    for(const auto &line:contour[0]){
        if(line.line.a.x()<=scale_(.05) && line.line.b.x()>=scale_(.05)){CHECK(line.color==0);preserved=true;}
        recovered|=line.color==2;
    }
    CHECK(preserved);CHECK(recovered);
}

TEST_CASE("Default region recovery preserves paint and rejects competing coverage", "[MultiMaterialSegmentation][DefaultRegionRecovery][Regression]")
{
    const double angle=GENERATE(0.,.19,.57);
    const size_t shift=GENERATE(size_t(0),size_t(1),size_t(2),size_t(3));
    const auto box=[&](double x0,double y0,double x1,double y1){
        ExPolygon p(Polygon({Point::new_scale(x0,y0),Point::new_scale(x1,y0),Point::new_scale(x1,y1),Point::new_scale(x0,y1)}));
        p.rotate(angle);p.translate(Point::new_scale(13,-17));return ExPolygons{p};
    };
    const auto area=[](const ExPolygons &p){double a=0;for(const auto &v:p)a+=v.area()*SCALING_FACTOR*SCALING_FACTOR;return a;};
    const auto join=[](const std::vector<ExPolygons> &g){ExPolygons all;for(const auto &p:g)append(all,p);return union_ex(all);};
    const auto slot=[&](size_t c){return 1+(c-1+shift)%4;};
    std::vector<ExPolygons> old(5),candidate(5);
    old[slot(1)]=box(4,4,6,6);old[slot(2)]=box(1,7,3,9);old[0]=diff_ex(box(0,0,10,10),join(old));
    candidate[slot(3)]=box(-1,-1,5.5,11);candidate[slot(4)]=box(4.5,-1,11,11);candidate[0]=box(1,1,3,3);
    auto result=old;
    REQUIRE(SegmentationDetail::transfer_default_color_regions(result,candidate));
    CHECK_THAT(area(diff_ex(join(old),join(result))),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area(diff_ex(join(result),join(old))),Catch::Matchers::WithinAbs(0.,1e-6));
    ExPolygons added;
    for(size_t c=1;c<old.size();++c){
        CHECK_THAT(area(diff_ex(old[c],result[c])),Catch::Matchers::WithinAbs(0.,1e-6));
        append(added,diff_ex(result[c],old[c]));
    }
    CHECK(area(added)>50.);
    CHECK_THAT(area(intersection_ex(added,candidate[0])),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area(intersection_ex(added,intersection_ex(candidate[slot(3)],candidate[slot(4)]))),Catch::Matchers::WithinAbs(0.,1e-6));
    for(size_t a=0;a<result.size();++a)for(size_t b=a+1;b<result.size();++b)
        CHECK_THAT(area(intersection_ex(result[a],result[b])),Catch::Matchers::WithinAbs(0.,1e-6));
    auto empty_control=old;
    CHECK_FALSE(SegmentationDetail::transfer_default_color_regions(empty_control,std::vector<ExPolygons>(5)));
    for(size_t c=0;c<old.size();++c)CHECK_THAT(area(diff_ex(old[c],empty_control[c])),Catch::Matchers::WithinAbs(0.,1e-6));
}

// Retained local geometry can identify which preprocessing step removes a
// painted boundary. This diagnostic does not change the production pipeline.
TEST_CASE("Saved segmentation input reproduces each preprocessing stage", "[MultiMaterialSegmentation][ColorPreprocessingEvidence][.]")
{
    const char *trace = std::getenv("ORCA_COLOR_PREPROCESS_TRACE");
    const char *layers_file = std::getenv("ORCA_COLOR_PREPROCESS_LAYERS");
    const char *destination = std::getenv("ORCA_COLOR_PREPROCESS_OUTPUT");
    if (!trace || !layers_file || !destination)
        SKIP("Explicit retained traces, layer list and empty output directory required.");
    const std::filesystem::path output(destination);
    REQUIRE(std::filesystem::is_directory(output));
    REQUIRE(std::filesystem::is_empty(output));
    const auto read = [](const std::filesystem::path &path) {
        std::ifstream file(path);
        if (!file) throw std::runtime_error("Missing preprocessing trace");
        ExPolygons polygons; std::string row;
        while (std::getline(file, row)) {
            std::istringstream line(row); char kind; line >> kind;
            if (kind == 'C' || kind == 'H') {
                Points points; coord_t x, y;
                while (line >> x >> y) points.emplace_back(x, y);
                if (kind == 'C') polygons.emplace_back(Polygon(std::move(points)));
                else polygons.back().holes.emplace_back(std::move(points));
            }
        }
        return polygons;
    };
    const auto write = [&](const std::string &stage, size_t layer, const ExPolygons &polygons) {
        std::ofstream file(output / (stage + "-" + std::to_string(layer) + ".txt"));
        file << "G 0\n";
        const auto ring = [&](char kind, const Polygon &polygon) {
            file << kind;
            for (const Point &point : polygon.points) file << ' ' << point.x() << ' ' << point.y();
            file << '\n';
        };
        for (const auto &polygon : polygons) {
            ring('C', polygon.contour);
            for (const auto &hole : polygon.holes) ring('H', hole);
        }
        file.flush(); REQUIRE(bool(file));
    };
    const auto area_mm2 = [](const ExPolygons &polygons) {
        double area = 0.; for (const auto &polygon : polygons) area += polygon.area();
        return area * SCALING_FACTOR * SCALING_FACTOR;
    };
    std::ifstream layers(layers_file); REQUIRE(bool(layers));
    size_t layer, count = 0;
    while (layers >> layer) {
        {
            CAPTURE(layer);
            const auto raw = read(std::filesystem::path(trace) / ("mm-exact-0-raw-" + std::to_string(layer) + ".txt"));
            ExPolygons polygons;
            for (const auto &polygon : raw) append(polygons, offset_ex(polygon, float(10 * SCALED_EPSILON)));
            write("expanded", layer, polygons);
            polygons = union_ex(polygons); write("united", layer, polygons);
            remove_small_and_small_holes(polygons, Slic3r::sqr(scale_(0.1f))); write("small_removed", layer, polygons);
            polygons = offset_ex(polygons, -10.f * float(SCALED_EPSILON)); write("contracted", layer, polygons);
            polygons = expolygons_simplify(polygons, 5 * SCALED_EPSILON); write("simplified", layer, polygons);
            polygons = remove_duplicates(std::move(polygons), scaled<coord_t>(0.01), PI / 6); write("deduplicated", layer, polygons);
            const auto actual = read(std::filesystem::path(trace) / ("mm-exact-0-processed-" + std::to_string(layer) + ".txt"));
            CHECK_THAT(area_mm2(diff_ex(polygons, actual)), Catch::Matchers::WithinAbs(0., 1e-6));
            CHECK_THAT(area_mm2(diff_ex(actual, polygons)), Catch::Matchers::WithinAbs(0., 1e-6));
            ++count;
        }
    }
    REQUIRE(count > 0);
}

// Opt-in replay consumes retained local contour evidence; ordinary tests do not
// depend on a private model or environment-specific validation directory.
// The four retained contours currently expose known overlap/coverage defects.
// Keep those acceptance failures visible when explicitly replaying the evidence.
TEST_CASE("Saved colored contours retain exclusive ownership near their boundary", "[MultiMaterialSegmentation][ColoredContourReplay][.]")
{
    const char *root = std::getenv("ORCA_CONTOUR_REPLAY_TRACES");
    if (!root) SKIP("Set ORCA_CONTOUR_REPLAY_TRACES to retained native traces.");
    std::vector<int> replay_layers{215, 283, 284, 358};
    if (const char *list = std::getenv("ORCA_CONTOUR_REPLAY_LAYERS")) {
        std::ifstream layers(list); REQUIRE(bool(layers));
        replay_layers.clear(); int layer;
        while (layers >> layer) {
            REQUIRE(layer >= 0);
            replay_layers.push_back(layer);
        }
        REQUIRE_FALSE(replay_layers.empty());
    }
    for (int layer : replay_layers) {
        DYNAMIC_SECTION("Layer " << layer) {
            std::ifstream file(std::string(root) + "/mm-exact-0-colors-" + std::to_string(layer) + ".txt");
            REQUIRE(file.good());
            std::vector<ColoredLines> contours;
            std::string row;
            while (std::getline(file, row)) {
                if (row == "C") { contours.emplace_back(); continue; }
                std::istringstream stream(row);
                int color; coord_t ax, ay, bx, by;
                if (stream >> color >> ax >> ay >> bx >> by) {
                    REQUIRE_FALSE(contours.empty());
                    contours.back().push_back({Line(Point(ax, ay), Point(bx, by)), color, int(contours.size()-1), int(contours.back().size())});
                }
            }
            const auto regions = SegmentationDetail::segment_colored_contours(contours, 5, layer);
            Polygons input;
            for (const auto &contour : contours) {
                Points points;
                for (const auto &line : contour) points.push_back(line.line.a);
                input.emplace_back(std::move(points));
            }
            const auto area_mm2 = [](const ExPolygons &polys) { double sum = 0.; for (const auto &p : polys) sum += p.area() * SCALING_FACTOR * SCALING_FACTOR; return sum; };
            const ExPolygons shape = union_ex(input);
            std::vector<ExPolygons> baseline(5);
            std::ifstream old_file(std::string(root) + "/mm-exact-0-sides-" + std::to_string(layer) + ".txt");
            REQUIRE(old_file.good());
            size_t group = 0;
            while (std::getline(old_file, row)) {
                std::istringstream stream(row); char kind; stream >> kind;
                if (kind == 'G') stream >> group;
                else if (kind == 'C' || kind == 'H') {
                    Points points; coord_t x, y; while (stream >> x >> y) points.emplace_back(x, y);
                    if (kind == 'C') baseline.at(group).emplace_back(Polygon(std::move(points)));
                    else baseline.at(group).back().holes.emplace_back(std::move(points));
                }
            }
            ExPolygons baseline_joined;
            for (const auto &region : baseline) append(baseline_joined, region);
            const double baseline_missing = area_mm2(diff_ex(shape, baseline_joined));
            const double baseline_outside = area_mm2(diff_ex(baseline_joined, shape));
            if (std::getenv("ORCA_CONTOUR_REPLAY_EXPECT_SUBSET"))
                for(size_t color=0;color<regions.size();++color) {
                    CAPTURE(layer,color);
                    // Keep the strict area diagnostic visible. Tiny offsets of
                    // weakly simple polygons are not a monotone reference.
                    CHECK_THAT(area_mm2(diff_ex(regions[color],baseline[color])),Catch::Matchers::WithinAbs(0.,1e-6));
                }
            if (std::getenv("ORCA_CONTOUR_REPLAY_EXPECT_BASELINE")) {
                for (size_t color = 0; color < regions.size(); ++color) {
                    REQUIRE_THAT(area_mm2(diff_ex(regions[color], baseline[color])), Catch::Matchers::WithinAbs(0., 1e-6));
                    REQUIRE_THAT(area_mm2(diff_ex(baseline[color], regions[color])), Catch::Matchers::WithinAbs(0., 1e-6));
                }
            }
            ExPolygons joined;
            for (const auto &region : regions) append(joined, region);
            const double missing = area_mm2(diff_ex(shape, joined));
            const double outside = area_mm2(diff_ex(joined, shape));
            double overlap = 0.;
            for (size_t a = 0; a < regions.size(); ++a)
                for (size_t b = a + 1; b < regions.size(); ++b)
                    for (const auto &p : intersection_ex(regions[a], regions[b])) overlap += p.area() * SCALING_FACTOR * SCALING_FACTOR;
            if (const char *output = std::getenv("ORCA_CONTOUR_REPLAY_OUTPUT")) {
                std::ofstream pairs(std::string(output) + "/component-overlaps-" + std::to_string(layer) + ".txt");
                REQUIRE(pairs.good());
                pairs.precision(17);
                for (size_t a=0; a<regions.size(); ++a) for(size_t b=a+1; b<regions.size(); ++b)
                    for(size_t i=0; i<regions[a].size(); ++i) for(size_t j=0; j<regions[b].size(); ++j) {
                        const double shared=area_mm2(intersection_ex(ExPolygons{regions[a][i]},ExPolygons{regions[b][j]}));
                        if(shared>1e-9) pairs << a << ' ' << i << ' ' << b << ' ' << j << ' ' << shared << ' ' << area_mm2(ExPolygons{regions[a][i]}) << ' ' << area_mm2(ExPolygons{regions[b][j]}) << '\n';
                    }
                std::ofstream out(std::string(output) + "/regions-" + std::to_string(layer) + ".txt");
                REQUIRE(out.good());
                const auto write_groups = [](std::ostream &stream, const std::vector<ExPolygons> &groups) {
                    for (size_t color = 0; color < groups.size(); ++color) {
                        stream << "G " << color << '\n';
                        for (const auto &poly : union_ex(groups[color])) {
                            stream << "C"; for (const auto &p : poly.contour.points) stream << ' ' << p.x() << ' ' << p.y(); stream << '\n';
                            for (const auto &hole : poly.holes) { stream << "H"; for (const auto &p : hole.points) stream << ' ' << p.x() << ' ' << p.y(); stream << '\n'; }
                        }
                    }
                };
                write_groups(out,regions);
                std::ofstream old_out(std::string(output) + "/baseline-regions-" + std::to_string(layer) + ".txt");
                REQUIRE(old_out.good()); write_groups(old_out,baseline);
            }
            CAPTURE(layer, overlap, missing, outside, baseline_missing, baseline_outside);
            CHECK_THAT(overlap, Catch::Matchers::WithinAbs(0., 1e-6));
            CHECK_THAT(missing, Catch::Matchers::WithinAbs(0., 1e-6));
            CHECK_THAT(outside, Catch::Matchers::WithinAbs(0., 1e-6));
        }
    }
}

TEST_CASE("Colored holes retain their own side wall without overlapping the outer color", "[MultiMaterialSegmentation][ColoredHoles][Regression]")
{
    const double angle = GENERATE(0., 0.17, 0.53);
    const int hole_count = GENERATE(1, 2);
    Polygon outer({Point::new_scale(-20, -20), Point::new_scale(20, -20), Point::new_scale(20, 20), Point::new_scale(-20, 20)});
    ExPolygon shape(outer);
    for (int h = 0; h < hole_count; ++h) {
        const double x = h == 0 ? -7. : 7.;
        shape.holes.emplace_back(Points{Point::new_scale(x - 2, -2), Point::new_scale(x - 2, 2), Point::new_scale(x + 2, 2), Point::new_scale(x + 2, -2)});
    }
    shape.rotate(angle);
    std::vector<ColoredLines> contours;
    const auto add = [&](const Polygon &poly, int color) {
        ColoredLines lines;
        for (const Line &line : poly.lines())
            lines.push_back({line, color, int(contours.size()), int(lines.size())});
        contours.push_back(std::move(lines));
    };
    add(shape.contour, 1);
    for (const Polygon &hole : shape.holes) add(hole, 2);
    const auto regions = SegmentationDetail::segment_colored_contours(contours, 3);
    const auto area_mm2 = [](const ExPolygons &polys) { double value = 0.; for (const auto &poly : polys) value += poly.area(); return value * SCALING_FACTOR * SCALING_FACTOR; };
    REQUIRE_THAT(area_mm2(intersection_ex(regions[1], regions[2])), Catch::Matchers::WithinAbs(0., 1e-6));
    ExPolygons joined = regions[1];
    append(joined, regions[2]);
    REQUIRE_THAT(area_mm2(diff_ex(ExPolygons{shape}, joined)), Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE_THAT(area_mm2(diff_ex(joined, ExPolygons{shape})), Catch::Matchers::WithinAbs(0., 1e-6));
    for (const Polygon &hole : shape.holes) {
        Polygon solid = hole;
        solid.reverse();
        const ExPolygons ring = diff_ex(offset_ex(Polygons{solid}, float(scale_(0.2))), ExPolygons{ExPolygon(solid)});
        REQUIRE_THAT(area_mm2(diff_ex(ring, regions[2])), Catch::Matchers::WithinAbs(0., 1e-6));
    }
}

TEST_CASE("A touching slit does not duplicate the colored side partition", "[MultiMaterialSegmentation][TouchingContours][Regression]")
{
    const coord_t tip_offset = GENERATE(coord_t(2), coord_t(6), coord_t(24));
    const double angle = GENERATE(0., 0.17);
    const coord_t span = scale_(20.);
    Polygon polygon(Points{Point(-span,-span), Point(span,-span), Point(span,span),
        Point(0,span), Point(tip_offset,0), Point(tip_offset/2,-coord_t(scale_(1.))),
        Point(0,0), Point(0,span), Point(-span,span)});
    polygon.rotate(angle);
    ColoredLines lines;
    for (size_t i=0; i<polygon.points.size(); ++i)
        lines.push_back({Line(polygon.points[i],polygon.points[(i+1)%polygon.points.size()]), i==8 ? 2 : 1, 0, int(i)});
    const auto result = SegmentationDetail::segment_colored_contours({lines},3);
    const auto area_mm2 = [](const ExPolygons &polys) { double sum=0.; for(const auto &p:polys)sum+=p.area()*SCALING_FACTOR*SCALING_FACTOR; return sum; };
    ExPolygons joined=result[1]; append(joined,result[2]);
    const ExPolygons shape=union_ex(Polygons{polygon});
    CAPTURE(tip_offset,angle);
    CHECK_THAT(area_mm2(intersection_ex(result[1],result[2])),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(shape,joined)),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(joined,shape)),Catch::Matchers::WithinAbs(0.,1e-6));
}

TEST_CASE("An invalid partition restores interior boundary ownership and retains unprojected paint", "[MultiMaterialSegmentation][InvalidColoredPartition][Regression]")
{
    const double angle = GENERATE(0., 0.17);
    const bool default_bottom = GENERATE(false, true);
    const int left_color = GENERATE(1, 2);
    const int right_color = 3-left_color;
    Polygon outline(Points{Point::new_scale(-2,-2),Point::new_scale(0,-2),Point::new_scale(2,-2),
                           Point::new_scale(2,2),Point::new_scale(0,2),Point::new_scale(-2,2)});
    outline.rotate(angle);
    std::vector<ColoredLines> contours(1);
    const auto lines=outline.lines();
    for(size_t i=0;i<lines.size();++i) {
        int color=(i==0||i==4||i==5)?left_color:right_color;
        if(default_bottom&&i<2)color=0;
        contours[0].push_back({lines[i],color,0,int(i)});
    }
    ExPolygon bad(Polygon(Points{Point::new_scale(1,-2),Point::new_scale(4,-2),Point::new_scale(4,2),Point::new_scale(1,2)}));
    ExPolygon unprojected(Polygon(Points{Point::new_scale(-.5,-1.8),Point::new_scale(-.1,-1.8),Point::new_scale(-.1,-1.2),Point::new_scale(-.5,-1.2)}));
    bad.rotate(angle);unprojected.rotate(angle);
    std::vector<ExPolygons> regions(4);regions[right_color].push_back(bad);
    if(default_bottom)regions[3].push_back(unprojected);
    SegmentationDetail::repair_invalid_colored_partition(regions,contours);
    const auto area_mm2=[](const ExPolygons &polys){double a=0.;for(const auto&p:polys)a+=p.area();return a*SCALING_FACTOR*SCALING_FACTOR;};
    ExPolygons joined;for(const auto&g:regions)append(joined,g);
    CHECK_THAT(area_mm2(diff_ex(ExPolygons{ExPolygon(outline)},joined)),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(joined,ExPolygons{ExPolygon(outline)})),Catch::Matchers::WithinAbs(0.,1e-6));
    for(size_t a=0;a<regions.size();++a)for(size_t b=a+1;b<regions.size();++b)
        CHECK_THAT(area_mm2(intersection_ex(regions[a],regions[b])),Catch::Matchers::WithinAbs(0.,1e-6));
    for(const auto&[x,color]:std::vector<std::pair<double,int>>{{-1.95,left_color},{1.95,right_color}}){
        Point probe=Point::new_scale(x,0.);probe.rotate(angle);
        CHECK(std::any_of(regions[color].begin(),regions[color].end(),[&](const ExPolygon&p){return p.contains(probe);}));
    }
    if(default_bottom)CHECK_THAT(area_mm2(diff_ex(ExPolygons{unprojected},regions[3])),Catch::Matchers::WithinAbs(0.,1e-6));
}

TEST_CASE("A nested painted hole keeps its source color and the original coverage", "[MultiMaterialSegmentation][NestedPaintedRegions][Regression]")
{
    const size_t inner_color = GENERATE(size_t(0),size_t(1),size_t(2));
    const size_t outer_color = (inner_color+1)%3;
    const bool conflicting_boundary = GENERATE(false,true);
    const bool cancelling_contour = GENERATE(false,true);
    const auto square = [](double radius) { return Polygon(Points{Point::new_scale(-radius,-radius),Point::new_scale(radius,-radius),Point::new_scale(radius,radius),Point::new_scale(-radius,radius)}); };
    ExPolygon shape(square(20.));
    Polygon hole=square(2.); hole.reverse(); shape.holes.push_back(hole);
    ExPolygon inner(square(6.)); inner.holes.push_back(hole);
    std::vector<ColoredLines> contours;
    const auto add = [&](const Polygon &polygon, int color) {
        ColoredLines lines; for(const auto &line:polygon.lines()) lines.push_back({line,color,int(contours.size()),int(lines.size())});
        contours.push_back(std::move(lines));
    };
    add(shape.contour,int(outer_color)); add(hole,int(inner_color));
    if(conflicting_boundary) {
        Polygon other=square(0.5); other.translate(scale_(4.),0); other.reverse();
        shape.holes.push_back(other); inner.holes.push_back(other); add(other,int(outer_color));
    }
    std::vector<ExPolygons> regions(3);
    regions[outer_color]={shape}; regions[inner_color]={inner};
    if(cancelling_contour) {
        // A weakly simple contour can carry a clockwise lobe cancelled by a
        // second contour of the same color. Clipping only the first contour
        // would normalize that lobe and incorrectly create new coverage.
        Polygon weak(Points{Point::new_scale(-20,-20),Point::new_scale(20,-20),Point::new_scale(20,20),Point::new_scale(-20,20),Point::new_scale(-20,-20),
            Point::new_scale(30,-20),Point::new_scale(30,-10),Point::new_scale(40,-10),Point::new_scale(40,-20),Point::new_scale(30,-20),Point::new_scale(-20,-20)});
        regions[outer_color][0].contour=std::move(weak);
        regions[outer_color].emplace_back(Polygon(Points{Point::new_scale(30,-20),Point::new_scale(40,-20),Point::new_scale(40,-10),Point::new_scale(30,-10)}));
    }
    SegmentationDetail::repair_nested_colored_regions(regions,contours);
    const auto area_mm2=[](const ExPolygons &polys) {double sum=0.;for(const auto &p:polys)sum+=p.area()*SCALING_FACTOR*SCALING_FACTOR;return sum;};
    const ExPolygons expected_outer=conflicting_boundary ? ExPolygons{shape} : diff_ex(ExPolygons{shape},ExPolygons{inner});
    CHECK_THAT(area_mm2(diff_ex(regions[outer_color],expected_outer)),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(expected_outer,regions[outer_color])),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(ExPolygons{inner},regions[inner_color])),Catch::Matchers::WithinAbs(0.,1e-6));
    ExPolygons joined=regions[inner_color];append(joined,regions[outer_color]);
    CHECK_THAT(area_mm2(diff_ex(ExPolygons{shape},joined)),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(joined,ExPolygons{shape})),Catch::Matchers::WithinAbs(0.,1e-6));
}

// Diagnostic for the integer-grid error in the unmodified Clipper reference,
// independent of either colored-region repair implementation.
TEST_CASE("Rotated reference subtraction retains exact union area", "[BoundaryOverlapQuantization][.]")
{
    const auto rectangle=[](double x1,double y1,double x2,double y2) {
        Polygon p(Points{Point::new_scale(x1,y1),Point::new_scale(x2,y1),Point::new_scale(x2,y2),Point::new_scale(x1,y2)});
        p.rotate(0.17);return p;
    };
    ExPolygon left(rectangle(-20.,-20.,3.,20.)),right(rectangle(-3.,-19.,20.,19.));
    Polygon hole=rectangle(-1.,-1.,1.,1.);hole.reverse();
    left.holes.push_back(hole);right.holes.push_back(hole);
    ExPolygons reference={left};
    append(reference,diff_ex(ExPolygons{right},intersection_ex(ExPolygons{left},ExPolygons{right})));
    double missing_mm2=0.;
    for(const auto &p:diff_ex(ExPolygons{left,right},reference)) missing_mm2+=p.area()*SCALING_FACTOR*SCALING_FACTOR;
    CAPTURE(missing_mm2);
    CHECK_THAT(missing_mm2,Catch::Matchers::WithinAbs(0.,1e-6));
}

TEST_CASE("A partial color overlap keeps the unique boundary owner without losing coverage", "[MultiMaterialSegmentation][BoundaryOwnedOverlap][Regression]")
{
    const size_t owner = GENERATE(size_t(0), size_t(1), size_t(2), size_t(3));
    const size_t other = (owner + 1) % 4, third = (owner + 2) % 4;
    const bool conflicting_boundary = GENERATE(false, true);
    const bool third_region = GENERATE(false, true);
    const double angle = GENERATE(0., 0.17);
    const auto rectangle = [angle](double x1, double y1, double x2, double y2) {
        Polygon polygon(Points{Point::new_scale(x1,y1),Point::new_scale(x2,y1),Point::new_scale(x2,y2),Point::new_scale(x1,y2)});
        polygon.rotate(angle); return polygon;
    };
    ExPolygon left(rectangle(-20.,-20.,3.,20.)), right(rectangle(-3.,-19.,20.,19.));
    Polygon hole = rectangle(-1.,-1.,1.,1.); hole.reverse();
    left.holes.push_back(hole); right.holes.push_back(hole);
    std::vector<ColoredLines> contours;
    const auto add = [&](const Polygon &polygon, size_t color) {
        ColoredLines lines;
        for (const auto &line : polygon.lines()) lines.push_back({line,int(color),int(contours.size()),int(lines.size())});
        contours.push_back(std::move(lines));
    };
    add(hole,owner);
    // This source boundary lies outside the overlapping portion.
    add(rectangle(10.,-1.,12.,1.),other);
    if (conflicting_boundary) add(rectangle(-0.5,5.,0.5,6.),other);
    std::vector<ExPolygons> regions(4);
    regions[owner]={left}; regions[other]={right};
    if (third_region) regions[third]={ExPolygon(rectangle(-1.,10.,1.,12.))};
    ExPolygons before;
    for (const auto &group : regions) append(before,group);
    ExPolygons removable=diff_ex(intersection_ex(regions[owner],regions[other]),regions[third]);
    const ExPolygons expected_other=conflicting_boundary ? regions[other] : diff_ex(regions[other],removable);
    const ExPolygons expected_third=regions[third];
    SegmentationDetail::repair_boundary_owned_overlaps(regions,contours);
    const auto area_mm2=[](const ExPolygons &polys) {double sum=0.;for(const auto &p:polys)sum+=p.area()*SCALING_FACTOR*SCALING_FACTOR;return sum;};
    CAPTURE(owner,conflicting_boundary,third_region,angle);
    CHECK_THAT(area_mm2(diff_ex(regions[other],expected_other)),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(expected_other,regions[other])),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(ExPolygons{left},regions[owner])),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(regions[owner],ExPolygons{left})),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(expected_third,regions[third])),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(regions[third],expected_third)),Catch::Matchers::WithinAbs(0.,1e-6));
    ExPolygons after;
    for (const auto &group : regions) append(after,group);
    // Intersections are rounded onto the integer coordinate lattice. The
    // unmodified reference subtraction also loses 4.361358e-6 mm² in the
    // rotated fixture, so a constant area bound is not a valid coverage oracle.
    // Permit only a two-coordinate rounding band along the original edges
    // (2e-6 mm); keep the original area diagnostic in BoundaryOverlapQuantization.
    const Polygons rounding_band=offset(to_polylines(before),2.f);
    CHECK_THAT(area_mm2(diff_ex(diff_ex(before,after),rounding_band)),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(area_mm2(diff_ex(after,before)),Catch::Matchers::WithinAbs(0.,1e-6));
    // A 0.01 mm loss at the same boundary must not fit inside this allowance.
    const ExPolygons visible_loss=diff_ex(ExPolygons{left},offset_ex(ExPolygons{left},-float(scale_(0.01))));
    CHECK(area_mm2(diff_ex(visible_loss,rounding_band)) > 1e-6);
}

TEST_CASE("Painted shell propagation gives each buffer cell a single layer group owner", "[MultiMaterialSegmentation][Regression]")
{
    const size_t num_layers = GENERATE(size_t(0), size_t(1), size_t(7), size_t(19), size_t(500));
    const size_t reach = GENERATE(size_t(1), size_t(2), size_t(4), size_t(7));
    std::mutex mutex;
    std::vector<std::array<size_t, 3>> groups;
    SegmentationDetail::for_each_painting_layer_group(num_layers, reach, [&](size_t begin, size_t end, size_t offset) {
        // Collect first: Catch assertions stay on the calling thread. The mutex
        // makes the test safe even if a future grouping regression overlaps work.
        std::lock_guard<std::mutex> lock(mutex);
        groups.push_back({begin, end, offset});
    });
    std::vector<size_t> visits(num_layers, 0);
    std::vector<std::set<size_t>> top_owners(2 * num_layers), bottom_owners(2 * num_layers);
    for (size_t group = 0; group < groups.size(); ++group) {
        const auto [begin, end, offset] = groups[group];
        REQUIRE(begin < end);
        REQUIRE(end <= num_layers);
        for (size_t layer = begin; layer < end; ++layer) {
            ++visits[layer];
            // Check the full allowed footprint, including source layer and both
            // clipped ends. Geometry-dependent early exits only shrink it.
            const size_t lower = layer > reach ? layer - reach : 0;
            const size_t upper = std::min(num_layers - 1, layer + reach);
            for (size_t target = lower; target <= layer; ++target)
                top_owners.at(offset + target).insert(group);
            for (size_t target = layer; target <= upper; ++target)
                bottom_owners.at(offset + target).insert(group);
        }
    }
    for (size_t visited : visits)
        REQUIRE(visited == 1);
    for (size_t cell = 0; cell < 2 * num_layers; ++cell) {
        REQUIRE(top_owners[cell].size() <= 1);
        REQUIRE(bottom_owners[cell].size() <= 1);
    }
}

TEST_CASE("Projected painting overlap reduction ignores worker insertion order", "[MultiMaterialSegmentation][Regression]")
{
    using namespace SegmentationDetail;
    const coord_t unit = scale_(1.0);
    const Points first_points{{0, 0}, {10 * unit, 0}, {20 * unit, 0}};
    const Points second_points{{0, unit}, {10 * unit, unit}};
    const std::vector<EdgeGrid::Contour> contours{
        EdgeGrid::Contour(first_points, true), EdgeGrid::Contour(second_points, true)};
    const std::vector<PaintedLine> input{
        {0, 0, Line({0, 0}, {2 * unit, 0}), 3},
        {0, 0, Line({0, 0}, {2 * unit, 0}), 1},
        {0, 0, Line({0, 0}, {10 * unit, 0}), 4},
        {0, 0, Line({4 * unit, 0}, {6 * unit, 0}), 2},
        {0, 1, Line({10 * unit, 0}, {20 * unit, 0}), 2},
        {1, 0, Line({0, unit}, {10 * unit, unit}), 3}};
    // Keep the existing earlier-start / shorter-length precedence. Only the
    // identical short projections use the deterministic lower color-id tie-break.
    const std::vector<std::vector<PaintedLine>> expected{
        {{0, 0, Line({0, 0}, {2 * unit, 0}), 1},
         {0, 0, Line({2 * unit, 0}, {10 * unit, 0}), 4}, input[4]},
        {input[5]}};
    std::array<size_t, 6> order{0, 1, 2, 3, 4, 5};
    size_t permutations = 0;
    do {
        std::vector<PaintedLine> shuffled;
        for (size_t i : order)
            shuffled.push_back(input[i]);
        const auto actual = post_process_painted_lines(contours, std::move(shuffled));
        REQUIRE(actual.size() == expected.size());
        for (size_t c = 0; c < expected.size(); ++c) {
            REQUIRE(actual[c].size() == expected[c].size());
            for (size_t i = 0; i < expected[c].size(); ++i) {
                REQUIRE(actual[c][i].contour_idx == expected[c][i].contour_idx);
                REQUIRE(actual[c][i].line_idx == expected[c][i].line_idx);
                REQUIRE(actual[c][i].projected_line.a == expected[c][i].projected_line.a);
                REQUIRE(actual[c][i].projected_line.b == expected[c][i].projected_line.b);
                REQUIRE(actual[c][i].color == expected[c][i].color);
            }
        }
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    REQUIRE(permutations == 720);
    REQUIRE(post_process_painted_lines(contours, {}).empty());
}

TEST_CASE("Multi-material segmentation resolves the outer-wall line width", "[MultiMaterialSegmentation][Regression]")
{
    struct Case
    {
        std::string         description;
        double              outer_value;
        bool                outer_percent;
        double              line_value;
        bool                line_percent;
        std::vector<double> nozzle_diameters;
        int                 outer_wall_filament_id;
        double              expected;
    };

    auto c = GENERATE(values<Case>({
        {"absolute outer-wall width is used as-is",      0.6, false, 0.42, false, {0.4},      1, 0.6},
        {"percent outer-wall width uses the nozzle",     120, true,  0.42, false, {0.5},      1, 0.6},
        {"zero outer-wall width uses the line width",    0,   false, 0.5,  false, {0.4},      1, 0.5},
        {"zero outer-wall width uses a percent line",    0,   false, 100,  true,  {0.5},      1, 0.5},
        {"zero width falls back to auto",                0,   false, 0,    false, {0.4},      1, Flow::auto_extrusion_width(frExternalPerimeter, 0.4)},
        {"the auto fallback scales with the nozzle",     0,   false, 0,    false, {0.6},      1, Flow::auto_extrusion_width(frExternalPerimeter, 0.6)},
        {"a percent width uses the outer wall's nozzle", 120, true,  0.42, false, {0.4, 0.8}, 2, 0.96},
        {"the auto width uses the outer wall's nozzle",  0,   false, 0,    false, {0.4, 0.8}, 2, Flow::auto_extrusion_width(frExternalPerimeter, 0.8)},
        {"an absolute width ignores the nozzle",         0.6, false, 0.42, false, {0.4, 0.8}, 2, 0.6},
        {"a zero percent width uses the line width",     0,   true,  0.5,  false, {0.4},      1, 0.5},
        {"an unset filament id uses the first nozzle",   0,   false, 0,    false, {0.4, 0.8}, 0, Flow::auto_extrusion_width(frExternalPerimeter, 0.4)},
        {"an out-of-range filament id uses nozzle 1",    0,   false, 0,    false, {0.4, 0.8}, 5, Flow::auto_extrusion_width(frExternalPerimeter, 0.4)},
    }));

    DYNAMIC_SECTION(c.description)
    {
        PrintConfig print_config;
        print_config.nozzle_diameter.values = c.nozzle_diameters;

        PrintObjectConfig object_config;
        object_config.line_width = ConfigOptionFloatOrPercent(c.line_value, c.line_percent);

        PrintRegionConfig region_config;
        region_config.outer_wall_line_width        = ConfigOptionFloatOrPercent(c.outer_value, c.outer_percent);
        region_config.outer_wall_filament_id.value = c.outer_wall_filament_id;

        REQUIRE_THAT(resolve_outer_wall_line_width(region_config, object_config, print_config),
                     Catch::Matchers::WithinAbs(c.expected, 1e-9));
    }
}
