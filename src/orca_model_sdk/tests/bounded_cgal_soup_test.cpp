#include "BoundedCgalSoup.hpp"
#include <array>
#include <vector>
#include <cstdint>
#include <iostream>
#include <random>
using Face=std::array<uint32_t,3>;
using Soup=std::vector<Face>;
int main() {
    const Soup tetra{{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    struct Case { const char* name; Soup faces; bool expected; };
    std::vector<Case> cases{{"empty",{},true},{"closed",tetra,true},{"open",{{0,1,2}},true},
        {"ordered_edge_duplicate",{{0,1,2},{0,1,3}},false},
        {"repeated_vertex",{{0,0,1}},false},
        {"nonmanifold_edge",{{0,1,2},{1,0,3},{0,1,4}},false},
        {"singular_vertex",{{0,2,1},{0,1,3},{0,3,2},{1,2,3},{0,5,4},{0,4,6},{0,6,5},{4,5,6}},false}};
    auto reverse=tetra;for(auto& f:reverse)std::swap(f[1],f[2]);cases.push_back({"reversed",reverse,true});
    for(const auto& test:cases) {
        const auto original=CGAL::Polygon_mesh_processing::is_polygon_soup_a_polygon_mesh(test.faces);
        const auto adapted=CGAL::Polygon_mesh_processing::orca_model_bounded::is_polygon_soup_a_polygon_mesh(test.faces);
        if(original!=test.expected || adapted!=original){std::cerr<<"FAIL "<<test.name<<" original="<<original<<" adapted="<<adapted<<std::endl;return 1;}
    }
    std::mt19937 random(20261119);
    for(size_t i=0;i<256;++i) {
        Soup soup;for(size_t j=0;j<(i%17);++j)soup.push_back({uint32_t(random()%8),uint32_t(random()%8),uint32_t(random()%8)});
        const auto before=soup;
        const auto original=CGAL::Polygon_mesh_processing::is_polygon_soup_a_polygon_mesh(soup);
        const auto adapted=CGAL::Polygon_mesh_processing::orca_model_bounded::is_polygon_soup_a_polygon_mesh(soup);
        if(original!=adapted || soup!=before)return 2;
    }
    std::cout<<"PASS 8 named original-versus-private CGAL cases + 256 deterministic differential soups"<<std::endl;
    return 0;
}
