#pragma once

#include "SurfaceSelectionState.hpp"
#include "slic3r/AI/Contracts/LocalPrintColorResult.hpp"
#include <set>
#include <string>

namespace Slic3r::GUI::LocalSemanticEvidence {
using Json = nlohmann::json;
inline constexpr const char* schema = "orcaslicer.local-semantic-evidence.v1";
inline constexpr const char* label_schema = "farl-celebm-face19-subset-v1";
inline constexpr size_t max_faces = 2000000, max_vertices = 6000000;
inline constexpr size_t max_bytes = 128ULL * 1024 * 1024, max_subjects = 32, max_regions = 416;

inline bool supported_label(const std::string& value)
{
    static const std::set<std::string> labels {"neck","face","rr","lr","rb","lb","re","le","nose","imouth","llip","ulip","hair","cloth"};
    return labels.count(value) != 0;
}

// This cannot be constructed from a worker's echoed IDs. The host must first
// verify source bytes, then supply its actual native mesh and the render mesh.
class VerifiedFaceBinding {
public:
    bool valid() const { return m_count != 0; }
    const std::string& source_sha256() const { return m_source; }
    const std::string& geometry_id() const { return m_native; }
    const std::string& render_geometry_id() const { return m_render; }
    size_t face_count() const { return m_count; }
    bool usable_face(size_t face) const { return face<m_count && !m_ambiguous.count(face); }
    bool adjacent_faces(size_t a,size_t b) const {
        if(a==b || !usable_face(a) || !usable_face(b))return false;
        size_t shared=0;
        for(int x:m_mesh.indices[a])for(int y:m_mesh.indices[b])
            if(m_mesh.vertices[x]==m_mesh.vertices[y]){++shared;break;}
        return shared==2;
    }
private:
    std::string m_source, m_native, m_render;
    size_t m_count {0};
    std::set<size_t> m_ambiguous;
    indexed_triangle_set m_mesh; // Host-owned geometry for bounded context-path proof.
    friend bool prove_ordered_faces(const std::string&, const indexed_triangle_set&, const indexed_triangle_set&,
                                    VerifiedFaceBinding&, std::string&);
};

// Strict same-face/same-corner proof for a static mesh subset. No face search,
// centering, welding, winding reversal or unit conversion is performed here.
// Coincident triangles have unknown texture identity. Exclude those ordinals
// from evidence without discarding the independently proven rest of the mesh.
inline bool prove_ordered_faces(const std::string& verified_source_sha256, const indexed_triangle_set& native,
    const indexed_triangle_set& rendered, VerifiedFaceBinding& destination, std::string& error)
{
    error.clear();
    auto fail = [&] { error = "Native and render meshes do not prove the same ordered surface."; return false; };
    if (!AI::is_lowercase_sha256(verified_source_sha256) || native.indices.empty() || native.indices.size()>max_faces ||
        native.vertices.empty() || native.vertices.size()>max_vertices || native.indices.size()!=rendered.indices.size() ||
        native.vertices.size()!=rendered.vertices.size()) return fail();
    for (const auto& v:native.vertices) if (!v.allFinite()) return fail();
    for (const auto& v:rendered.vertices) if (!v.allFinite()) return fail();
    const size_t missing=std::numeric_limits<size_t>::max();
    std::vector<size_t> forward(native.vertices.size(),missing),reverse(rendered.vertices.size(),missing);
    for (size_t f=0;f<native.indices.size();++f) {
        const auto& a=native.indices[f]; const auto& b=rendered.indices[f];
        for (int c=0;c<3;++c) {
            if (a[c]<0 || b[c]<0 || size_t(a[c])>=forward.size() || size_t(b[c])>=reverse.size()) return fail();
            if ((forward[a[c]]!=missing && forward[a[c]]!=size_t(b[c])) ||
                (reverse[b[c]]!=missing && reverse[b[c]]!=size_t(a[c]))) return fail();
            forward[a[c]]=size_t(b[c]); reverse[b[c]]=size_t(a[c]);
            const Vec3d from=native.vertices[a[c]].cast<double>(),to=rendered.vertices[b[c]].cast<double>();
            const double scale=std::max({1.0,from.cwiseAbs().maxCoeff(),to.cwiseAbs().maxCoeff()});
            const double tolerance=std::min(1e-4,8.0*std::numeric_limits<float>::epsilon()*scale);
            if ((from-to).cwiseAbs().maxCoeff()>tolerance) return fail();
        }
        const auto x=native.vertices[a[0]].cast<double>().eval(), y=native.vertices[a[1]].cast<double>().eval(), z=native.vertices[a[2]].cast<double>().eval();
        const auto u=rendered.vertices[b[0]].cast<double>().eval(), v=rendered.vertices[b[1]].cast<double>().eval(), w=rendered.vertices[b[2]].cast<double>().eval();
        if ((y-x).cross(z-x).squaredNorm()<=0 || (v-u).cross(w-u).squaredNorm()<=0) return fail();
    }
    // Unreferenced vertices cannot be assigned an arbitrary correspondence.
    if (std::find(forward.begin(),forward.end(),missing)!=forward.end()) return fail();
    std::set<size_t> ambiguous;
    auto exclude_coincident=[&](const indexed_triangle_set& mesh) {
        using Corner=std::array<uint32_t,3>;
        using Triangle=std::array<Corner,3>;
        std::vector<std::pair<Triangle,size_t>> triangles; triangles.reserve(mesh.indices.size());
        for (const auto& face:mesh.indices) {
            Triangle triangle {};
            for (int c=0;c<3;++c) for (int axis=0;axis<3;++axis) {
                // Normalize signed zero exactly as the geometry fingerprint.
                const float value=mesh.vertices[face[c]][axis]==0.f ? 0.f : mesh.vertices[face[c]][axis];
                std::memcpy(&triangle[c][axis],&value,sizeof(value));
            }
            std::sort(triangle.begin(),triangle.end()); triangles.emplace_back(triangle,triangles.size());
        }
        std::sort(triangles.begin(),triangles.end());
        for(size_t i=1;i<triangles.size();++i) if(triangles[i-1].first==triangles[i].first) {
            ambiguous.insert(triangles[i-1].second);ambiguous.insert(triangles[i].second);
        }
    };
    exclude_coincident(native);exclude_coincident(rendered);
    if(ambiguous.size()==native.indices.size())return fail();
    VerifiedFaceBinding value;
    value.m_source=verified_source_sha256; value.m_count=native.indices.size();
    value.m_ambiguous=std::move(ambiguous);
    value.m_mesh=native;
    value.m_native=AI::SurfaceSelectionPersistence::geometry_fingerprint(native);
    value.m_render=AI::SurfaceSelectionPersistence::geometry_fingerprint(rendered);
    if (!AI::is_lowercase_sha256(value.m_native) || !AI::is_lowercase_sha256(value.m_render)) return fail();
    destination=std::move(value); return true;
}

struct ExpectedIdentity {
    std::string request_id, source_sha256, geometry_id;
    size_t face_count {0};
    std::string weights_sha256, runtime_sha256, policy_sha256;
    // Host policy, never worker-controlled fields. Scores are uncalibrated.
    double minimum_confidence {.9}, minimum_dominance {.85};
    size_t maximum_views {16};
    std::set<std::string> protected_labels {"re","le","llip","ulip","imouth"};
};
struct EyeDetail {
    std::string subject_id, label;
    std::vector<size_t> aperture_faces, iris_faces;
};
struct FeatureDetail {
    std::string subject_id, label;
    std::vector<size_t> faces, iris_faces;
    size_t view_support {0};
};
struct Evidence {
    ExpectedIdentity identity;
    std::string render_geometry_id;
    std::vector<std::string> subjects;
    std::vector<AI::PrintColorRegion> regions;
    // -1 means unknown, including low confidence, unobserved and conflicts.
    std::vector<int32_t> face_regions;
    // Optional geometry hints, separate from semantic confidence and authority.
    std::vector<EyeDetail> eye_details;
    std::vector<FeatureDetail> feature_details;
    size_t known_faces {0}, unknown_faces {0}, ambiguous_faces {0}, below_threshold_faces {0};
};

namespace detail {
inline bool identifier(const std::string& value)
{
    return !value.empty() && value.size()<=96 && std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.';
    });
}
inline void require(bool value,const char* reason) { if (!value) throw std::invalid_argument(reason); }
inline size_t index(const Json& j,size_t limit)
{
    require(j.is_number_unsigned() || (j.is_number_integer() && j.get<int64_t>()>=0),"Invalid semantic index.");
    const auto value=j.get<uint64_t>(); require(value<=limit,"Semantic index exceeds limit."); return size_t(value);
}
inline double probability(const Json& j)
{
    require(j.is_number(),"Semantic score is not numeric.");
    const double value=j.get<double>(); require(std::isfinite(value) && value>=0 && value<=1,"Invalid semantic score."); return value;
}
inline void keys(const Json& j,const std::set<std::string>& allowed)
{
    require(j.is_object() && j.size()==allowed.size(),"Unexpected semantic fields.");
    for (auto i=j.begin();i!=j.end();++i) require(allowed.count(i.key())!=0,"Unexpected semantic fields.");
}
} // namespace detail

// Wire regions contain only {subject_id,label,samples}; each canonical sorted
// sample is [native_face,confidence,dominance,pixel_support,view_support]. There
// are deliberately no worker fields for protection, material, edits or approval.
inline bool decode(const std::string& bytes,const ExpectedIdentity& expected,const VerifiedFaceBinding& binding,
    Evidence& destination,std::string& error)
{
    using namespace detail;
    error.clear();
    try {
        require(binding.valid() && expected.source_sha256==binding.source_sha256() && expected.geometry_id==binding.geometry_id() &&
            expected.face_count==binding.face_count(),"A host-verified ordered face binding is required.");
        require(identifier(expected.request_id) && expected.face_count>0 && expected.face_count<=max_faces &&
            AI::is_lowercase_sha256(expected.weights_sha256) && AI::is_lowercase_sha256(expected.runtime_sha256) &&
            AI::is_lowercase_sha256(expected.policy_sha256),"Invalid expected semantic identity.");
        require(std::isfinite(expected.minimum_confidence) && expected.minimum_confidence>=.5 && expected.minimum_confidence<=1 &&
            std::isfinite(expected.minimum_dominance) && expected.minimum_dominance>=.5 && expected.minimum_dominance<=1 &&
            expected.maximum_views>=1 && expected.maximum_views<=64,"Invalid host semantic policy.");
        for (const auto& label:expected.protected_labels) require(supported_label(label),"Unsupported protected semantic label.");
        require(!bytes.empty() && bytes.size()<=max_bytes,"Semantic payload exceeds byte limit.");
        std::vector<std::set<std::string>> object_keys;
        auto callback=[&](int depth,Json::parse_event_t event,Json& value) {
            require(depth<=12,"Semantic JSON nesting exceeds limit.");
            if (event==Json::parse_event_t::object_start) { object_keys.resize(size_t(depth)+1); object_keys[size_t(depth)].clear(); }
            if (event==Json::parse_event_t::key) {
                require(depth>0 && size_t(depth)<=object_keys.size(),"Invalid semantic JSON object.");
                require(object_keys[size_t(depth)-1].insert(value.get<std::string>()).second,"Duplicate semantic JSON key.");
            }
            return true;
        };
        const auto j=Json::parse(bytes,callback);
        std::set<std::string> fields={"schema","label_schema","request_id","source_sha256","geometry_id","render_geometry_id","face_count",
                "weights_sha256","runtime_sha256","policy_sha256","subjects","regions"};
        if(j.contains("eye_details"))fields.insert("eye_details");
        if(j.contains("feature_details"))fields.insert("feature_details");
        keys(j,fields);
        require(j.at("schema")==schema && j.at("label_schema")==label_schema,"Unsupported semantic schema.");
        require(j.at("request_id")==expected.request_id && j.at("source_sha256")==expected.source_sha256 &&
            j.at("geometry_id")==expected.geometry_id && j.at("render_geometry_id")==binding.render_geometry_id() &&
            index(j.at("face_count"),max_faces)==expected.face_count && j.at("weights_sha256")==expected.weights_sha256 &&
            j.at("runtime_sha256")==expected.runtime_sha256 && j.at("policy_sha256")==expected.policy_sha256,"Stale semantic evidence identity.");
        require(j.at("subjects").is_array() && j.at("subjects").size()<=max_subjects &&
            j.at("regions").is_array() && j.at("regions").size()<=max_regions,"Semantic collection exceeds limit.");
        Evidence value; value.identity=expected; value.render_geometry_id=binding.render_geometry_id();
        std::set<std::string> subjects;
        for (const auto& item:j.at("subjects")) {
            const auto subject=item.get<std::string>();
            require(identifier(subject) && subjects.insert(subject).second,"Invalid or duplicate semantic subject.");
            value.subjects.push_back(subject);
        }
        std::vector<AI::PrintColorRegion> candidates;
        std::vector<int32_t> owners(expected.face_count,-1);
        std::vector<double> confidence(expected.face_count,0);
        std::vector<uint8_t> accepted(expected.face_count,0);
        std::set<std::pair<std::string,std::string>> identities;
        size_t observations=0;
        for (const auto& record:j.at("regions")) {
            keys(record,{"subject_id","label","samples"});
            AI::PrintColorRegion region;
            region.subject_id=record.at("subject_id").get<std::string>(); region.label=record.at("label").get<std::string>();
            require(subjects.count(region.subject_id)!=0 && supported_label(region.label) &&
                identities.emplace(region.subject_id,region.label).second,"Invalid semantic subject or label.");
            const auto& samples=record.at("samples");
            require(samples.is_array() && samples.size()<=max_faces-observations,"Too many semantic observations.");
            observations+=samples.size();
            region.id="auto-semantic:"+region.subject_id+":"+region.label;
            region.protect_color=expected.protected_labels.count(region.label)!=0; region.confidence=1;
            const auto owner=int32_t(candidates.size()); size_t previous=0; bool first=true;
            for (const auto& sample:samples) {
                require(sample.is_array() && sample.size()==5,"Invalid semantic face sample.");
                const size_t face=index(sample.at(0),expected.face_count-1);
                require(binding.usable_face(face),"Coincident faces cannot supply semantic evidence.");
                require(first || face>previous,"Semantic face samples must be sorted and unique."); previous=face; first=false;
                const double score=probability(sample.at(1)),dominance=probability(sample.at(2));
                const auto views=index(sample.at(4),expected.maximum_views);
                const auto pixels=index(sample.at(3),views*4096*4096);
                require(pixels>0 && views>0 && views<=pixels,"Invalid semantic observation support.");
                if (owners[face]!=-1) { owners[face]=-2; accepted[face]=0; continue; }
                owners[face]=owner; confidence[face]=std::min(score,dominance);
                accepted[face]=score>=expected.minimum_confidence && dominance>=expected.minimum_dominance && (pixels>=2 || views>=2);
            }
            candidates.push_back(std::move(region));
        }
        for (size_t f=0;f<expected.face_count;++f) {
            if (owners[f]==-2) { ++value.ambiguous_faces; continue; }
            if (owners[f]<0) continue;
            if (!accepted[f]) { ++value.below_threshold_faces; continue; }
            auto& region=candidates[size_t(owners[f])]; region.faces.push_back(f); region.confidence=std::min(region.confidence,confidence[f]);
        }
        value.face_regions.assign(expected.face_count,-1);
        for (auto& region:candidates) {
            if (region.faces.empty()) continue;
            for (const auto f:region.faces) value.face_regions[f]=int32_t(value.regions.size());
            value.known_faces+=region.faces.size(); value.regions.push_back(std::move(region));
        }
        if(j.contains("eye_details")) {
            const auto& hints=j.at("eye_details");
            require(hints.is_array() && hints.size()<=2*max_subjects,"Too many eye hints.");
            std::set<std::pair<std::string,std::string>> seen;
            for(const auto& hint:hints) {
                keys(hint,{"subject_id","label","aperture_faces","iris_faces"});
                EyeDetail eye;eye.subject_id=hint.at("subject_id").get<std::string>();eye.label=hint.at("label").get<std::string>();
                require((eye.label=="re" || eye.label=="le") && seen.emplace(eye.subject_id,eye.label).second,"Invalid eye hint identity.");
                const auto region=std::find_if(value.regions.begin(),value.regions.end(),[&](const auto& r){return r.subject_id==eye.subject_id && r.label==eye.label;});
                require(region!=value.regions.end(),"Eye hint has no accepted eye region.");
                auto faces=[&](const Json& array,const std::vector<size_t>& allowed) {
                    require(array.is_array() && !array.empty() && array.size()<=allowed.size(),"Invalid eye hint size.");
                    std::vector<size_t> result;
                    for(const auto& item:array) {
                        const size_t f=index(item,expected.face_count-1);
                        require((result.empty() || f>result.back()) && std::binary_search(allowed.begin(),allowed.end(),f),"Eye hint must stay within its verified surface.");
                        result.push_back(f);
                    }
                    return result;
                };
                eye.aperture_faces=faces(hint.at("aperture_faces"),region->faces);
                eye.iris_faces=faces(hint.at("iris_faces"),eye.aperture_faces);
                value.eye_details.push_back(std::move(eye));
            }
        }
        if(j.contains("feature_details")) {
            const auto& hints=j.at("feature_details");
            require(hints.is_array() && hints.size()<=5*max_subjects,"Too many facial shape hints.");
            const std::set<std::string> parts={"re","le","ulip","llip","imouth"};
            const std::set<std::string> head={"face","nose","re","le","ulip","llip","imouth","rb","lb"};
            std::set<std::pair<std::string,std::string>> seen;
            std::vector<bool> occupied(expected.face_count,false);
            for(const auto& hint:hints) {
                if(hint.contains("anchor_paths"))keys(hint,{"subject_id","label","faces","iris_faces","view_support","anchor_paths"});
                else keys(hint,{"subject_id","label","faces","iris_faces","view_support"});
                FeatureDetail shape;
                shape.subject_id=hint.at("subject_id").get<std::string>();shape.label=hint.at("label").get<std::string>();
                shape.view_support=index(hint.at("view_support"),16);
                require(subjects.count(shape.subject_id) && parts.count(shape.label) && shape.view_support>=2 &&
                    seen.emplace(shape.subject_id,shape.label).second,"Invalid facial shape identity or support.");
                const auto& indices=hint.at("faces");
                require(indices.is_array() && !indices.empty() && indices.size()<=expected.face_count,"Invalid facial shape size.");
                bool anchored=false;
                for(const auto& item:indices) {
                    const size_t f=index(item,expected.face_count-1);
                    require(binding.usable_face(f) && !occupied[f] && (shape.faces.empty() || f>shape.faces.back()),"Facial shapes must use distinct verified faces.");
                    if(value.face_regions[f]>=0) {
                        const auto& region=value.regions[size_t(value.face_regions[f])];
                        require(region.subject_id==shape.subject_id && head.count(region.label),"Facial shape crosses another subject or nonfacial surface.");
                        anchored=true;
                    }
                    occupied[f]=true;shape.faces.push_back(f);
                }
                if(hint.contains("anchor_paths")) {
                    const auto& paths=hint.at("anchor_paths");
                    require(paths.is_array() && paths.size()>=2 && paths.size()<=4,"Invalid facial context paths.");
                    std::set<size_t> endpoints;
                    for(const auto& path:paths) {
                        require(path.is_array() && path.size()>=2 && path.size()<=9,"Invalid facial context path length.");
                        std::set<size_t> visited;size_t previous=SIZE_MAX;
                        for(const auto& item:path) {
                            const size_t f=index(item,expected.face_count-1);
                            require(binding.usable_face(f) && visited.insert(f).second,"Invalid facial context face.");
                            if(previous==SIZE_MAX)require(std::binary_search(shape.faces.begin(),shape.faces.end(),f),"Context must start inside its feature.");
                            else require(binding.adjacent_faces(previous,f),"Context must follow shared native surface edges.");
                            if(value.face_regions[f]>=0) {
                                const auto& region=value.regions[size_t(value.face_regions[f])];
                                require(region.subject_id==shape.subject_id && head.count(region.label),"Context crosses another subject or nonfacial surface.");
                            }
                            previous=f;
                        }
                        require(!std::binary_search(shape.faces.begin(),shape.faces.end(),previous) &&
                            value.face_regions[previous]>=0 && endpoints.insert(previous).second,"Context must end at distinct accepted facial anchors.");
                    }
                    anchored=true;
                }
                require(anchored,"Facial shape has no accepted subject anchor.");
                const auto& iris=hint.at("iris_faces");
                require(iris.is_array() && iris.size()<=shape.faces.size() &&
                    (shape.label=="re" || shape.label=="le" || iris.empty()),"Invalid facial iris size.");
                for(const auto& item:iris) {
                    const size_t f=index(item,expected.face_count-1);
                    require((shape.iris_faces.empty() || f>shape.iris_faces.back()) &&
                        std::binary_search(shape.faces.begin(),shape.faces.end(),f),"Iris leaves its facial shape.");
                    shape.iris_faces.push_back(f);
                }
                value.feature_details.push_back(std::move(shape));
            }
        }
        value.unknown_faces=expected.face_count-value.known_faces;
        destination=std::move(value); return true;
    } catch (const std::exception& exception) { error=exception.what(); return false; }
}
} // namespace Slic3r::GUI::LocalSemanticEvidence
