// Actual residual counts on native output, not assumptions from repair return.
J enhanced_topology_counts(Context& c,const std::vector<V>& vertices,const std::vector<T>& faces)
{
    std::map<V,uint32_t> points;std::vector<uint32_t> remap;remap.reserve(vertices.size());
    for(size_t i=0;i<vertices.size();++i){if((i&4095)==0)check_cancel(c);auto [it,inserted]=points.emplace(vertices[i],uint32_t(i));remap.push_back(it->second);}
    points.clear();
    std::vector<T> canonical;canonical.reserve(faces.size());
    for(size_t i=0;i<faces.size();++i){if((i&4095)==0)check_cancel(c);const auto& f=faces[i];canonical.push_back({remap.at(f[0]),remap.at(f[1]),remap.at(f[2])});}
    remap.clear();remap.shrink_to_fit();
    const auto counts=compact_topology_counts(c,vertices,canonical);
    const auto degenerate=counts[0],duplicate=counts[1],boundary=counts[2],nonmanifold=counts[3],inconsistent=counts[4];
    return {{"boundaryEdges",boundary},{"nonManifoldEdges",nonmanifold},{"inconsistentEdges",inconsistent},{"degenerateFaces",degenerate},{"duplicateFaces",duplicate},{"defects",boundary+nonmanifold+inconsistent+degenerate+duplicate}};
}

// Included only in the private GLB safety translation unit, with its path guards.
J enhanced_glb_carrier(Context& c, const Glb& g, Topology& input, const Topology& output,
                       const J& request, J proof, const F::path& source, const std::function<void(const char*)>& event)
{
    const auto started=std::chrono::steady_clock::now();
    auto elapsed=[&]{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();};
    J timings=J::object();event("appearanceMapping");
    const auto& primitives=g.doc.at("meshes")[0].at("primitives");
    require(primitives.size()==1,"Enhanced appearance currently requires one primitive");
    const auto& primitive=primitives[0];const auto& attributes=primitive.at("attributes");
    const auto position_count=g.item("accessors",attributes.at("POSITION").get<size_t>()).at("count").get<size_t>();
    auto uv=attributes.contains("TEXCOORD_0")?g.accessor(attributes.at("TEXCOORD_0"),"VEC2"):std::vector<double>{};
    auto normals=attributes.contains("NORMAL")?g.accessor(attributes.at("NORMAL"),"VEC3"):std::vector<double>{};
    std::vector<uint32_t> indices;
    if(primitive.contains("indices")) indices=g.accessor<uint32_t>(primitive.at("indices"),"SCALAR",true);
    else {indices.resize(position_count);std::iota(indices.begin(),indices.end(),0.);}
    event("carrierWriting");
    V low=output.vertices.front(),high=low;
    for(const auto& v:output.vertices)for(size_t k=0;k<3;++k){low[k]=std::min(low[k],v[k]);high[k]=std::max(high[k],v[k]);}
    double diagonal=0;for(size_t k=0;k<3;++k)diagonal+=(high[k]-low[k])*(high[k]-low[k]);diagonal=std::sqrt(diagonal);
    double maximum_projection=0;require(diagonal>0,"Invalid output extent");
    J doc=g.doc;
    std::string bin;
    size_t exported_vertex_count=0;
    std::string position_bytes,uv_bytes,normal_bytes,index_bytes;
    std::map<std::array<uint32_t,8>,uint32_t> split;
    const auto export_capacity=std::min<size_t>(glb_vertices,output.faces.size()*3);
    uint64_t mapping_workspace=0,carrier_estimated_peak=0;
    const uint64_t strings_bound=uint64_t(export_capacity)*(12+(uv.empty()?0:8)+(normals.empty()?0:12))+output.faces.size()*12;
    const uint64_t fixed_workspace=g.source.size()+uint64_t(g.bin)*16+128ull*1024*1024
        +(input.vertices.capacity()+output.vertices.capacity())*sizeof(V)
        +(input.faces.capacity()+output.faces.capacity())*sizeof(T)
        +(uv.capacity()+normals.capacity())*sizeof(double)+indices.capacity()*sizeof(uint32_t)+strings_bound;
    auto admit=[&](uint64_t prospective_nodes) {
        const uint64_t used=fixed_workspace+mapping_workspace+prospective_nodes*96;
        carrier_estimated_peak=std::max(carrier_estimated_peak,used);
        if(used>768ull*1024*1024)throw Error("RESOURCE_LIMIT","Carrier live allocation plan exceeds 768 MiB policy");
    };
    admit(0);
    position_bytes.reserve(export_capacity*12);if(!uv.empty())uv_bytes.reserve(export_capacity*8);
    if(!normals.empty())normal_bytes.reserve(export_capacity*12);index_bytes.reserve(output.faces.size()*12);
    auto append_float=[](std::string& bytes,float f){uint32_t bits;std::memcpy(&bits,&f,4);put32(bytes,bits);};
    enhanced_corner_sources(c,input.vertices,input.faces,output.vertices,output.faces,[&](size_t face,const std::array<double,11>& mapping) {
        if((face&1023)==0)check_cancel(c);
        maximum_projection=std::max(maximum_projection,mapping[10]);
        require(maximum_projection<=diagonal*.002,"Appearance source projection exceeds bounded displacement");
        const auto source_face=size_t(mapping[0]);T result_face;
        for(size_t corner=0;corner<3;++corner) {
            const auto& v=output.vertices[output.faces[face][corner]];
            float u=0,w=0;V normal{};
            for(size_t k=0;k<3;++k) {
                const auto id=size_t(indices.at(source_face*3+k));const auto weight=mapping[1+corner*3+k];
                if(!uv.empty()) {u+=float(uv.at(id*2)*weight);w+=float(uv.at(id*2+1)*weight);}
                if(!normals.empty()) {normal[0]+=normals.at(id*3)*weight;normal[1]-=normals.at(id*3+2)*weight;normal[2]+=normals.at(id*3+1)*weight;}
            }
            if(!normals.empty()) {
                const auto& a=output.vertices[output.faces[face][0]];const auto& b=output.vertices[output.faces[face][1]];const auto& d=output.vertices[output.faces[face][2]];
                V edge{b[0]-a[0],b[1]-a[1],b[2]-a[2]},other{d[0]-a[0],d[1]-a[1],d[2]-a[2]};
                V cross{edge[1]*other[2]-edge[2]*other[1],edge[2]*other[0]-edge[0]*other[2],edge[0]*other[1]-edge[1]*other[0]};
                double length=0,dot=0;for(size_t k=0;k<3;++k){length+=normal[k]*normal[k];dot+=normal[k]*cross[k];}
                require(std::isfinite(length)&&length>1e-20,"Cannot reconstruct normal basis");
                const double sign=dot<0?-1.:1.;for(auto& n:normal)n=sign*n/std::sqrt(length);
            }
            std::array<float,8> values{float(v[0]),float(v[1]),float(v[2]),u,w,float(normal[0]),float(normal[1]),float(normal[2])};
            std::array<uint32_t,8> key;std::memcpy(key.data(),values.data(),sizeof(key));
            admit(exported_vertex_count+1);
            auto [it,inserted]=split.emplace(key,uint32_t(exported_vertex_count));
            if(inserted) {
                require(exported_vertex_count<glb_vertices,"Appearance seam splitting exceeds vertex budget");
                ++exported_vertex_count;
                for(size_t j=0;j<3;++j)append_float(position_bytes,values[j]);
                if(!uv.empty()){append_float(uv_bytes,u);append_float(uv_bytes,w);}
                if(!normals.empty())for(size_t j=5;j<8;++j)append_float(normal_bytes,values[j]);
            }
            result_face[corner]=it->second;put32(index_bytes,it->second);
        }

    },[&](uint64_t workspace){mapping_workspace=workspace;admit(exported_vertex_count);});
    proof["workspaceAdmission"]["carrierPeakPlanBytes"]=carrier_estimated_peak;
    timings["mappingAndWritingMs"]=elapsed();split.clear();indices.clear();indices.shrink_to_fit();
    // The original buffer remains byte-for-byte on disk. If carrying inactive
    // source geometry would exceed the candidate cap, keep all encoded images
    // with exact material/texture/sampler semantics and rebase only their views.
    const auto source_bin_size=g.doc.at("buffers")[0].at("byteLength").get<size_t>();
    const uint64_t preferred_display_bytes=64ull*1024*1024;
    const bool compact_bin=source_bin_size+position_bytes.size()+uv_bytes.size()+normal_bytes.size()+index_bytes.size()+g.bin+4096>preferred_display_bytes;
    size_t active_image_bytes=0;
    for(const auto& image:g.doc.value("images",J::array()))active_image_bytes+=g.view(image.at("bufferView").get<size_t>()).second+3;
    const auto bin_bound=(compact_bin?active_image_bytes:source_bin_size)+position_bytes.size()+uv_bytes.size()+normal_bytes.size()+index_bytes.size()+16;
    if(bin_bound>max_bytes)throw Error("RESOURCE_LIMIT","Active candidate payload exceeds 100 MiB policy");
    bin.reserve(bin_bound);
    if(compact_bin) {
        doc["bufferViews"]=J::array();doc["accessors"]=J::array();
        for(size_t i=0;i<doc.value("images",J::array()).size();++i) {
            const auto view=g.view(g.doc["images"][i]["bufferView"].get<size_t>());
            while(bin.size()%4)bin.push_back(0);
            const auto id=doc["bufferViews"].size();
            doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",bin.size()},{"byteLength",view.second}});
            bin.append(g.source,view.first,view.second);doc["images"][i]["bufferView"]=id;
        }
    } else bin=g.source.substr(g.bin,source_bin_size);
    const auto source_face_count=input.faces.size();
    const bool has_uv=!uv.empty(),has_normal=!normals.empty();
    uv.clear();uv.shrink_to_fit();normals.clear();normals.shrink_to_fit();
    input.vertices.clear();input.vertices.shrink_to_fit();input.faces.clear();input.faces.shrink_to_fit();
    auto accessor=[&](const std::string& data,const char* type,size_t count,unsigned component) {
        while(bin.size()%4)bin.push_back(0);
        const size_t offset=bin.size(),view_id=doc["bufferViews"].size(),id=doc["accessors"].size();bin+=data;
        doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",data.size()}});
        doc["accessors"].push_back({{"bufferView",view_id},{"componentType",component},{"count",count},{"type",type}});
        return id;
    };
    // Bounds belong to the accessor's raw Core-float frame, not source metres
    // or the canonical display root. Read the bytes actually being serialized.
    V position_min{},position_max{};
    require(exported_vertex_count>0 && position_bytes.size()==exported_vertex_count*12,"Invalid serialized POSITION length");
    for(size_t i=0;i<exported_vertex_count;++i) {
        if((i&4095)==0)check_cancel(c);
        for(size_t k=0;k<3;++k) {
            const uint32_t bits=u32(position_bytes,i*12+k*4);float value;std::memcpy(&value,&bits,4);
            require(std::isfinite(value),"Non-finite serialized POSITION");
            if(i==0)position_min[k]=position_max[k]=value;
            else {position_min[k]=std::min(position_min[k],double(value));position_max[k]=std::max(position_max[k],double(value));}
        }
    }
    const auto position_accessor=accessor(position_bytes,"VEC3",exported_vertex_count,5126);
    doc["accessors"][position_accessor]["min"]=position_min;
    doc["accessors"][position_accessor]["max"]=position_max;
    J attrs={{"POSITION",position_accessor}};
    if(has_uv)attrs["TEXCOORD_0"]=accessor(uv_bytes,"VEC2",exported_vertex_count,5126);
    if(has_normal)attrs["NORMAL"]=accessor(normal_bytes,"VEC3",exported_vertex_count,5126);
    position_bytes.clear();position_bytes.shrink_to_fit();uv_bytes.clear();uv_bytes.shrink_to_fit();normal_bytes.clear();normal_bytes.shrink_to_fit();
    auto derived=primitive;derived["attributes"]=attrs;derived["indices"]=accessor(index_bytes,"SCALAR",output.faces.size()*3,5125);
    doc["meshes"]=J::array({{{"primitives",J::array({derived})}}});
    const J matrix=J::array({.001,0.,0.,0.,0.,0.,-.001,0.,0.,.001,0.,0.,0.,0.,0.,1.});
    doc["nodes"]=J::array({{{"mesh",0},{"matrix",matrix}}});doc["scenes"]=J::array({{{"nodes",J::array({0})}}});doc["scene"]=0;
    doc["buffers"][0]["byteLength"]=bin.size();while(bin.size()%4)bin.push_back(0);
    index_bytes.clear();index_bytes.shrink_to_fit();
    auto json=doc.dump();while(json.size()%4)json+=' ';
    require(json.size()<=8388608,"Candidate JSON exceeds budget");
    std::string bytes;bytes.reserve(28+json.size()+bin.size());put32(bytes,0x46546c67);put32(bytes,2);put32(bytes,uint32_t(28+json.size()+bin.size()));
    put32(bytes,uint32_t(json.size()));put32(bytes,0x4e4f534a);bytes+=json;put32(bytes,uint32_t(bin.size()));put32(bytes,0x004e4942);bytes+=bin;bin.clear();bin.shrink_to_fit();
    require(bytes.size()<=max_bytes,"Candidate exceeds byte budget");
    timings["carrierReadyMs"]=elapsed();event("carrierReadback");
    const Glb readback(bytes);
    require(readback.doc.at("nodes")[0].at("matrix")==matrix,"Carrier coordinate frame mismatch");
    auto rp=readback.accessor(attrs.at("POSITION"),"VEC3");auto ri=readback.accessor<uint32_t>(derived.at("indices"),"SCALAR",true);
    std::vector<V> read_vertices;std::vector<T> read_faces;
    V actual_min{rp.at(0),rp.at(1),rp.at(2)},actual_max=actual_min;
    for(size_t i=0;i<rp.size();i+=3) {
        read_vertices.push_back({rp[i],rp[i+1],rp[i+2]});
        for(size_t k=0;k<3;++k){actual_min[k]=std::min(actual_min[k],rp[i+k]);actual_max[k]=std::max(actual_max[k],rp[i+k]);}
    }
    const auto& serialized_position=readback.item("accessors",position_accessor);
    require(serialized_position.at("min")==J(actual_min) && serialized_position.at("max")==J(actual_max),"Serialized POSITION bounds differ from actual readback");
    for(size_t i=0;i<ri.size();i+=3)read_faces.push_back({uint32_t(ri[i]),uint32_t(ri[i+1]),uint32_t(ri[i+2])});
    rp.clear();rp.shrink_to_fit();ri.clear();ri.shrink_to_fit();
    proof["readbackGeometryId"]=enhanced_core_id(read_vertices,read_faces);
    require(proof.at("readbackGeometryId")==proof.at("outputGeometryId"),"Serialized geometry differs from native output");
    require(!proof.at("readbackGeometryId").get<std::string>().empty(),"Invalid serialized geometry identity");
    proof["readbackVerified"]=true;
    proof["geometryValid"]=true;
    proof["canceled"]=false;proof["resourceFailure"]=false;
    proof["appearancePolicyVersion"]="appearance-reconstruction-v1";
    for(const char* field:{"materials","textures","samplers"})
        require(readback.doc.value(field,J::array())==g.doc.value(field,J::array()),"Appearance channel semantics changed");
    auto source_images=g.doc.value("images",J::array()),candidate_images=readback.doc.value("images",J::array());
    require(source_images.size()==candidate_images.size(),"Image count changed");
    for(size_t i=0;i<source_images.size();++i) {
        // View identity may be rebased, image semantics and encoded bytes may not.
        const auto original=g.view(source_images[i]["bufferView"].get<size_t>());
        const auto derived_view=readback.view(candidate_images[i]["bufferView"].get<size_t>());
        require(original.second==derived_view.second && g.source.compare(original.first,original.second,readback.source,derived_view.first,derived_view.second)==0,"Active encoded image bytes changed");
        candidate_images[i]["bufferView"]=source_images[i]["bufferView"];
    }
    require(source_images==candidate_images,"Image semantics changed");
    if(!compact_bin)require(readback.source.compare(readback.bin,source_bin_size,g.source,g.bin,source_bin_size)==0,"Source encoded appearance payload changed");
    proof["carrierLayoutSelectionBytes"]=preferred_display_bytes;
    proof["appearanceCarrierLayout"]=compact_bin?"active-encoded-images":"original-bin-prefix";

    timings["readbackReadyMs"]=elapsed();event("appearanceVerification");
    J checks{{"structure",check(true)},
             {"geometry",{{"performed",false},{"status","not_run"},{"count",nullptr},{"details","Full bidirectional geometry deviation and area policy not evaluated by enhanced-only entry"}}},
             {"topology",{{"performed",false},{"status","not_run"},{"count",nullptr}}},
             {"selfIntersection",{{"performed",false},{"status","not_run"},{"count",nullptr}}},
             {"appearance",check(true,0,"Source encoded channels and material semantics preserved; bounded per-corner UV/normal reconstruction re-read")}};
    J channel_evidence=J::array();
    for(size_t i=0;i<g.doc.value("images",J::array()).size();++i) {
        const auto a=g.view(g.doc["images"][i]["bufferView"]),b=readback.view(readback.doc["images"][i]["bufferView"]);
        const auto original=sha256(g.source.substr(a.first,a.second)),derived=sha256(readback.source.substr(b.first,b.second));
        require(original==derived,"Encoded image identity differs");
        channel_evidence.push_back({{"imageIndex",i},{"mimeType",g.doc["images"][i]["mimeType"]},{"sourceSha256",original},{"candidateSha256",derived}});
    }
    if(has_uv)require(readback.accessor(attrs.at("TEXCOORD_0"),"VEC2").size()==exported_vertex_count*2,"Invalid serialized UV channel");
    if(has_normal)require(readback.accessor(attrs.at("NORMAL"),"VEC3").size()==exported_vertex_count*3,"Invalid serialized normal channel");
#ifdef ORCA_MODEL_HAS_GLB_CGAL
    const auto residual=proof.at("residualChecks");
    const auto counts=proof.at("topologyCounts");proof.erase("topologyCounts");
    proof.erase("residualChecks");
    checks["topology"]=check(residual.at("topologyValid").get<bool>()&&counts.at("defects")==0,counts.at("defects").get<size_t>()+(residual.at("topologyValid").get<bool>()?0:1),"Actual edge/face counts plus CGAL soup/vertex link; invalid vertex link adds one risk flag");
    checks["topology"]["details"]={{"edgeFaceCounts",counts},{"CGAL",residual}};
    if(residual.at("selfIntersectionPerformed").get<bool>()) {
        const bool risk=residual.at("selfIntersects").get<bool>();
        checks["selfIntersection"]=check(!risk,0);
        if(risk)checks["selfIntersection"]["count"]=nullptr;
        checks["selfIntersection"]["details"]={{"predicate","CGAL does_self_intersect"},{"riskFlag",risk},{"intersectionPairsEnumerated",false}};
    }
    if(!residual.at("topologyValid").get<bool>()) {
        if(residual.contains("nonManifoldVertexFans"))checks["topology"]["count"]=counts.at("defects").get<size_t>()+residual.at("nonManifoldVertexFans").get<size_t>();
        else checks["topology"]["count"]=nullptr;
    }
#endif
    const auto dir=private_output(c,F::u8path(request.at("outputDirectory").get<std::string>())/"verified-enhanced");
    const auto basename=sha256(request.at("identity").dump()+"glb-repair-v2");
    const auto path=private_output(c,dir/(basename+".glb"));
    const auto receipt_path=private_output(c,dir/(basename+".json"));
    J result{{"policyVersion","glb-repair-v2"},{"completion","native_compatible"},{"identity",request.at("identity")},
             {"source",{{"path",source.u8string()},{"sha256",sha256(g.source)},{"bytes",g.source.size()},{"format","glb"},{"mediaType","model/gltf-binary"}}},
             {"candidate",{{"path",path.u8string()},{"sha256",sha256(bytes)},{"bytes",bytes.size()},{"format","glb"},{"mediaType","model/gltf-binary"}}},
             {"checks",checks},{"nativeProof",proof},{"repairApplied",proof.at("modified")},
             {"geometryVerified",false},{"appearanceVerified",true},{"manufacturingSafe",false},{"vertices",exported_vertex_count},{"triangles",output.faces.size()},
             {"appearanceEvidence",{{"policyVersion","appearance-reconstruction-v1"},{"encodedImages",channel_evidence},{"maximumCornerProjectionMillimetres",maximum_projection},{"maximumCornerProjectionRatio",maximum_projection/diagonal},{"uvMapping","exact-face-first-or-Core-AABB-barycentric"},{"mappedCorners",output.faces.size()*3},{"outputFaces",output.faces.size()},{"sourcePrimitive",0},{"sourceFaces",source_face_count},{"normalPolicy",has_normal?"source-projected-normalized-and-winding-aligned":"GLTF-generated normals"},{"tangentPolicy","GLTF-renderer-derived-from-repaired-UV-and-normal"},{"coordinateFrame","raw-Core-float-mm-Z-up-with-canonical-GLTF-root"}}}};
    // Stable receipt identity excludes runtime durations. Actual current durations
    // remain in the response; replay re-executes and proves source -> output.
    auto durable=result;durable["nativeProof"].erase("wallMs");durable["nativeProof"].erase("cpuMs");
    const auto receipt=durable.dump();require(receipt.size()<=65536,"Receipt exceeds byte budget");
    timings["verificationReadyMs"]=elapsed();
    bool created=false,receipt_created=false;
    try {
        event("publishing");created=publish_exclusive(path,bytes,source);event("candidatePublished");
        require(sha256(read_private(path))==sha256(bytes),"Candidate disk readback mismatch");
        receipt_created=publish_exclusive(receipt_path,receipt,source);check_cancel(c);
        require(read_private(receipt_path)==receipt,"Receipt disk readback mismatch");event("receiptPublished");
    } catch(...) {
        std::error_code ignored;
        if(receipt_created)F::remove(io_path(receipt_path),ignored);
        if(created)F::remove(io_path(path),ignored);
        throw;
    }
    timings["publishedMs"]=elapsed();result["carrierTimingsSinceStartMs"]=timings;
    result["receipt"]={{"path",receipt_path.u8string()},{"sha256",sha256(receipt)},{"bytes",receipt.size()}};
    return result;
}
