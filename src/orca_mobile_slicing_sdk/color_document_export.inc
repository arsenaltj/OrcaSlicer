// Included inside engine.cpp's private namespace. This reads the replayed Model,
// not the lossy SC01 renderer carrier. World transforms are baked in double.
Json export_color_document(LoadedProject& project,const Json& request,const std::string& workspace,Budget& budget)
{
    if(!request.contains("projectId")||!request.contains("projectRevision"))throw std::runtime_error("COLOR_SOURCE_IDENTITY_REQUIRED");
    validate_output_capabilities(project);
    const auto* configured=project.config.option<Slic3r::ConfigOptionStrings>("filament_colour");
    if(!configured||configured->values.empty())throw std::runtime_error("COLOR_PALETTE_REQUIRED");
    Json parts=Json::array(),faceColors=Json::array(),materialIds=Json::array();
    std::ostringstream vertices,faces;vertices<<std::setprecision(17);size_t vertexCount=0,faceCount=0;
    for(size_t oi=0;oi<project.model.objects.size();++oi){
        const auto* object=project.model.objects[oi];
        if(!object->layer_config_ranges.empty())throw std::runtime_error("COLOR_EXPORT_LAYER_RANGES_UNSUPPORTED");
        for(const auto& key:object->config.keys())if(key!="extruder")throw std::runtime_error("COLOR_EXPORT_OBJECT_OPTIONS_UNSUPPORTED:"+key);
        const size_t count=std::max<size_t>(1,object->instances.size());
        for(size_t ii=0;ii<count;++ii)for(size_t vi=0;vi<object->volumes.size();++vi){
            budget.check();const auto* volume=object->volumes[vi];
            if(!volume->is_model_part()||volume->is_fdm_support_painted()||volume->is_seam_painted()||volume->is_fuzzy_skin_painted())
                throw std::runtime_error("COLOR_EXPORT_ANNOTATION_UNSUPPORTED");
            for(const auto& key:volume->config.keys())if(key!="extruder")throw std::runtime_error("COLOR_EXPORT_VOLUME_OPTIONS_UNSUPPORTED:"+key);
            const auto matrix=(object->instances.empty()?Slic3r::Transform3d::Identity():object->instances[ii]->get_matrix())*volume->get_matrix();
            const bool reverse=matrix.linear().determinant()<0;
            const auto firstVertex=vertexCount,firstFace=faceCount;
            auto append=[&](const indexed_triangle_set& mesh,size_t material){
                if(material>=configured->values.size())throw std::runtime_error("COLOR_EXPORT_MATERIAL_OUT_OF_RANGE");
                const auto hex=configured->values[material];
                if(hex.size()!=7||hex[0]!='#'||hex.find_first_not_of("0123456789abcdefABCDEF",1)!=std::string::npos)throw std::runtime_error("COLOR_EXPORT_INVALID_COLOR");
                Json rgba=Json::array();for(size_t c=0;c<3;++c)rgba.push_back(double(std::stoul(hex.substr(1+2*c,2),nullptr,16))/255.);rgba.push_back(1.);
                const auto offset=vertexCount;
                for(const auto& v:mesh.vertices){budget.check();const Slic3r::Vec3d point=matrix*v.cast<double>();if(!point.allFinite())throw std::runtime_error("COLOR_EXPORT_NONFINITE");vertices<<"v "<<point.x()<<' '<<point.y()<<' '<<point.z()<<'\n';++vertexCount;}
                for(const auto& t:mesh.indices){budget.check();faces<<"f "<<offset+size_t(t[0])+1<<' '<<offset+size_t(t[reverse?2:1])+1<<' '<<offset+size_t(t[reverse?1:2])+1<<'\n';faceColors.push_back(rgba);materialIds.push_back(material);++faceCount;}
                if(vertexCount>2500000||faceCount>2000000)throw std::runtime_error("COLOR_EXPORT_RESOURCE_LIMIT");
            };
            const size_t fallback=size_t(std::max(1,volume->extruder_id())-1);
            if(volume->mmu_segmentation_facets.empty())append(volume->mesh().its,fallback);
            else{std::vector<indexed_triangle_set> groups;volume->mmu_segmentation_facets.get_facets(*volume,groups);for(size_t state=0;state<groups.size();++state)if(!groups[state].indices.empty())append(groups[state],state?state-1:fallback);}
            parts.push_back(Json{{"entityId","object-"+std::to_string(oi)+"-instance-"+std::to_string(ii)+"-volume-"+std::to_string(vi)},
                {"objectIndex",oi},{"instanceIndex",ii},{"volumeIndex",vi},{"name",volume->name},
                {"firstTriangle",firstFace},{"triangleCount",faceCount-firstFace},{"firstVertex",firstVertex},{"vertexCount",vertexCount-firstVertex}});
        }
    }
    if(!faceCount)throw std::runtime_error("COLOR_EXPORT_EMPTY");
    const auto bytes=vertices.str()+faces.str();if(bytes.size()>100u*1024u*1024u)throw std::runtime_error("COLOR_EXPORT_RESOURCE_LIMIT");
    auto prefix=root(request,workspace)/(artifact_key(request,project)+"-color");auto mesh=prefix;mesh+=".obj";auto partial=mesh;partial+=".partial";
    {std::ofstream out(partial,std::ios::binary|std::ios::trunc);out<<bytes;out.flush();if(!out)throw std::runtime_error("COLOR_EXPORT_WRITE_FAILED");}
    budget.check();commit_file(partial,mesh);
    const auto hash=file_sha256(mesh.u8string());const auto config=config_json(project.config);
    Json binding{{"projectId",request.at("projectId")},{"projectRevision",request.at("projectRevision")},{"inputSha256",project.inputSha256},
        {"config",config},{"configSha256",json_sha256(config)},{"selectedPlateId",project.metadata.value("selectedPlateId",Json())},
        {"scope","native-model-world-baked-parts"},{"requiresSourceProjectRebind",true}};
    Json appearance{{"schemaVersion",1},{"units","mm"},{"upAxis","Z"},{"geometrySha256",hash},
        {"faceColors",faceColors},{"materialIds",materialIds},{"parts",parts},{"sourceProjectBinding",binding}};
    auto sidecar=prefix;sidecar+=".appearance.json";budget.check();write_json(sidecar,appearance);
    return Json{{"schemaVersion",2},{"state","succeeded"},{"projectId",request.at("projectId")},{"projectRevision",request.at("projectRevision")},
        {"sourceOriginalPreserved",true},{"units","mm"},{"upAxis","Z"},{"sourceProjectBinding",binding},
        {"artifacts",Json::array({{{"kind","manufacturingMesh"},{"format","obj"},{"path",mesh.u8string()},{"sha256",hash},{"sizeBytes",bytes.size()}},
          {{"kind","appearance"},{"format","json"},{"path",sidecar.u8string()},{"sha256",file_sha256(sidecar.u8string())},{"sizeBytes",std::filesystem::file_size(sidecar)}}})}};
}

void apply_color_document(LoadedProject& project,const Json& request,const std::string& workspace,Budget& budget)
{
#ifndef ORCA_MOBILE_REAL_TRIANGULATION
    throw std::runtime_error("COLOR_REBIND_NATIVE_TRIANGULATION_REQUIRED");
#else
    if(!request.value("confirmMaterialMapping",false))throw std::runtime_error("COLOR_REBIND_CONFIRMATION_REQUIRED");
    const auto path=request.at("appearancePath").get<std::string>();
    if(std::filesystem::file_size(std::filesystem::u8path(path))>100u*1024u*1024u||file_sha256(path)!=request.at("appearanceSha256"))
        throw std::runtime_error("COLOR_REBIND_APPEARANCE_IDENTITY");
    std::ifstream input(std::filesystem::u8path(path));Json appearance;input>>appearance;
    const auto& meta=appearance.at("appearanceMetadata");
    if(meta.contains("printColorDraft")||meta.value("printColorCandidate",false)||!meta.contains("printColorPlan"))throw std::runtime_error("COLOR_REBIND_CONFIRMED_PLAN_REQUIRED");
    const auto& plan=meta.at("printColorPlan");
    if(plan.value("trialOnly",false)||plan.at("materialFingerprint")!=request.at("materialFingerprint")||plan.at("processFingerprint")!=request.at("processFingerprint"))
        throw std::runtime_error("COLOR_REBIND_PLAN_CHANGED");
    const auto& binding=meta.at("sourceProjectBinding");
    if(binding.at("projectId")!=request.at("projectId")||binding.at("projectRevision")!=request.at("projectRevision")||binding.at("inputSha256")!=project.inputSha256||binding.at("configSha256")!=json_sha256(config_json(project.config))||binding.at("selectedPlateId")!=project.metadata.value("selectedPlateId",Json()))
        throw std::runtime_error("COLOR_REBIND_SOURCE_CHANGED");
    const auto current=export_color_document(project,request,workspace,budget);
    if(appearance.at("geometrySha256")!=current.at("artifacts").at(0).at("sha256"))throw std::runtime_error("COLOR_REBIND_GEOMETRY_CHANGED");
    const auto originalPath=current.at("artifacts").at(1).at("path").get<std::string>();std::ifstream originalStream(std::filesystem::u8path(originalPath));Json original;originalStream>>original;
    if(meta.at("parts")!=original.at("parts"))throw std::runtime_error("COLOR_REBIND_PARTS_CHANGED");
    const auto& ids=appearance.at("materialIds");if(!ids.is_array()||ids.size()!=original.at("materialIds").size())throw std::runtime_error("COLOR_REBIND_FACE_COUNT");
    const auto prepared=prepare_color_config(project,request);
    if(prepared.at("manufacturingConfig")!=request.at("manufacturingConfig")||prepared.at("processFingerprint")!=request.at("processFingerprint")||
       prepared.at("materialFingerprint")!=request.at("materialFingerprint")||prepared.at("palette")!=plan.at("palette"))throw std::runtime_error("COLOR_REBIND_MANUFACTURING_CONFIG_CHANGED");
    // Validate the complete face assignment before changing the detached native
    // Model. The configured filament array also contains support-only materials;
    // it is not the set of materials permitted to paint model surfaces.
    std::map<int,std::array<double,3>> modelPalette;
    const auto& nativeBindings=prepared.at("bindings");
    for(const auto& material:prepared.at("palette")){
        const int id=material.at("materialId").get<int>();
        const auto route=std::find_if(nativeBindings.begin(),nativeBindings.end(),[&](const auto& x){return x.at("materialId")==id;});
        if(route==nativeBindings.end()||route->at("purpose")!="model"||route->at("slot")!=material.at("slot")||
           !modelPalette.emplace(id,material.at("color").get<std::array<double,3>>()).second)
            throw std::runtime_error("COLOR_REBIND_MODEL_BINDING_MISMATCH");
    }
    const auto& faceColors=appearance.at("faceColors");
    if(!faceColors.is_array()||faceColors.size()!=ids.size())throw std::runtime_error("COLOR_REBIND_FACE_COLOR_COUNT");
    // Direct matching uses PreviewPalette::Color (float32); Mesh and its JSON
    // sidecar store those channels as doubles. 1e-7 absolute normalized sRGB
    // covers float32 rounding (<6e-8), far below one 8-bit step (1/255).
    // Alpha follows the existing Model RGBA contract: finite in [0,1], retained
    // without claiming physical opacity equivalence. Only RGB binds the spool.
    constexpr double faceRgbSerializationTolerance=1e-7;
    for(size_t f=0;f<ids.size();++f){
        budget.check();
        if(!ids.at(f).is_number_integer())throw std::runtime_error("COLOR_REBIND_MATERIAL_OUT_OF_RANGE");
        const auto wide=ids.at(f).get<int64_t>();
        if(wide<0||wide>255)throw std::runtime_error("COLOR_REBIND_MATERIAL_OUT_OF_RANGE");
        const auto material=modelPalette.find(int(wide));
        if(material==modelPalette.end())throw std::runtime_error("COLOR_REBIND_MODEL_MATERIAL_NOT_IN_PLAN");
        const auto& rgba=faceColors.at(f);
        if(!rgba.is_array()||rgba.size()!=4)throw std::runtime_error("COLOR_REBIND_FACE_COLOR_INVALID");
        for(size_t channel=0;channel<4;++channel){
            if(!rgba.at(channel).is_number())throw std::runtime_error("COLOR_REBIND_FACE_COLOR_INVALID");
            const double value=rgba.at(channel).get<double>();
            if(!std::isfinite(value)||value<0||value>1)throw std::runtime_error("COLOR_REBIND_FACE_COLOR_INVALID");
            if(channel<3&&std::abs(value-material->second[channel])>faceRgbSerializationTolerance)
                throw std::runtime_error("COLOR_REBIND_FACE_COLOR_MISMATCH");
        }
    }
    apply_config_overrides(project.config,prepared.at("manufacturingConfig"));
    const auto* colors=project.config.option<Slic3r::ConfigOptionStrings>("filament_colour");
    std::map<std::pair<size_t,size_t>,Json> assigned;
    for(const auto& part:original.at("parts")){
        const auto oi=part.at("objectIndex").get<size_t>(),vi=part.at("volumeIndex").get<size_t>(),first=part.at("firstTriangle").get<size_t>(),count=part.at("triangleCount").get<size_t>();
        Json values=Json::array();for(size_t f=0;f<count;++f){const int id=ids.at(first+f).get<int>();if(id<0||!colors||size_t(id)>=colors->values.size()||id+1>int(Slic3r::EnforcerBlockerType::ExtruderMax))throw std::runtime_error("COLOR_REBIND_MATERIAL_OUT_OF_RANGE");values.push_back(id);}
        auto inserted=assigned.emplace(std::make_pair(oi,vi),values);
        if(!inserted.second){if(inserted.first->second!=values)throw std::runtime_error("COLOR_REBIND_INSTANCE_COLOR_CONFLICT");continue;}
        auto* volume=project.model.objects.at(oi)->volumes.at(vi);indexed_triangle_set flattened;
        auto append=[&](const indexed_triangle_set& mesh){const int offset=int(flattened.vertices.size());flattened.vertices.insert(flattened.vertices.end(),mesh.vertices.begin(),mesh.vertices.end());for(auto face:mesh.indices){face.array()+=offset;flattened.indices.push_back(face);}};
        if(volume->mmu_segmentation_facets.empty())append(volume->mesh().its);
        else{std::vector<indexed_triangle_set> groups;volume->mmu_segmentation_facets.get_facets(*volume,groups);for(const auto& mesh:groups)if(!mesh.indices.empty())append(mesh);}
        if(flattened.indices.size()!=count)throw std::runtime_error("COLOR_REBIND_LEAF_COUNT");
        volume->set_mesh(std::move(flattened));Slic3r::TriangleSelector selector(volume->mesh());
        for(size_t f=0;f<count;++f){budget.check();selector.set_facet(int(f),static_cast<Slic3r::EnforcerBlockerType>(values.at(f).get<int>()+1));}
        volume->mmu_segmentation_facets.set(selector);volume->config.erase("extruder");project.model.objects.at(oi)->config.erase("extruder");project.model.objects.at(oi)->invalidate_bounding_box();
    }
    project.metadata["multiMaterialConfirmed"]=true;
    project.metadata["nativeMaterialConfigurationVerified"]=true;
    project.metadata["filamentConfirmationSource"]="operator";
    project.metadata["actualFilamentLoadingVerified"]=false;
    project.metadata["printColorPlan"]=plan;
    project.metadata["materialAssignments"]=Json::array();
    project.metadata.erase("outputMode");
    project.metadata.erase("singleMaterialFilamentIndex");
#endif
}
