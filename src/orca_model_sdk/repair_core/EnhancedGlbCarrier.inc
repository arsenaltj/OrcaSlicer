// Actual residual counts on native output, not assumptions from repair return.
J enhanced_topology_counts(Context& c,const std::vector<V>& vertices,const std::vector<T>& faces)
{
    std::map<V,uint32_t> points;std::vector<uint32_t> remap;remap.reserve(vertices.size());
    for(const auto& v:vertices){auto [it,inserted]=points.emplace(v,uint32_t(points.size()));remap.push_back(it->second);}
    std::set<std::array<uint32_t,3>> unique;
    std::map<std::pair<uint32_t,uint32_t>,std::array<size_t,2>> edges;
    size_t degenerate=0,duplicate=0,boundary=0,nonmanifold=0,inconsistent=0;
    for(size_t i=0;i<faces.size();++i) {
        if((i&4095)==0)check_cancel(c);const auto& f=faces[i];
        const auto& a=vertices.at(f[0]);const auto& b=vertices.at(f[1]);const auto& d=vertices.at(f[2]);
        V u{b[0]-a[0],b[1]-a[1],b[2]-a[2]},v{d[0]-a[0],d[1]-a[1],d[2]-a[2]};
        if(V{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]}==V{})++degenerate;
        T canonical{remap.at(f[0]),remap.at(f[1]),remap.at(f[2])},key=canonical;std::sort(key.begin(),key.end());
        if(!unique.insert(key).second)++duplicate;
        for(size_t k=0;k<3;++k){auto x=canonical[k],y=canonical[(k+1)%3];++edges[{std::min(x,y),std::max(x,y)}][x<y?0:1];}
    }
    for(const auto& entry:edges){const auto& use=entry.second;if(use[0]+use[1]==1)++boundary;if(use[0]+use[1]>2)++nonmanifold;if(use[0]+use[1]==2&&(use[0]==2||use[1]==2))++inconsistent;}
    return {{"boundaryEdges",boundary},{"nonManifoldEdges",nonmanifold},{"inconsistentEdges",inconsistent},{"degenerateFaces",degenerate},{"duplicateFaces",duplicate},{"defects",boundary+nonmanifold+inconsistent+degenerate+duplicate}};
}

// Included only in the private GLB safety translation unit, with its path guards.
J enhanced_glb_carrier(Context& c, const Glb& g, const Topology& input, const Topology& output,
                       const J& request, J proof, const F::path& source, const std::function<void(const char*)>& event)
{
    const auto started=std::chrono::steady_clock::now();
    auto elapsed=[&]{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();};
    J timings=J::object();event("appearanceMapping");
    const auto& primitives=g.doc.at("meshes")[0].at("primitives");
    require(primitives.size()==1,"Enhanced appearance currently requires one primitive");
    const auto& primitive=primitives[0];const auto& attributes=primitive.at("attributes");
    const auto positions=g.accessor(attributes.at("POSITION"),"VEC3");
    auto uv=attributes.contains("TEXCOORD_0")?g.accessor(attributes.at("TEXCOORD_0"),"VEC2"):std::vector<double>{};
    auto normals=attributes.contains("NORMAL")?g.accessor(attributes.at("NORMAL"),"VEC3"):std::vector<double>{};
    std::vector<double> indices;
    if(primitive.contains("indices")) indices=g.accessor(primitive.at("indices"),"SCALAR",true);
    else {indices.resize(positions.size()/3);std::iota(indices.begin(),indices.end(),0.);}
    auto mappings=enhanced_corner_sources(c,input.vertices,input.faces,output.vertices,output.faces);
    timings["mappingMs"]=elapsed();event("carrierWriting");
    V low=output.vertices.front(),high=low;
    for(const auto& v:output.vertices)for(size_t k=0;k<3;++k){low[k]=std::min(low[k],v[k]);high[k]=std::max(high[k],v[k]);}
    double diagonal=0;for(size_t k=0;k<3;++k)diagonal+=(high[k]-low[k])*(high[k]-low[k]);diagonal=std::sqrt(diagonal);
    double maximum_projection=0;for(const auto& m:mappings)maximum_projection=std::max(maximum_projection,m[10]);
    require(diagonal>0&&maximum_projection<=diagonal*.002,"Appearance source projection exceeds bounded displacement");
    J doc=g.doc;
    std::string bin=g.source.substr(g.bin,g.doc.at("buffers")[0].at("byteLength").get<size_t>());
    size_t exported_vertex_count=0;
    std::string position_bytes,uv_bytes,normal_bytes,index_bytes;
    std::map<std::array<uint32_t,8>,uint32_t> split;
    auto append_float=[](std::string& bytes,float f){uint32_t bits;std::memcpy(&bits,&f,4);put32(bytes,bits);};
    for(size_t face=0;face<output.faces.size();++face) {
        if((face&1023)==0)check_cancel(c);
        const auto& mapping=mappings.at(face);const auto source_face=size_t(mapping[0]);T result_face;
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

    }
    mappings.clear();mappings.shrink_to_fit();split.clear();
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
    if(!uv.empty())attrs["TEXCOORD_0"]=accessor(uv_bytes,"VEC2",exported_vertex_count,5126);
    if(!normals.empty())attrs["NORMAL"]=accessor(normal_bytes,"VEC3",exported_vertex_count,5126);
    position_bytes.clear();position_bytes.shrink_to_fit();uv_bytes.clear();uv_bytes.shrink_to_fit();normal_bytes.clear();normal_bytes.shrink_to_fit();
    auto derived=primitive;derived["attributes"]=attrs;derived["indices"]=accessor(index_bytes,"SCALAR",output.faces.size()*3,5125);
    doc["meshes"]=J::array({{{"primitives",J::array({derived})}}});
    const J matrix=J::array({.001,0.,0.,0.,0.,0.,-.001,0.,0.,.001,0.,0.,0.,0.,0.,1.});
    doc["nodes"]=J::array({{{"mesh",0},{"matrix",matrix}}});doc["scenes"]=J::array({{{"nodes",J::array({0})}}});doc["scene"]=0;
    doc["buffers"][0]["byteLength"]=bin.size();while(bin.size()%4)bin.push_back(0);
    index_bytes.clear();index_bytes.shrink_to_fit();
    auto json=doc.dump();while(json.size()%4)json+=' ';
    require(json.size()<=8388608,"Candidate JSON exceeds budget");
    std::string bytes;put32(bytes,0x46546c67);put32(bytes,2);put32(bytes,uint32_t(28+json.size()+bin.size()));
    put32(bytes,uint32_t(json.size()));put32(bytes,0x4e4f534a);bytes+=json;put32(bytes,uint32_t(bin.size()));put32(bytes,0x004e4942);bytes+=bin;bin.clear();bin.shrink_to_fit();
    require(bytes.size()<=max_bytes,"Candidate exceeds byte budget");
    timings["carrierReadyMs"]=elapsed();event("carrierReadback");
    const Glb readback(bytes);
    require(readback.doc.at("nodes")[0].at("matrix")==matrix,"Carrier coordinate frame mismatch");
    auto rp=readback.accessor(attrs.at("POSITION"),"VEC3");auto ri=readback.accessor(derived.at("indices"),"SCALAR",true);
    std::vector<V> read_vertices;std::vector<T> read_faces;
    V actual_min{rp.at(0),rp.at(1),rp.at(2)},actual_max=actual_min;
    for(size_t i=0;i<rp.size();i+=3) {
        read_vertices.push_back({rp[i],rp[i+1],rp[i+2]});
        for(size_t k=0;k<3;++k){actual_min[k]=std::min(actual_min[k],rp[i+k]);actual_max[k]=std::max(actual_max[k],rp[i+k]);}
    }
    const auto& serialized_position=readback.item("accessors",position_accessor);
    require(serialized_position.at("min")==J(actual_min) && serialized_position.at("max")==J(actual_max),"Serialized POSITION bounds differ from actual readback");
    for(size_t i=0;i<ri.size();i+=3)read_faces.push_back({uint32_t(ri[i]),uint32_t(ri[i+1]),uint32_t(ri[i+2])});
    proof["readbackGeometryId"]=enhanced_core_id(read_vertices,read_faces);
    require(proof.at("readbackGeometryId")==proof.at("outputGeometryId"),"Serialized geometry differs from native output");
    require(!proof.at("readbackGeometryId").get<std::string>().empty(),"Invalid serialized geometry identity");
    proof["readbackVerified"]=true;
    proof["geometryValid"]=true;
    proof["canceled"]=false;proof["resourceFailure"]=false;
    proof["appearancePolicyVersion"]="appearance-reconstruction-v1";
    for(const char* field:{"materials","textures","images","samplers"})
        require(readback.doc.value(field,J::array())==g.doc.value(field,J::array()),"Appearance channel semantics changed");
    require(readback.source.compare(readback.bin,g.doc.at("buffers")[0].at("byteLength").get<size_t>(),g.source,g.bin,g.doc.at("buffers")[0].at("byteLength").get<size_t>())==0,"Source encoded appearance payload changed");

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
    if(!uv.empty())require(readback.accessor(attrs.at("TEXCOORD_0"),"VEC2").size()==exported_vertex_count*2,"Invalid serialized UV channel");
    if(!normals.empty())require(readback.accessor(attrs.at("NORMAL"),"VEC3").size()==exported_vertex_count*3,"Invalid serialized normal channel");
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
             {"appearanceEvidence",{{"policyVersion","appearance-reconstruction-v1"},{"encodedImages",channel_evidence},{"maximumCornerProjectionMillimetres",maximum_projection},{"maximumCornerProjectionRatio",maximum_projection/diagonal},{"uvMapping","exact-face-first-or-Core-AABB-barycentric"},{"mappedCorners",output.faces.size()*3},{"outputFaces",output.faces.size()},{"sourcePrimitive",0},{"sourceFaces",input.faces.size()},{"normalPolicy",normals.empty()?"GLTF-generated normals":"source-projected-normalized-and-winding-aligned"},{"tangentPolicy","GLTF-renderer-derived-from-repaired-UV-and-normal"},{"coordinateFrame","raw-Core-float-mm-Z-up-with-canonical-GLTF-root"}}}};
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
