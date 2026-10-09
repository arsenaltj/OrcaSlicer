// Included by Plater.cpp after the private loader and native color helpers.
AI::ModelImportResult Plater::import_workbench_model(const AI::ModelImportRequest& request)
{
    AI::ModelImportResult result;
    result.color_mode = request.color_mode;
    auto fail = [&](const std::string& error, AI::ModelImportOutcome outcome = AI::ModelImportOutcome::ImportFailed) {
        result.error = error; result.outcome = outcome; return result;
    };
    try {
        if (!wxGetApp().preset_bundle || !p->can_begin_project_config_change() || is_background_process_slicing())
            return fail("Finish the active operation before importing the working copy.");
        const fs::path path(request.artifact.local_path);
        const auto source_hash = AI::model_artifact_sha256(path);
        if (source_hash.empty()) return fail("The working model is missing or invalid.", AI::ModelImportOutcome::InvalidArtifact);
        const auto workspace_unchanged = OrcaWorkspaceAdapter(this, {}).capture_import_guard();
        TriangleMesh source;
        ObjInfo source_colors;
        if (!AI::load_model_artifact(path, source, source_colors, result.error))
            return fail(result.error, AI::ModelImportOutcome::InvalidArtifact);
        const bool local_colors = !request.face_color_overrides.empty() || !request.subface_color_overrides.empty();
        if (local_colors && request.face_color_geometry_id != AI::SurfaceSelectionPersistence::geometry_fingerprint(source.its))
            return fail("The local colors belong to another geometry version.", AI::ModelImportOutcome::InvalidArtifact);
        if (local_colors && request.color_mode != AI::ImportColorMode::NativeMatch &&
            show_redesign_confirmation(this, _L("此匹配方式将重新分配局部改色的耗材。继续？"), _L("配色确认"),
                {wxYES_NO | wxNO_DEFAULT}) != wxID_YES)
            return fail({}, AI::ModelImportOutcome::Cancelled);
        TextureImportOptions options = model_import_color_options(request);
        options.workbench_review = true;
        if (request.matched_colors && request.color_mode == AI::ImportColorMode::NativeMatch) {
            const auto& matched = *request.matched_colors;
            if (!matched.valid() || matched.source_sha256 != source_hash ||
                matched.geometry_id != AI::SurfaceSelectionPersistence::geometry_fingerprint(source.its))
                return fail("The saved material assignment belongs to another model version.", AI::ModelImportOutcome::InvalidArtifact);
            options.matched_source = std::make_shared<indexed_triangle_set>(source.its);
        }
        const bool obj_match = request.color_mode == AI::ImportColorMode::AutoMap || request.color_mode == AI::ImportColorMode::ManualMatch;
        auto import_path = path;
        bool cancelled = false;
        ObjImportColorFn obj_mapper;
        if (obj_match) {
            const auto cache = fs::path(Slic3r::temporary_dir()) / "ai-import";
            import_path = cache / ("orcaslicer-ai-glb-" + source_hash + ".obj");
            if (!AI::write_model_artifact(import_path, source.its, source_colors.vertex_colors, result.error)) return fail(result.error);
            options.source_units_in_meters = false;
            obj_mapper = workbench_obj_color_mapper(this, request.color_mode,
                OrcaWorkspaceAdapter(this, {}).printable_palette(), result, cancelled);
        }
        Model detached = Model::read_from_file(import_path.string(), nullptr, nullptr, LoadStrategy::LoadModel,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, std::move(obj_mapper));
        if (cancelled) return fail({}, AI::ModelImportOutcome::Cancelled);
        if (detached.objects.size() != 1 || detached.objects.front()->volumes.size() != 1)
            return fail("This working copy requires one model-part volume.");
        apply_texture_import_units(detached, &options);
        auto& object = *detached.objects.front();
        auto staged = std::make_unique<PresetBundle>(*wxGetApp().preset_bundle);
        if (request.color_mode == AI::ImportColorMode::SingleColor) {
            object.config.set("extruder", 1);
            object.volumes.front()->config.set("extruder", 1);
        } else if (!obj_match) {
            priv::TextureImportResult mapping;
            if (!p->run_textured_mesh_import_dialog(detached, mapping, {}, {}, &options))
                return fail({}, AI::ModelImportOutcome::Cancelled);
            if (mapping.fallback_to_geometry_only)
                return fail("Color preparation failed. The working copy is retained; choose single color explicitly to continue.");
            if (mapping.skipped) result.color_mode = AI::ImportColorMode::SingleColor;
            else if (mapping.matched_colors) result.colors_applied = true;
            else {
                if (!stage_workbench_texture_import(object, mapping.painted, mapping.matches,
                        mapping.filament_entries, mapping.new_mixed_filaments, *staged, result.error)) return fail(result.error);
                result.colors_applied = true;
                result.source_color_count = mapping.painted.cluster_colors.size();
                std::set<int> used;
                for (const auto& match : mapping.matches) used.insert(match.filament_index);
                result.mapped_color_count = used.size();
            }
            if (result.colors_applied && !request.subface_color_overrides.empty()) {
                auto* volume = object.volumes.front();
                if (!LocalPrintColorApplication::same_surface_partition(source.its, volume->mesh().its, false))
                    return fail("The native loader changed the local color topology.");
                auto painting = volume->mmu_segmentation_facets.get_data();
                if (!apply_subface_color_overrides(volume->mesh().its, volume->mesh().its, painting,
                        request.face_color_overrides, request.subface_color_overrides, result.error)) return fail(result.error);
                volume->mmu_segmentation_facets.set_data(std::move(painting));
                result.subface_colors_applied = true;
                result.subface_color_count = request.subface_color_overrides.size();
            }
        }
        detached.texture_mesh.reset();
        if (source_hash != AI::model_artifact_sha256(path) || !workspace_unchanged())
            return fail("The model, materials or project changed during confirmation. Reopen color matching.");
        size_t index = 0;
        if (!adopt_local_print_model(object, staged.get(), index, result.error)) return fail(result.error);
        result.outcome = AI::ModelImportOutcome::Imported;
        return result;
    } catch (const std::exception& exception) { return fail(exception.what()); }
}
