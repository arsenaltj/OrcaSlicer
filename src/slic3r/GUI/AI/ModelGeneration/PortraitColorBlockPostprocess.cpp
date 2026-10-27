#include "PortraitColorBlockPostprocess.hpp"
#include "ModelPreviewPalette.hpp"
#include "PortraitShapeDetails.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_set>

namespace Slic3r::GUI::PortraitColorBlocks {
namespace {
using RGB = SC::Color;
struct Edge {
    uint32_t a, b, unit;
    uint8_t axis, line, lo, hi;
    bool operator<(const Edge& e) const {
        return std::tie(a,b,axis,line,lo,hi,unit) < std::tie(e.a,e.b,e.axis,e.line,e.lo,e.hi,e.unit);
    }
    bool same_support(const Edge& e) const { return a==e.a && b==e.b && axis==e.axis && line==e.line; }
};
struct Link { uint32_t a,b; double length; };
enum class BlockedKind : uint8_t { Color, Semantic, Topology, NonManifold, Historical };
struct BlockedBoundary {
    uint32_t a = 0;
    uint32_t b = UINT32_MAX;
    BlockedKind kind = BlockedKind::Topology;
    float color_distance = 0.f;
};
struct Bounds { std::array<std::array<int,3>,3> bary; };
bool color_valid(const RGB& c) {
    return std::all_of(c.begin(),c.end(),[](float v){return std::isfinite(v) && v>=0 && v<=1;});
}
bool seed(SC::Label l) {
    return l==SC::Label::Iris || l==SC::Label::EyeSclera || l==SC::Label::Lips ||
        l==SC::Label::FaceSkin || l==SC::Label::BodySkin;
}
const char* role_name(SC::Label label) {
    switch (label) {
    case SC::Label::Iris: return "Iris";
    case SC::Label::EyeSclera: return "EyeSclera";
    case SC::Label::FaceSkin:
    case SC::Label::BodySkin: return "Skin";
    case SC::Label::Hair: return "Hair";
    case SC::Label::Lips:
    case SC::Label::MouthInterior: return "Lips";
    case SC::Label::Eyebrow: return "Eyebrow";
    default: return nullptr;
    }
}
bool protected_role(SC::Label label) { return role_name(label) != nullptr; }
bool core_role(const std::string& role) {
    return role == "Skin" || role == "Lips" || role == "EyeSclera" || role == "Iris";
}
int role_priority(const std::string& role) {
    if (role == "Iris") return 0;
    if (role == "EyeSclera") return 1;
    if (role == "Lips") return 2;
    if (role == "Skin") return 3;
    if (role == "Eyebrow") return 4;
    if (role == "Hair") return 5;
    return 6;
}
// Color is the primary partition signal. Keep a boundary when a continuous
// color block crosses a confirmed anatomy label; unknown units stay separate
// from a seed so conflicting seeds can remain unresolved during propagation.
bool protected_semantic(SC::Label label) {
    return label==SC::Label::Iris || label==SC::Label::EyeSclera || label==SC::Label::Lips;
}
bool semantic_boundary(SC::Label a, float confidence_a, SC::Label b, float confidence_b) {
    if (a == b)
        return false;
    if (a == SC::Label::Background || b == SC::Label::Background)
        return true;
    // Facial features remain hard semantic boundaries. Skin labels are
    // protected only when both sides are high-confidence; an uncertain or
    // unknown skin edge may join a continuous same-color garment/hair/skin
    // region instead of producing a triangular fragment.
    if (protected_semantic(a) || protected_semantic(b))
        return true;
    const bool uncertain = a==SC::Label::Unknown || b==SC::Label::Unknown ||
        confidence_a<SC::minimum_confidence || confidence_b<SC::minimum_confidence;
    const bool skin_boundary = a==SC::Label::FaceSkin || a==SC::Label::BodySkin ||
        b==SC::Label::FaceSkin || b==SC::Label::BodySkin;
    return skin_boundary && !uncertain;
}
bool propagation_boundary(SC::Label a, float confidence_a, SC::Label b, float confidence_b) {
    if (a==b || a==SC::Label::Unknown || b==SC::Label::Unknown)
        return false;
    if (a==SC::Label::Background || b==SC::Label::Background)
        return true;
    if (protected_semantic(a) || protected_semantic(b))
        return true;
    const bool skin_boundary = a==SC::Label::FaceSkin || a==SC::Label::BodySkin ||
        b==SC::Label::FaceSkin || b==SC::Label::BodySkin;
    return skin_boundary && confidence_a>=SC::minimum_confidence && confidence_b>=SC::minimum_confidence;
}
float lab_delta(const RGB& a,const RGB& b) {
    const float l=(a[0]-b[0])*.65f;
    return l*l+(a[1]-b[1])*(a[1]-b[1])+(a[2]-b[2])*(a[2]-b[2]);
}
float delta(RGB a,RGB b) {return lab_delta(PreviewPalette::to_lab(a),PreviewPalette::to_lab(b));}
bool prefix(SC::SubfacePath a, SC::SubfacePath b) {
    return a.depth<=b.depth && (b.value>>(2*(b.depth-a.depth)))==a.value;
}
void checkpoint(const SC::Cancel& cancel, size_t n) {
    if ((n&4095)==0 && cancel && cancel()) throw std::runtime_error("人像颜色块处理已取消");
}
void choose_primary_cause(BlockDiagnostic &diagnostic) {
    const auto count = [&](BlockCause cause) {
        switch (cause) {
        case BlockCause::ColorThreshold: return diagnostic.color_blocked_edges;
        case BlockCause::SemanticBoundary: return diagnostic.semantic_blocked_edges;
        case BlockCause::TopologyDisconnected: return diagnostic.topology_blocked_edges;
        case BlockCause::NonManifold: return diagnostic.nonmanifold_edges;
        case BlockCause::HistoricalLock: return diagnostic.historical_locked_edges;
        case BlockCause::None: return size_t(0);
        }
        return size_t(0);
    };
    // Explicit locks and non-manifold geometry are higher-signal causes when
    // they tie with ordinary color/semantic boundaries.
    const std::array<BlockCause, 5> priority{{BlockCause::HistoricalLock,
        BlockCause::NonManifold, BlockCause::SemanticBoundary,
        BlockCause::ColorThreshold, BlockCause::TopologyDisconnected}};
    const bool has_interblock_cause = diagnostic.color_blocked_edges ||
        diagnostic.semantic_blocked_edges || diagnostic.nonmanifold_edges ||
        diagnostic.historical_locked_edges;
    size_t best = 0;
    diagnostic.primary = BlockCause::None;
    for (const auto cause : priority) {
        if (has_interblock_cause && cause == BlockCause::TopologyDisconnected)
            continue;
        const size_t value = count(cause);
        if (value > best) {
            best = value;
            diagnostic.primary = cause;
        }
    }
    if (diagnostic.primary == BlockCause::None && diagnostic.boundary_count)
        diagnostic.primary = BlockCause::TopologyDisconnected;
}
}

size_t Result::unresolved_count() const {
    return size_t(std::count_if(current().blocks.begin(),current().blocks.end(),
        [](const Block& b){return b.status==Status::Unresolved;}));
}
DiagnosticSummary Result::diagnostic_summary() const {
    DiagnosticSummary summary;
    for (const auto &block : current().blocks) {
        switch (block.diagnostic.primary) {
        case BlockCause::ColorThreshold: ++summary.color_threshold; break;
        case BlockCause::SemanticBoundary: ++summary.semantic_boundary; break;
        case BlockCause::TopologyDisconnected: ++summary.topology_disconnected; break;
        case BlockCause::NonManifold: ++summary.nonmanifold; break;
        case BlockCause::HistoricalLock: ++summary.historical_locked; break;
        case BlockCause::None: break;
        }
        summary.color_blocked_edges += block.diagnostic.color_blocked_edges;
        summary.semantic_blocked_edges += block.diagnostic.semantic_blocked_edges;
        summary.topology_blocked_edges += block.diagnostic.topology_blocked_edges;
        summary.nonmanifold_edges += block.diagnostic.nonmanifold_edges;
        summary.historical_locked_edges += block.diagnostic.historical_locked_edges;
    }
    return summary;
}
std::vector<uint32_t> Result::face_partition(
    const std::vector<std::vector<int32_t>>& face_neighbors) const {
    std::vector<uint32_t> result(face_count, UINT32_MAX);
    std::vector<std::map<uint32_t, double>> weights(face_count);
    const auto &track = current();
    for (size_t i = 0; i < units.size(); ++i) {
        if (i >= track.unit_blocks.size() || track.unit_blocks[i] >= track.blocks.size())
            throw std::runtime_error("颜色块面分区包含无效块身份");
        const auto &unit = units[i];
        if (unit.face >= face_count) throw std::runtime_error("颜色块面分区包含无效面身份");
        // A depth-d subface has 1/4^d of its parent triangle's area.
        const double weight = std::ldexp(1.0, -2 * int(unit.path.depth));
        weights[unit.face][track.unit_blocks[i]] += weight;
    }
    for (size_t face = 0; face < weights.size(); ++face) {
        if (weights[face].empty()) throw std::runtime_error("颜色块面分区未覆盖模型面");
        const auto best = std::max_element(weights[face].begin(), weights[face].end(),
            [](const auto &a, const auto &b) {
                return a.second != b.second ? a.second < b.second : a.first > b.first;
            });
        result[face] = best->first + 1; // BeautyPuzzle reserves zero as empty.
    }
    if (!face_neighbors.empty()) {
        if (face_neighbors.size() != result.size())
            throw std::runtime_error("颜色块面分区邻接关系与模型面数不一致");
        uint32_t next_id = 0;
        for (const auto id : result) next_id = std::max(next_id, id);
        ++next_id;
        std::vector<uint8_t> visited(result.size(), 0);
        std::unordered_set<uint32_t> seen_ids;
        std::vector<size_t> queue;
        for (size_t seed = 0; seed < result.size(); ++seed) {
            if (visited[seed]) continue;
            const uint32_t source_id = result[seed];
            visited[seed] = 1;
            queue.assign(1, seed);
            for (size_t at = 0; at < queue.size(); ++at) {
                const size_t face = queue[at];
                for (const int32_t neighbor : face_neighbors[face]) {
                    if (neighbor < 0 || size_t(neighbor) >= result.size() || visited[size_t(neighbor)] ||
                        result[size_t(neighbor)] != source_id)
                        continue;
                    visited[size_t(neighbor)] = 1;
                    queue.push_back(size_t(neighbor));
                }
            }
            // The first connected component keeps the color-block identity;
            // later islands receive fresh identities instead of being merged
            // by RGB or rejected as a disconnected BeautyPuzzle piece.
            const uint32_t id = seen_ids.insert(source_id).second ? source_id : next_id++;
            if (id != source_id) for (const size_t face : queue) result[face] = id;
        }
    }
    return result;
}
void Result::appearance(SC::FaceColors& faces, SC::SubfaceColors& children) const {
    faces.clear(); children.clear();
    const auto& t=current(); faces.reserve(face_count);
    for (size_t i=0;i<units.size();++i) {
        const auto& u=units[i]; const auto& b=t.blocks[t.unit_blocks[i]];
        if (!u.path.depth) faces.emplace_back(u.face,b.display);
        else children.push_back({u.face,u.path,b.display,b.confidence});
    }
    std::sort(children.begin(),children.end(),[](const auto& a,const auto& b){return std::tie(a.face_id,a.path)<std::tie(b.face_id,b.path);});
}
bool Result::recolor(uint32_t id, const RGB& color) {
    if (!color_valid(color) || id>=current().blocks.size()) return false;
    auto& b=tracks[size_t(active)].blocks[id];
    b.display=color; b.status=Status::ManualConfirmed; b.candidate=-1;
    b.display_slot=-1; b.reason.clear(); b.confidence=1; return true;
}
size_t Result::memory_bytes() const {
    size_t bytes=sizeof(*this)+units.capacity()*sizeof(Unit);
    for(const auto& t:tracks) {
        bytes+=t.unit_blocks.capacity()*sizeof(uint32_t)+t.blocks.capacity()*sizeof(Block);
        for(const auto& b:t.blocks) bytes+=b.members.capacity()*sizeof(uint32_t)+
            b.neighbors.capacity()*sizeof(std::pair<uint32_t,double>)+b.reason.capacity();
    }
    return bytes;
}

std::shared_ptr<Result> build(const Input& in, const SC::Cancel& cancel) {
    const auto& source=in.source; const auto& mesh=source.mesh;
    const size_t count=mesh.indices.size();
    if (!count || count>maximum_units || mesh.vertices.size()>maximum_units*3 || in.analysis.geometry_id!=source.geometry_id ||
        in.analysis.content_id!=source.content_id || in.analysis.face_labels.size()!=count ||
        in.analysis.face_confidence.size()!=count || (!in.person_ids.empty() && in.person_ids.size()!=count))
        throw std::runtime_error("人像颜色块的几何或识别身份不一致");
    auto result=std::make_shared<Result>();
    result->face_count=count; result->geometry_id=source.geometry_id;
    result->strategy=in.strategy;
    result->algorithm_version = in.strategy == Strategy::ColorRegionMajorityV2
        ? portrait_postprocess_algorithm_version : 0;
    result->source_signature=source.content_id; result->semantic_signature=in.analysis.signature;
    result->control_signature=controls_signature(in.mapping,in.targets);
    nlohmann::json palette={{"mapping",in.mapping},{"targets",in.targets},{"faces",in.processed_faces}};
    palette["children"]=nlohmann::json::array();
    for(const auto& c:in.processed_subfaces) palette["children"].push_back({c.face_id,c.path.depth,c.path.value,c.color});
    result->palette_signature=PortraitShapeCache::digest(palette.dump());
    palette=nlohmann::json();
    std::map<size_t,std::vector<SC::SubfaceColor>> children;
    struct ManualUnit { SC::SubfacePath path; RGB color; uint32_t block; };
    std::array<std::map<size_t,std::vector<ManualUnit>>,2> manual;
    if(in.previous) {
        if(in.previous->geometry_id!=source.geometry_id || in.previous->source_signature!=source.content_id ||
            in.previous->face_count!=count)throw std::runtime_error("手动颜色块的源模型身份已变化，原配色已保留");
        for(size_t ti=0;ti<2;++ti)for(const auto& b:in.previous->tracks[ti].blocks)
            if(b.status==Status::ManualConfirmed)for(auto member:b.members) {
                if(member>=in.previous->units.size())throw std::runtime_error("手动颜色块成员无效");
                const auto& u=in.previous->units[member];
                manual[ti][u.face].push_back({u.path,b.display,b.id});
            }
    }
    std::map<size_t,std::vector<SC::SubfaceLabelEvidence>> evidence;
    for(const auto& e:in.analysis.subface_labels) evidence[e.face_id].push_back(e);
    for(const auto& c:in.processed_subfaces) {
        if(c.face_id>=count || !c.path.depth || c.path.depth>4 ||
            unsigned(c.path.value)>=(1u<<(2*c.path.depth)) || !color_valid(c.color))
            throw std::runtime_error("普通配色包含无效子面");
        children[c.face_id].push_back(c);
    }
    std::vector<RGB> raw, processed;
    std::vector<SC::Label> labels;
    std::vector<float> confidence;
    std::vector<uint32_t> people;
    std::array<std::vector<uint32_t>,2> manual_blocks;
    std::array<std::vector<RGB>,2> manual_colors;
    std::map<size_t,RGB> overrides(in.processed_faces.begin(),in.processed_faces.end());
    std::vector<RGB> lab;
    for(const auto& c:in.mapping) lab.push_back(PreviewPalette::to_lab(c));
    const bool v2 = in.strategy == Strategy::ColorRegionMajorityV2;
    const auto &analysis_palette = v2 && !in.intermediate_palette.empty() ? in.intermediate_palette : in.targets;
    if(!v2 && in.mapping.size()!=in.targets.size()) throw std::runtime_error("普通配色色板映射不完整");
    if(v2 && analysis_palette.empty()) throw std::runtime_error("人像颜色区域缺少十色中间调色板");
    std::vector<RGB> analysis_lab;
    for (const auto& color : analysis_palette) analysis_lab.push_back(PreviewPalette::to_lab(color));
    const bool vertex=source.vertex_colors.size()==mesh.vertices.size();
    const bool face=source.face_colors.size()==count;
    for(size_t f=0;f<count;++f) {
        checkpoint(cancel,f);
        const auto child=children.find(f);
        std::vector<SC::SubfacePath> pending{{0,0}};
        while(!pending.empty()) {
            const auto path=pending.back(); pending.pop_back();
            const auto detail=evidence.find(f);
            bool split=(child!=children.end() && std::any_of(child->second.begin(),child->second.end(),
                [&](const auto& c){return c.path.depth>path.depth && prefix(path,c.path);})) ||
                (!v2 && detail!=evidence.end() && std::any_of(detail->second.begin(),detail->second.end(),[&](const auto& e){
                    return e.path.depth<=4 && e.path.depth>path.depth && prefix(path,e.path) &&
                        e.confidence>=SC::minimum_confidence && seed(e.label);
                }));
            for(const auto& track:manual)if(const auto it=track.find(f);it!=track.end())
                split=split || std::any_of(it->second.begin(),it->second.end(),[&](const auto& u){
                    return u.path.depth>path.depth && prefix(path,u.path);
                });
            if(split) { for(int c=3;c>=0;--c) pending.push_back({uint8_t(path.depth+1),uint8_t(path.value*4+c)}); continue; }
            if(result->units.size()>=maximum_units) throw std::runtime_error("人像颜色块超过安全单元预算");
            const auto bary_corners=corners(path);
            RGB rgb{};
            for(const auto& bary:bary_corners) for(size_t k=0;k<3;++k) for(size_t c=0;c<3;++c) {
                const auto index=mesh.indices[f][k];
                if(index<0 || size_t(index)>=mesh.vertices.size()) throw std::runtime_error("无效网格顶点");
                rgb[c]+=bary[k]*(vertex?source.vertex_colors[index][c]:face?source.face_colors[f][c]:.7f)/3.f;
            }
            RGB mapped=rgb;
            if(v2) mapped=analysis_palette[PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(rgb),analysis_lab)];
            else if(!lab.empty()) mapped=in.targets[PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(rgb),lab)];
            if(const auto it=overrides.find(f);it!=overrides.end()) mapped=it->second;
            if(child!=children.end()) {
                uint8_t depth=0;
                for(const auto& c:child->second) if(prefix(c.path,path) && c.path.depth>=depth) {mapped=c.color;depth=c.path.depth;}
            }
            if(!color_valid(rgb) || !color_valid(mapped))throw std::runtime_error("颜色块输入包含无效颜色");
            auto label=in.analysis.face_labels[f]; auto conf=in.analysis.face_confidence[f];
            uint8_t depth=0;
            // Existing subface evidence is used only where it covers the unit.
            // No new semantic subdivisions or contour expansions are created.
            if(detail!=evidence.end()) for(const auto& e:detail->second) if(prefix(e.path,path) && e.path.depth>depth) {
                depth=e.path.depth;label=e.label;conf=e.confidence;
            }
            if(!std::isfinite(conf) || conf<0 || conf>1 || size_t(label)>=SC::label_count)
                throw std::runtime_error("颜色块输入包含无效识别证据");
            if(conf<SC::minimum_confidence) label=SC::Label::Unknown;
            result->units.push_back({f,path}); raw.push_back(rgb); processed.push_back(mapped);
            for(size_t ti=0;ti<2;++ti) {
                uint32_t id=UINT32_MAX;RGB color{};
                if(const auto it=manual[ti].find(f);it!=manual[ti].end())for(const auto& u:it->second)
                    if(prefix(u.path,path)) {id=u.block;color=u.color;break;}
                manual_blocks[ti].push_back(id);manual_colors[ti].push_back(color);
            }
            labels.push_back(label); confidence.push_back(conf);
            people.push_back(in.person_ids.empty()?0:in.person_ids[f]);
        }
    }
    // Exact coincident seam vertices share identity. No tolerance welding,
    // radius neighbors or normal-angle barriers are introduced.
    std::vector<uint32_t> order(mesh.vertices.size()), canonical(mesh.vertices.size());
    std::iota(order.begin(),order.end(),0);
    const auto position=[&](uint32_t v){const auto& p=mesh.vertices[v];return std::make_tuple(p.x(),p.y(),p.z());};
    for(const auto& p:mesh.vertices) if(!p.allFinite()) throw std::runtime_error("非有限网格顶点");
    std::sort(order.begin(),order.end(),[&](auto a,auto b){return position(a)<position(b);});
    uint32_t identity=0;
    for(size_t i=0;i<order.size();++i) {
        if(i && position(order[i])!=position(order[i-1])) ++identity;
        canonical[order[i]]=identity;
    }
    std::vector<Edge> edges; edges.reserve(result->units.size()*3);
    for(uint32_t i=0;i<result->units.size();++i) {
        checkpoint(cancel,i);
        const auto& u=result->units[i];
        const auto bary_corners=corners(u.path);
        Bounds bounds;
        for(int k=0;k<3;++k) for(int c=0;c<3;++c) bounds.bary[k][c]=int(std::lround(bary_corners[k][c]*16));
        for(int e=0;e<3;++e) {
            const auto& x=bounds.bary[e];const auto& y=bounds.bary[(e+1)%3];
            int axis=0;while(axis<3 && x[axis]!=y[axis]) ++axis;
            if(axis==3) throw std::runtime_error("子面边不是中点树边");
            const int k=(axis+1)%3, l=(axis+2)%3;
            Edge edge{uint32_t(u.face),0,i,uint8_t(axis+1),uint8_t(x[axis]),uint8_t(std::min(x[k],y[k])),uint8_t(std::max(x[k],y[k]))};
            if(x[axis]==0) {
                auto a=canonical[mesh.indices[u.face][k]],b=canonical[mesh.indices[u.face][l]];
                int lo=std::min(x[l],y[l]),hi=std::max(x[l],y[l]);
                if(a>b) {std::swap(a,b);const int prev=lo;lo=16-hi;hi=16-prev;}
                edge={a,b,i,0,0,uint8_t(lo),uint8_t(hi)};
            }
            edges.push_back(edge);
        }
    }
    std::sort(edges.begin(),edges.end());
    std::vector<Link> links;
    std::vector<BlockedBoundary> topology_boundaries;
    std::vector<std::pair<uint32_t, uint32_t>> shared_pairs;
    for(size_t start=0;start<edges.size();) {
        checkpoint(cancel,start);
        size_t end=start+1;while(end<edges.size() && edges[start].same_support(edges[end])) ++end;
        // At most two incident surfaces per atomic segment. Non-manifold
        // supports are barriers, never bridges between touching people.
        for(int segment=0;segment<16;++segment) {
            std::vector<const Edge*> incident;
            for(size_t k=start;k<end;++k) if(edges[k].lo<=segment && edges[k].hi>segment)
                incident.push_back(&edges[k]);
            if(incident.size()!=2) {
                const auto kind = incident.size()>2 ? BlockedKind::NonManifold : BlockedKind::Topology;
                for(const auto *entry : incident)
                    topology_boundaries.push_back({entry->unit,UINT32_MAX,kind,0.f});
                continue;
            }
            const Edge* a=incident[0];const Edge* b=incident[1];
            if(a->unit==b->unit) continue;
            shared_pairs.emplace_back(std::min(a->unit,b->unit),std::max(a->unit,b->unit));
            const auto& u=result->units[a->unit];const auto& tri=mesh.indices[u.face];
            double length=0;
            if(a->axis==0) {
                for(int k=0;k<3;++k) if(canonical[tri[k]]==a->a)
                    for(int l=0;l<3;++l) if(canonical[tri[l]]==a->b) length=(mesh.vertices[tri[k]]-mesh.vertices[tri[l]]).norm()/16.;
            } else {
                int k=(a->axis)%3,l=(a->axis+1)%3;
                length=(mesh.vertices[tri[k]]-mesh.vertices[tri[l]]).norm()/16.;
            }
            const auto first=std::min(a->unit,b->unit),second=std::max(a->unit,b->unit);
            if(!links.empty() && links.back().a==first && links.back().b==second)links.back().length+=length;
            else links.push_back({first,second,length});
        }
        start=end;
    }
    std::sort(links.begin(),links.end(),[](const auto& a,const auto& b){return std::tie(a.a,a.b)<std::tie(b.a,b.b);});
    size_t written=0;
    for(const auto& link:links) {
        if(written && links[written-1].a==link.a && links[written-1].b==link.b) links[written-1].length+=link.length;
        else links[written++]=link;
    }
    links.resize(written); edges.clear(); edges.shrink_to_fit();
    std::sort(shared_pairs.begin(),shared_pairs.end());
    shared_pairs.erase(std::unique(shared_pairs.begin(),shared_pairs.end()),shared_pairs.end());
    std::vector<std::vector<uint32_t>> adjacency(result->units.size());
    for(const auto& l:links) { adjacency[l.a].push_back(l.b);adjacency[l.b].push_back(l.a); }
    for(size_t ti=0;ti<2;++ti) {
        const auto& colors=ti==0?raw:processed; auto& track=result->tracks[ti];
        // V2 establishes one canonical partition from the original surface
        // appearance.  The processed ten-color track is display data only;
        // using it to build the graph would merge a garment into a nearby
        // skin tone before the final palette allocator can preserve it.
        const auto& partition_colors = v2 ? raw : colors;
        std::vector<RGB> color_labs; color_labs.reserve(partition_colors.size());
        for(const auto& color:partition_colors)color_labs.push_back(PreviewPalette::to_lab(color));
        track.unit_blocks.assign(colors.size(),UINT32_MAX);
        for(uint32_t i=0;i<colors.size();++i) {
            checkpoint(cancel,i); if(track.unit_blocks[i]!=UINT32_MAX) continue;
            Block block;block.id=uint32_t(track.blocks.size());block.original=block.display=colors[i];
            block.canonical = raw[i];
            block.semantic=labels[i];
            block.status=seed(labels[i])?Status::Confirmed:Status::Unresolved;
            block.confidence=labels[i]!=SC::Label::Unknown?confidence[i]:0;
            if(block.status==Status::Unresolved)block.reason=labels[i]==SC::Label::Unknown?
                "no_confirmed_lighter_neighbor":"non_anatomy_semantic_barrier";
            const bool locked=manual_blocks[ti][i]!=UINT32_MAX;
            if(locked) {block.display=manual_colors[ti][i];block.status=Status::ManualConfirmed;block.confidence=1;block.reason.clear();}
            block.members.push_back(i);track.unit_blocks[i]=block.id;
            for(size_t q=0;q<block.members.size();++q) for(const auto n:adjacency[block.members[q]]) {
                if(track.unit_blocks[n]!=UINT32_MAX || (!v2 && people[n]!=people[i]) ||
                    (!v2 && semantic_boundary(labels[n],confidence[n],labels[i],confidence[i])) ||
                    manual_blocks[ti][n]!=manual_blocks[ti][i] ||
                    // V1 keeps its historical neighbor-to-neighbor traversal so
                    // old saved results remain compatible. V2 partitions the
                    // original appearance and must compare every candidate to
                    // the block seed; comparing to the most recently accepted
                    // face lets a gradual gradient drift across the whole mesh
                    // and can bind one semantic seed to an enormous region.
                    (!locked && lab_delta(color_labs[n],
                        color_labs[v2 ? i : block.members[q]]) > same_color_distance)) continue;
                track.unit_blocks[n]=block.id;block.members.push_back(n);
            }
            for(const auto member:block.members) if(seed(labels[member])) {
                block.semantic=labels[member];block.status=Status::Confirmed;
                block.confidence=std::max(block.confidence,confidence[member]);
                block.reason.clear();break;
            }
            // Preserve a stable source representative for later card switches
            // and error scoring.  Area weighting avoids letting a subdivided
            // child face outweigh its parent unit.
            if (v2) {
                RGB canonical_color{}; double canonical_area = 0;
                for (const auto member : block.members) {
                    const double area = std::ldexp(1.0, -2 * int(result->units[member].path.depth));
                    canonical_area += area;
                    for (size_t c = 0; c < 3; ++c) canonical_color[c] += raw[member][c] * float(area);
                }
                if (canonical_area > 0)
                    for (auto& channel : canonical_color) channel = float(channel / canonical_area);
                block.canonical = canonical_color;
            } else {
                block.canonical = block.original;
            }
            track.blocks.push_back(std::move(block));
            // Bound fragmented input while it grows, rather than waiting for
            // all blocks and their boundary maps to be allocated.
            if((track.blocks.size()&4095)==0 && result->memory_bytes()>256ULL*1024*1024)
                throw std::runtime_error("颜色碎块超过安全内存预算，已保留普通配色");
        }
        std::map<std::pair<uint32_t,uint32_t>,double> boundaries;
        for(const auto& l:links) {
            auto a=track.unit_blocks[l.a],b=track.unit_blocks[l.b];if(a==b)continue;
            if(a>b)std::swap(a,b);boundaries[{a,b}]+=l.length;
        }
        for(const auto& p:boundaries) {
            track.blocks[p.first.first].neighbors.emplace_back(p.first.second,p.second);
            track.blocks[p.first.second].neighbors.emplace_back(p.first.first,p.second);
        }
        // Attribute every boundary that prevented a pair of units from
        // joining. The same topology evidence is shared by both tracks,
        // while color thresholds are evaluated against the active track's
        // actual face colors.
        const auto add_boundary = [&](uint32_t id, BlockedKind kind, float color_distance) {
            if(id>=track.blocks.size()) return;
            auto &diagnostic=track.blocks[id].diagnostic;
            ++diagnostic.boundary_count;
            diagnostic.max_color_distance=std::max(diagnostic.max_color_distance,color_distance);
            switch(kind) {
            case BlockedKind::Color: ++diagnostic.color_blocked_edges; break;
            case BlockedKind::Semantic: ++diagnostic.semantic_blocked_edges; break;
            case BlockedKind::Topology: ++diagnostic.topology_blocked_edges; break;
            case BlockedKind::NonManifold: ++diagnostic.nonmanifold_edges; break;
            case BlockedKind::Historical: ++diagnostic.historical_locked_edges; break;
            }
        };
        for(const auto &boundary:topology_boundaries) {
            if(boundary.a>=track.unit_blocks.size()) continue;
            const auto block_id=track.unit_blocks[boundary.a];
            add_boundary(block_id,boundary.kind,0.f);
        }
        for(const auto &[left,right]:shared_pairs) {
            if(left>=track.unit_blocks.size() || right>=track.unit_blocks.size()) continue;
            const auto left_block=track.unit_blocks[left],right_block=track.unit_blocks[right];
            if(left_block==right_block) continue;
            const float color_distance=lab_delta(color_labs[left],color_labs[right]);
            BlockedKind kind;
            if(manual_blocks[ti][left]!=manual_blocks[ti][right]) kind=BlockedKind::Historical;
            else if(!v2 && (people[left]!=people[right] || semantic_boundary(labels[left],confidence[left],labels[right],confidence[right]))) kind=BlockedKind::Semantic;
            else if(color_distance>same_color_distance) kind=BlockedKind::Color;
            else continue;
            add_boundary(left_block,kind,color_distance);
            add_boundary(right_block,kind,color_distance);
        }
        for(auto &block:track.blocks) choose_primary_cause(block.diagnostic);
        if (v2) {
            // ColorRegionMajorityV2 is intentionally a two-step operation:
            // topology and color establish the region first, then semantics
            // annotate that region by weighted majority.  No semantic edge is
            // allowed to split the region.
            struct RoleVote { double score = 0; double area = 0; RGB color {}; };
            std::map<std::string, RoleVote> role_votes;
            for (auto &block : track.blocks) {
                std::map<std::string, double> votes, areas;
                std::map<SC::Label, double> label_votes;
                RGB average{}; double total_area = 0;
                for (const auto member : block.members) {
                    const double area = std::ldexp(1.0, -2 * int(result->units[member].path.depth));
                    total_area += area;
                    // Semantic votes and all palette decisions use the
                    // original source appearance.  The processed track is
                    // only a display candidate and must not erase a garment
                    // color before the allocator sees it.
                    for (size_t c=0; c<3; ++c) average[c] += raw[member][c] * float(area);
                    if (confidence[member] < SC::minimum_confidence) continue;
                    if (labels[member] != SC::Label::Unknown && labels[member] != SC::Label::Background)
                        label_votes[labels[member]] += area * confidence[member];
                    if (const auto role = role_name(labels[member])) {
                        votes[role] += area * confidence[member]; areas[role] += area;
                    }
                }
                if (total_area > 0) for (auto &c : average) c = float(c / total_area);
                const auto winner = std::max_element(votes.begin(), votes.end(), [](const auto &a, const auto &b) {
                    if (a.second != b.second) return a.second < b.second;
                    const int ap = role_priority(a.first), bp = role_priority(b.first);
                    return ap != bp ? ap > bp : a.first > b.first;
                });
                const double total_vote = std::accumulate(votes.begin(), votes.end(), 0.0,
                    [](double value, const auto &item) { return value + item.second; });
                const auto label_winner = std::max_element(label_votes.begin(), label_votes.end(),
                    [](const auto &a, const auto &b) {
                        if (a.second != b.second) return a.second < b.second;
                        return int(a.first) > int(b.first);
                    });
                const bool role_confirmed = winner != votes.end() && winner->second >= total_vote * .55 && winner->second > 0;
                const bool label_confirmed = label_winner != label_votes.end() &&
                    label_winner->second >= std::accumulate(label_votes.begin(), label_votes.end(), 0.0,
                        [](double value, const auto &item) { return value + item.second; }) * .55;
                if (role_confirmed || label_confirmed) {
                    if (role_confirmed) {
                        const auto &role = winner->first;
                        block.semantic = role == "Iris" ? SC::Label::Iris : role == "EyeSclera" ? SC::Label::EyeSclera :
                            role == "Skin" ? SC::Label::FaceSkin : role == "Hair" ? SC::Label::Hair :
                            role == "Lips" ? SC::Label::Lips : SC::Label::Eyebrow;
                        block.confidence = float(winner->second / std::max(total_vote, 1e-12));
                    } else {
                        block.semantic = label_winner->first;
                        const double label_total = std::accumulate(label_votes.begin(), label_votes.end(), 0.0,
                            [](double value, const auto &item) { return value + item.second; });
                        block.confidence = float(label_winner->second / std::max(label_total, 1e-12));
                    }
                    block.status = block.confidence >= .70f ? Status::Confirmed : Status::Unresolved;
                    block.reason = block.status == Status::Confirmed ? std::string() : "low_confidence_semantic_vote";
                    if (block.status == Status::Confirmed && role_confirmed) {
                        const auto &role = winner->first;
                        auto &vote = role_votes[role];
                        const double score = winner->second;
                        if (vote.score > 0) for (size_t c=0; c<3; ++c)
                            vote.color[c] = float((vote.color[c] * vote.score + average[c] * score) / (vote.score + score));
                        else vote.color = average;
                        vote.score += score; vote.area += areas[role];
                        if (ti == 1) {
                            result->role_regions[role].push_back(block.id);
                            result->role_confidence[role] = std::max(result->role_confidence[role], block.confidence);
                        }
                    }
                    // Keep dark secondary anatomy identities when a same-color
                    // region is won by another role.  The region remains one
                    // color block by design, but hair/eyebrow must still be
                    // able to reuse the darkest legal slot after a card switch.
                    if (ti == 1) {
                        for (const auto &[observed_role, score] : votes) {
                            if (core_role(observed_role) ||
                                (winner != votes.end() && observed_role == winner->first) ||
                                (observed_role != "Hair" && observed_role != "Eyebrow"))
                                continue;
                            auto &secondary = role_votes[observed_role];
                            if (secondary.score > 0) for (size_t c = 0; c < 3; ++c)
                                secondary.color[c] = float((secondary.color[c] * secondary.score + average[c] * score) /
                                    (secondary.score + score));
                            else secondary.color = average;
                            secondary.score += score;
                            secondary.area += areas[observed_role];
                            result->role_regions[observed_role].push_back(block.id);
                            result->role_confidence[observed_role] = std::max(
                                result->role_confidence[observed_role], float(score / std::max(total_vote, 1e-12)));
                        }
                    }
                } else {
                    block.semantic = SC::Label::Unknown; block.status = Status::Unresolved;
                    block.confidence = total_vote > 0 && winner != votes.end() ? float(winner->second / total_vote) : 0;
                    block.reason = "unresolved_color_region";
                }
                block.display = block.original;
            }
            if (!portrait_color_postprocess_supported(in.final_color_count))
                throw std::runtime_error("人像颜色区域后处理仅支持四至六色");
            const size_t final_count = in.final_color_count;
            std::vector<std::pair<std::string, RoleVote>> roles;
            for (const auto &[role, vote] : role_votes)
                if (vote.score > 0) roles.emplace_back(role, vote);
            std::sort(roles.begin(), roles.end(), [](const auto& a, const auto& b) {
                const bool ac = core_role(a.first), bc = core_role(b.first);
                if (ac != bc) return ac > bc;
                if (a.second.area != b.second.area) return a.second.area > b.second.area;
                const int ap = role_priority(a.first), bp = role_priority(b.first);
                return ap != bp ? ap < bp : a.first < b.first;
            });
            // The final palette is the automatic suggestion's slot vector.
            // V2 may bind regions to different slots, but must never rebuild
            // or reorder those slots. This keeps 4/5/6-color identity stable
            // across ordinary preview, portrait optimization and persistence.
            if (!in.targets.empty() && in.targets.size() != final_count)
                throw std::runtime_error("人像颜色区域的 AI 配色槽位数量不一致");
            std::vector<RGB> palette;
            if (!in.targets.empty()) {
                palette = in.targets;
            } else {
                const auto append_distinct = [&](const RGB& color) {
                    if (!color_valid(color) || palette.size() >= final_count) return;
                    if (std::none_of(palette.begin(), palette.end(), [&](const RGB& other) {
                        return delta(color, other) < 1e-8f;
                    })) palette.push_back(color);
                };
                for (const auto& color : in.mapping) append_distinct(color);
                for (const auto& color : analysis_palette) append_distinct(color);
                for (const auto& block : track.blocks) append_distinct(block.canonical);
                if (palette.empty()) throw std::runtime_error("人像颜色区域缺少合法色板");
                while (palette.size() < final_count) palette.push_back(palette.back());
            }
            for (const auto& color : palette)
                if (!color_valid(color)) throw std::runtime_error("人像颜色区域包含无效 AI 配色值");
            result->final_slot_indices.resize(palette.size());
            std::iota(result->final_slot_indices.begin(), result->final_slot_indices.end(), size_t(0));
            std::vector<RGB> palette_lab;
            palette_lab.reserve(palette.size());
            for (const auto& color : palette) palette_lab.push_back(PreviewPalette::to_lab(color));

            std::map<std::string, size_t> role_slots;
            std::vector<size_t> used_core_slots;
            const auto is_dark_role = [](const std::string& role) {
                return role == "Iris" || role == "Hair" || role == "Eyebrow";
            };
            const auto choose_slot = [&](const RGB& desired, bool dark) -> std::optional<size_t> {
                size_t selected = palette.size();
                float best_distance = std::numeric_limits<float>::max();
                float best_lightness = std::numeric_limits<float>::max();
                for (size_t slot = 0; slot < palette.size(); ++slot) {
                    if (std::find(used_core_slots.begin(), used_core_slots.end(), slot) != used_core_slots.end()) continue;
                    if (std::any_of(used_core_slots.begin(), used_core_slots.end(), [&](size_t used) {
                        return delta(palette[slot], palette[used]) < 1e-8f;
                    })) continue;
                    const float distance = delta(palette[slot], desired);
                    const float lightness = palette_lab[slot][0];
                    if (selected == palette.size() || (dark && (lightness < best_lightness - 1e-7f ||
                            (std::abs(lightness - best_lightness) <= 1e-7f && distance < best_distance))) ||
                        (!dark && distance < best_distance)) {
                        selected = slot; best_distance = distance; best_lightness = lightness;
                    }
                }
                return selected < palette.size() ? std::optional<size_t>(selected) : std::nullopt;
            };
            if (!in.fixed_role_slots.empty()) {
                for (const auto& [role, slot] : in.fixed_role_slots)
                    if (role.empty() || slot >= palette.size())
                        throw std::runtime_error("荣耀套装语义槽位引用无效");
                // Product-card locks are deliberately non-exclusive. Hair,
                // iris and eyebrow share charcoal; sclera and teeth share
                // white. Only roles actually confirmed by V2 are persisted.
                for (const auto& [role, vote] : roles) {
                    const auto fixed = in.fixed_role_slots.find(role);
                    if (fixed != in.fixed_role_slots.end()) role_slots[role] = fixed->second;
                    else {
                        const auto nearest = PreviewPalette::nearest_lab_index(
                            PreviewPalette::to_lab(vote.color), palette_lab);
                        role_slots[role] = nearest < palette.size() ? nearest : 0;
                    }
                }
            } else {
                // Core anatomy roles are hard slots.  Use the role vote first
                // and then a legal target candidate if source colors collide.
                for (const auto &[role, vote] : roles) {
                    if (!core_role(role)) continue;
                    const auto selected = choose_slot(vote.color, is_dark_role(role));
                    if (!selected) throw std::runtime_error("当前色卡无法提供四个独立保护颜色");
                    role_slots[role] = *selected;
                    used_core_slots.push_back(*selected);
                }
                if (role_slots.size() > final_count)
                    throw std::runtime_error("人像核心角色数量超过最终色数");
                // Hair and eyebrows keep their semantic identity but share the
                // darkest original suggestion slot.
                for (const auto &[role, vote] : roles) {
                    if (core_role(role)) continue;
                    if (is_dark_role(role)) {
                        const auto darkest = std::min_element(palette_lab.begin(), palette_lab.end(),
                            [](const RGB& a, const RGB& b) { return a[0] < b[0]; });
                        role_slots[role] = size_t(darkest - palette_lab.begin());
                    } else {
                        const auto nearest = PreviewPalette::nearest_lab_index(
                            PreviewPalette::to_lab(vote.color), palette_lab);
                        role_slots[role] = nearest < palette.size() ? nearest : 0;
                    }
                }
            }
            if (ti == 1) {
                result->role_locks.clear();
                result->role_slot_locks = role_slots;
                for (const auto& [role, slot] : role_slots)
                    result->role_locks[role] = palette[slot];
            }
            result->intermediate_palette = analysis_palette; result->final_palette = palette;
            for (auto &block : track.blocks) {
                if (block.status == Status::ManualConfirmed) {
                    block.display_slot = -1;
                    continue;
                }
                auto index = PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(block.canonical), palette_lab);
                if (const auto role = role_name(block.semantic);
                    role && (in.fixed_role_slots.empty() ||
                        (block.status == Status::Confirmed &&
                         block.confidence >= fixed_role_minimum_confidence))) {
                    const auto lock = role_slots.find(role);
                    if (lock != role_slots.end()) index = lock->second;
                }
                block.display_slot = int64_t(index < palette.size() ? index : 0);
                block.display = palette[size_t(block.display_slot)];
            }
            continue;
        }
        // Propagation reads only original high-confidence seeds. Temporary
        // candidates and newly inherited labels never become new seeds.
        const auto originally_confirmed=[&](uint32_t b){return seed(labels[track.blocks[b].members.front()]);};
        for(auto& b:track.blocks) if(b.status==Status::Unresolved && b.semantic==SC::Label::Unknown) {
            SC::Label found=SC::Label::Unknown; bool conflict=false;float conf=1;
            for(const auto& n:b.neighbors) {
                const auto& other=track.blocks[n.first];
                if(!originally_confirmed(n.first) || people[b.members.front()]!=people[other.members.front()] ||
                    propagation_boundary(labels[b.members.front()],confidence[b.members.front()],
                        labels[other.members.front()],confidence[other.members.front()]) ||
                    delta(b.original,other.original)>same_color_distance)continue;
                if(found!=SC::Label::Unknown && found!=other.semantic)conflict=true;
                found=other.semantic;conf=std::min(conf,other.confidence);
            }
            if(!conflict && found!=SC::Label::Unknown) {b.semantic=found;b.status=Status::Confirmed;b.confidence=conf;b.reason.clear();}
            else if(conflict)b.reason="conflicting_semantic_seeds";
        }
        for(auto& b:track.blocks) if(b.status==Status::Unresolved && b.semantic==SC::Label::Unknown && b.reason!="conflicting_semantic_seeds") {
            const float light=PreviewPalette::to_lab(b.original)[0];
            const Block* best=nullptr;double boundary=-1;float distance=0;
            for(const auto& n:b.neighbors) {
                const auto& candidate=track.blocks[n.first];
                if(candidate.status!=Status::Confirmed || !originally_confirmed(n.first) ||
                    people[b.members.front()]!=people[candidate.members.front()] ||
                    PreviewPalette::to_lab(candidate.original)[0]<=light) continue;
                const float d=delta(b.original,candidate.original);
                if(!best || n.second>boundary || (n.second==boundary && (d<distance || (d==distance && candidate.id<best->id)))) {
                    best=&candidate;boundary=n.second;distance=d;
                }
            }
            if(best) {b.display=best->original;b.candidate=best->id;b.confidence=best->confidence*.5f;b.reason="lighter_neighbor_preview";}
        }
        if(result->memory_bytes()>256ULL*1024*1024)throw std::runtime_error("颜色碎块超过安全内存预算，已保留普通配色");
    }
    if(cancel && cancel())throw std::runtime_error("人像颜色块处理已取消");
    return result;
}

bool remap_to_palette(Result& result, const std::vector<SC::Color>& target, bool preserve_regions,
                      const std::map<std::string, size_t>& fixed_role_slots)
{
    if (target.empty() || target.size() > PreviewPalette::max_preview_colors)
        return false;
    for (const auto& color : target) if (!color_valid(color)) return false;

    // Old V2 caches do not have role_regions.  Reconstruct it from the block
    // labels once, so the new card-switch path remains backward compatible.
    if (result.role_regions.empty()) {
        for (const auto& block : result.current().blocks)
            if (const auto role = role_name(block.semantic))
                result.role_regions[role].push_back(block.id);
    }
    std::map<std::string, RGB> source_roles = result.role_locks;
    for (const auto& [role, ids] : result.role_regions) {
        if (source_roles.count(role) || ids.empty()) continue;
        const auto& block = result.current().blocks[std::min<size_t>(ids.front(), result.current().blocks.size() - 1)];
        source_roles[role] = block.canonical;
    }
    std::vector<RGB> target_labs;
    for (const auto& color : target) target_labs.push_back(PreviewPalette::to_lab(color));
    result.role_locks.clear();
    result.role_slot_locks.clear();
    std::map<std::string, size_t> role_slots;
    std::vector<size_t> used;
    std::vector<std::string> roles;
    for (const auto& [role, color] : source_roles) if (core_role(role)) roles.push_back(role);
    std::sort(roles.begin(), roles.end(), [](const auto& a, const auto& b) {
        const int ap = role_priority(a), bp = role_priority(b);
        return ap != bp ? ap < bp : a < b;
    });
    if (!fixed_role_slots.empty()) {
        for (const auto& [role, slot] : fixed_role_slots)
            if (role.empty() || slot >= target.size()) return false;
        for (const auto& [role, color] : source_roles) {
            const auto fixed = fixed_role_slots.find(role);
            if (fixed == fixed_role_slots.end()) continue;
            role_slots[role] = fixed->second;
            result.role_locks[role] = target[fixed->second];
        }
    } else {
        if (target.size() < roles.size()) return false;
        for (const auto& role : roles) {
            const auto desired = PreviewPalette::to_lab(source_roles[role]);
            size_t best = target.size(); float distance = std::numeric_limits<float>::max();
            float lightness = std::numeric_limits<float>::max();
            for (size_t i = 0; i < target.size(); ++i) {
                if (std::find(used.begin(), used.end(), i) != used.end()) continue;
                if (std::any_of(used.begin(), used.end(), [&](size_t used_slot) {
                    return delta(target[i], target[used_slot]) < 1e-8f;
                })) continue;
                const float d = PreviewPalette::distance(desired, target_labs[i]);
                const bool dark = role == "Iris" || role == "Hair" || role == "Eyebrow";
                const float candidate_lightness = target_labs[i][0];
                if (best == target.size() || (dark && (candidate_lightness < lightness - 1e-7f ||
                        (std::abs(candidate_lightness - lightness) <= 1e-7f && d < distance))) ||
                    (!dark && d < distance)) {
                    distance = d; lightness = candidate_lightness; best = i;
                }
            }
            if (best == target.size()) return false;
            role_slots[role] = best; used.push_back(best); result.role_locks[role] = target[best];
        }
        for (const auto& [role, color] : source_roles) {
            if (core_role(role) || !(role == "Hair" || role == "Eyebrow")) continue;
            const auto darkest = std::min_element(target_labs.begin(), target_labs.end(),
                [](const RGB& lhs, const RGB& rhs) { return lhs[0] < rhs[0]; });
            const size_t slot = size_t(darkest - target_labs.begin());
            role_slots[role] = slot;
            result.role_locks[role] = target[slot];
        }
    }
    result.role_slot_locks = role_slots;
    for (auto& track : result.tracks) {
        for (auto& block : track.blocks) {
            // A manually confirmed block is an explicit user choice and is a
            // hard lock across color-card changes.
            if (block.status == Status::ManualConfirmed) {
                block.display_slot = -1;
                continue;
            }
            const auto role = role_name(block.semantic);
            if (preserve_regions && role && (fixed_role_slots.empty() ||
                    (block.status == Status::Confirmed &&
                     block.confidence >= fixed_role_minimum_confidence))) {
                const auto slot = role_slots.find(role);
                if (slot != role_slots.end()) {
                    block.display = target[slot->second];
                    block.display_slot = int64_t(slot->second);
                    continue;
                }
            }
            const auto nearest = PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(block.canonical), target_labs);
            block.display_slot = int64_t(nearest < target.size() ? nearest : 0);
            block.display = target[size_t(block.display_slot)];
        }
    }
    result.final_palette = target;
    result.final_slot_indices.resize(target.size());
    std::iota(result.final_slot_indices.begin(), result.final_slot_indices.end(), size_t(0));
    return true;
}

size_t reliable_fixed_role_blocks(const Result& result,
                                  const std::map<std::string, size_t>& fixed_role_slots,
                                  float minimum_confidence)
{
    if (fixed_role_slots.empty() || !std::isfinite(minimum_confidence)) return 0;
    return size_t(std::count_if(result.current().blocks.begin(), result.current().blocks.end(),
        [&](const Block& block) {
            const auto role = role_name(block.semantic);
            return role && fixed_role_slots.count(role) && block.status == Status::Confirmed &&
                block.confidence >= minimum_confidence;
        }));
}

std::vector<size_t> used_display_slots(const Result& result)
{
    std::vector<size_t> slots;
    for (const auto& block : result.current().blocks) {
        if (block.display_slot < 0 || size_t(block.display_slot) >= result.final_palette.size()) continue;
        slots.push_back(size_t(block.display_slot));
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    return slots;
}

std::string controls_signature(const std::vector<SC::Color>& mapping,const std::vector<SC::Color>& targets) {
    // Bump the cache contract when connected-color behavior changes. Existing
    // saved candidates must be rebuilt instead of silently reusing fragmented
    // regions produced by an older traversal rule.
    return PortraitShapeCache::digest(nlohmann::json({{"strategy_revision",7},
        {"mapping",mapping},{"targets",targets}}).dump());
}
nlohmann::json encode(const Result& r) {
    nlohmann::json doc={{"schema",r.strategy==Strategy::ColorRegionMajorityV2 ? "orca.portrait-color-blocks/v2" : "orca.portrait-color-blocks/v1"},{"geometry_id",r.geometry_id},
        {"source_signature",r.source_signature},{"semantic_signature",r.semantic_signature},{"palette_signature",r.palette_signature},
        {"control_signature",r.control_signature},{"face_count",r.face_count},{"active_track",int(r.active)},{"units",nlohmann::json::array()},{"tracks",nlohmann::json::array()}};
    if (r.strategy == Strategy::ColorRegionMajorityV2) {
        doc["strategy"] = int(r.strategy); doc["algorithm_version"] = r.algorithm_version;
        doc["intermediate_palette"] = r.intermediate_palette;
        doc["final_palette"] = r.final_palette; doc["role_locks"] = r.role_locks;
        doc["final_slot_indices"] = r.final_slot_indices;
        doc["role_slot_locks"] = r.role_slot_locks;
        doc["role_regions"] = r.role_regions; doc["role_confidence"] = r.role_confidence;
    }
    for(const auto& u:r.units)doc["units"].push_back({u.face,u.path.depth,u.path.value});
    for(const auto& t:r.tracks) {
        nlohmann::json blocks=nlohmann::json::array();
        for(const auto& b:t.blocks) blocks.push_back({{"id",b.id},{"members",b.members},{"original",b.original},{"display",b.display},{"canonical",b.canonical},
            {"semantic",int(b.semantic)},{"status",int(b.status)},{"confidence",b.confidence},{"reason",b.reason},
            {"neighbors",b.neighbors},{"candidate",b.candidate},{"display_slot",b.display_slot},
            {"diagnostic",{{"primary",int(b.diagnostic.primary)},
                {"boundary_count",b.diagnostic.boundary_count},
                {"color_blocked_edges",b.diagnostic.color_blocked_edges},
                {"semantic_blocked_edges",b.diagnostic.semantic_blocked_edges},
                {"topology_blocked_edges",b.diagnostic.topology_blocked_edges},
                {"nonmanifold_edges",b.diagnostic.nonmanifold_edges},
                {"historical_locked_edges",b.diagnostic.historical_locked_edges},
                {"max_color_distance",b.diagnostic.max_color_distance}}}});
        doc["tracks"].push_back({{"blocks",std::move(blocks)},{"unit_blocks",t.unit_blocks}});
    }
    return doc;
}
std::shared_ptr<Result> decode(const nlohmann::json& doc,const std::string& geometry,size_t faces,
    const std::string& source,const std::string& semantic,const std::string& palette,std::string& error) {
    try {
        const auto require=[](bool ok){if(!ok)throw std::runtime_error("颜色块缓存身份或内容无效，已保留普通配色");};
        const auto bounded=[&](const nlohmann::json& value,uint64_t maximum) {
            require(value.is_number_integer());
            if(value.is_number_integer() && !value.is_number_unsigned())require(value.get<int64_t>()>=0);
            require(value.get<uint64_t>()<=maximum);
            return value.get<uint64_t>();
        };
        require(faces>0 && faces<=maximum_units);
        const auto schema=doc.at("schema").get<std::string>();
        const bool v2=schema=="orca.portrait-color-blocks/v2";
        require((v2 || schema=="orca.portrait-color-blocks/v1") && doc.at("geometry_id")==geometry &&
            doc.at("face_count")==faces && doc.at("source_signature")==source &&
            doc.at("semantic_signature")==semantic && doc.at("palette_signature")==palette);
        auto r=std::make_shared<Result>();r->geometry_id=geometry;r->face_count=faces;r->source_signature=source;
        r->semantic_signature=semantic;r->palette_signature=palette;
        r->strategy=v2 ? Strategy::ColorRegionMajorityV2 : Strategy::ConnectedColorBlocksV1;
        if (v2) {
            require(doc.contains("algorithm_version") && doc.at("algorithm_version").is_number_unsigned() &&
                doc.at("algorithm_version").get<uint32_t>() == portrait_postprocess_algorithm_version &&
                doc.contains("intermediate_palette") && doc.contains("final_palette") && doc.contains("role_locks") &&
                doc.contains("final_slot_indices") && doc.contains("role_slot_locks"));
            r->algorithm_version=doc.at("algorithm_version").get<uint32_t>();
            r->intermediate_palette=doc.at("intermediate_palette").get<std::vector<RGB>>();
            r->final_palette=doc.at("final_palette").get<std::vector<RGB>>();
            r->role_locks=doc.at("role_locks").get<std::map<std::string,RGB>>();
            r->final_slot_indices=doc.at("final_slot_indices").get<std::vector<size_t>>();
            r->role_slot_locks=doc.at("role_slot_locks").get<std::map<std::string,size_t>>();
            require(!r->intermediate_palette.empty() && !r->final_palette.empty() &&
                r->final_palette.size()<=PreviewPalette::max_preview_colors);
            require(r->final_slot_indices.size()==r->final_palette.size());
            for (const auto &color:r->intermediate_palette) require(color_valid(color));
            for (const auto &color:r->final_palette) require(color_valid(color));
            for (const auto &[role,color]:r->role_locks) { require(!role.empty()); require(color_valid(color)); }
            std::vector<uint8_t> slot_seen(r->final_palette.size(),0);
            for (const auto slot:r->final_slot_indices) {
                require(slot<r->final_palette.size() && !slot_seen[slot]);
                slot_seen[slot]=1;
            }
            for (const auto &[role,slot]:r->role_slot_locks) {
                require(!role.empty() && slot<r->final_palette.size());
                const auto color=r->role_locks.find(role);
                require(color!=r->role_locks.end() && delta(color->second,r->final_palette[slot])<1e-8f);
            }
            if (doc.contains("role_regions")) {
                r->role_regions=doc.at("role_regions").get<std::map<std::string,std::vector<uint32_t>>>();
                for (const auto &[role,ids]:r->role_regions) {
                    require(!role.empty());
                    for (const auto id:ids) require(id<maximum_units);
                }
            }
            if (doc.contains("role_confidence")) {
                r->role_confidence=doc.at("role_confidence").get<std::map<std::string,float>>();
                for (const auto &[role,value]:r->role_confidence)
                    require(!role.empty() && std::isfinite(value) && value>=0 && value<=1);
            }
        }
        r->control_signature=doc.at("control_signature").get<std::string>();
        require(r->control_signature.size()==64 && std::all_of(r->control_signature.begin(),r->control_signature.end(),
            [](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}));
        r->active=Track(bounded(doc.at("active_track"),1));
        const auto& units=doc.at("units");require(units.is_array() && !units.empty() && units.size()<=maximum_units);
        std::vector<size_t> coverage(faces,0);
        std::map<size_t,std::vector<SC::SubfacePath>> paths;
        for(const auto& u:units) {
            require(u.is_array() && u.size()==3);
            const size_t f=bounded(u[0],faces-1);const int d=int(bounded(u[1],4)),p=int(bounded(u[2],255));
            require(f<faces && d>=0 && d<=4 && p>=0 && p<(1<<(2*d)));
            SC::SubfacePath path{uint8_t(d),uint8_t(p)};
            for(const auto& prev:paths[f])require(!prefix(prev,path) && !prefix(path,prev));
            paths[f].push_back(path);coverage[f]+=size_t(1)<<(2*(4-d));r->units.push_back({f,path});
        }
        for(auto c:coverage)require(c==256);
        require(doc.at("tracks").is_array() && doc.at("tracks").size()==2);
        for(size_t ti=0;ti<2;++ti) {
            auto& t=r->tracks[ti];const auto& saved=doc.at("tracks")[ti];
            const auto& bs=saved.at("blocks");require(bs.is_array() && bs.size()<=units.size());
            require(!bs.empty() && saved.at("unit_blocks").is_array());
            for(const auto& item:saved.at("unit_blocks"))bounded(item,bs.size()-1);
            t.unit_blocks=saved.at("unit_blocks").get<std::vector<uint32_t>>();require(t.unit_blocks.size()==units.size());
            std::vector<uint8_t> seen(units.size(),0);
            for(const auto& b:bs) {
                Block block;block.id=uint32_t(bounded(b.at("id"),bs.size()-1));require(block.id==t.blocks.size());
                require(b.at("members").is_array());
                for(const auto& item:b.at("members"))bounded(item,units.size()-1);
                block.members=b.at("members").get<std::vector<uint32_t>>();require(!block.members.empty());
                for(auto m:block.members) {require(m<units.size() && !seen[m] && t.unit_blocks[m]==block.id);seen[m]=1;}
                block.original=b.at("original").get<RGB>();block.display=b.at("display").get<RGB>();
                block.canonical=b.contains("canonical") ? b.at("canonical").get<RGB>() : block.original;
                require(color_valid(block.original) && color_valid(block.display) && color_valid(block.canonical));
                require(b.at("semantic").is_number_integer() && b.at("status").is_number_integer() && b.at("candidate").is_number_integer());
                const int label=int(bounded(b.at("semantic"),SC::label_count-1)),status=int(bounded(b.at("status"),2));
                block.semantic=SC::Label(label);block.status=Status(status);block.confidence=b.at("confidence");
                require(std::isfinite(block.confidence) && block.confidence>=0 && block.confidence<=1);
                block.reason=b.at("reason");
                if(b.at("candidate")==-1)block.candidate=-1;
                else block.candidate=int64_t(bounded(b.at("candidate"),bs.size()-1));
                if (b.contains("display_slot")) {
                    require(b.at("display_slot").is_number_integer());
                    block.display_slot=b.at("display_slot").get<int64_t>();
                }
                if (v2) {
                    require(b.contains("display_slot") && block.display_slot>=-1 &&
                        block.display_slot<int64_t(r->final_palette.size()));
                    if (block.status!=Status::ManualConfirmed) {
                        require(block.display_slot>=0);
                        require(delta(block.display,r->final_palette[size_t(block.display_slot)])<1e-8f);
                    }
                }
                require(b.at("neighbors").is_array());
                for(const auto& n:b.at("neighbors")) {require(n.is_array() && n.size()==2);bounded(n[0],bs.size()-1);}
                block.neighbors=b.at("neighbors").get<std::vector<std::pair<uint32_t,double>>>();
                for(const auto& n:block.neighbors)require(n.first<bs.size() && n.first!=block.id && std::isfinite(n.second) && n.second>0);
                if (b.contains("diagnostic")) {
                    const auto &d=b.at("diagnostic");require(d.is_object());
                    if (d.contains("primary")) block.diagnostic.primary=BlockCause(bounded(d.at("primary"),5));
                    block.diagnostic.boundary_count=d.value("boundary_count",size_t(0));
                    block.diagnostic.color_blocked_edges=d.value("color_blocked_edges",size_t(0));
                    block.diagnostic.semantic_blocked_edges=d.value("semantic_blocked_edges",size_t(0));
                    block.diagnostic.topology_blocked_edges=d.value("topology_blocked_edges",size_t(0));
                    block.diagnostic.nonmanifold_edges=d.value("nonmanifold_edges",size_t(0));
                    block.diagnostic.historical_locked_edges=d.value("historical_locked_edges",size_t(0));
                    block.diagnostic.max_color_distance=d.value("max_color_distance",0.f);
                    require(std::isfinite(block.diagnostic.max_color_distance) && block.diagnostic.max_color_distance>=0);
                }
                t.blocks.push_back(std::move(block));
            }
            require(std::all_of(seen.begin(),seen.end(),[](auto v){return v==1;}));
        }
        error.clear();return r;
    } catch(const std::exception& e) {error=e.what();return {};}
}
} // namespace Slic3r::GUI::PortraitColorBlocks
