// Included after the synchronous compatibility importer in Plater.cpp.
bool Plater::import_workbench_model_async(const AI::ModelImportRequest& request,
    std::shared_ptr<WorkbenchImportSession> session, WorkbenchImportProgress progress,
    WorkbenchImportCompletion completion, std::function<bool()> asset_current)
{
    if (!session || !asset_current || !asset_current() || !wxGetApp().preset_bundle ||
        !get_ui_job_worker().is_idle() || is_background_process_slicing()) return false;
    struct PreparedImport {
        AI::ModelImportResult result;
        LocalPrintModelImport::PlacementSnapshot placement;
        std::unique_ptr<Model> model;
        std::unique_ptr<PresetBundle> bundle;
        std::vector<TextureFilamentEntry> filaments;
        bool dialog_cancelled {false};
        bool source_unchanged {false};
    };
    auto prepared = std::make_shared<PreparedImport>();
    prepared->result.color_mode = request.color_mode;
    if (!capture_local_print_placement(prepared->placement, prepared->result.error)) {
        session->advance(WorkbenchImportPhase::Failed);
        completion(prepared->result);
        return true;
    }
    prepared->bundle = std::make_unique<PresetBundle>(*wxGetApp().preset_bundle);
    prepared->filaments = workbench_texture_filaments(*prepared->bundle);
    auto workspace_current = OrcaWorkspaceAdapter(this, {}).capture_import_guard();
    auto options = model_import_color_options(request);
    options.workbench_review = true;
    auto obj_mapper = workbench_obj_color_mapper(this, request.color_mode,
        OrcaWorkspaceAdapter(this, {}).printable_palette(), prepared->result, prepared->dialog_cancelled, true);
    wxWeakRef<Plater> weak(this);
    return queue_job(get_ui_job_worker(),
        [weak, request, session, prepared, progress, asset_current, options = std::move(options),
         obj_mapper = std::move(obj_mapper)](Job::Ctl& ctl) mutable {
            const auto started = std::chrono::steady_clock::now();
            auto last = started;
            auto phase = WorkbenchImportPhase::Reading;
            auto stopped = [&] { return ctl.was_canceled() || session->cancelled(); };
            auto fail = [&](const std::string& error, AI::ModelImportOutcome outcome = AI::ModelImportOutcome::ImportFailed) {
                prepared->result.error = error;
                prepared->result.outcome = outcome;
            };
            auto ui = [&](std::function<void()> action) {
                ctl.call_on_main_thread([weak, session, asset_current, action = std::move(action), prepared] {
                    if (!weak || session->cancelled()) return;
                    if (!asset_current()) {
                        prepared->result.error = "The working model changed during import.";
                        session->cancel();
                        return;
                    }
                    try { action(); }
                    catch (const std::exception& e) { prepared->result.error = e.what(); session->cancel(); }
                }).get();
            };
            auto notify = [&](WorkbenchImportPhase next, const std::string& message = std::string{}) {
                const auto now = std::chrono::steady_clock::now();
                BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] phase=" << int(phase) << " ms=" <<
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
                last = now;
                phase = next;
                if (!session->advance(next)) return false;
                ui([&] { if (progress) progress(next, message); });
                return !stopped();
            };
            try {
                const fs::path path(request.artifact.local_path);
                const auto source_hash = AI::model_artifact_sha256(path);
                if (source_hash.empty()) { fail("The working model is missing or invalid.", AI::ModelImportOutcome::InvalidArtifact); return; }
                if (stopped()) return;
                const bool obj_match = request.color_mode == AI::ImportColorMode::AutoMap || request.color_mode == AI::ImportColorMode::ManualMatch;
                const bool local_colors = !request.face_color_overrides.empty() || !request.subface_color_overrides.empty();
                TriangleMesh source;
                ObjInfo source_colors;
                const bool needs_source = obj_match || (request.color_mode == AI::ImportColorMode::NativeMatch &&
                    (local_colors || request.matched_colors));
                if (needs_source &&
                    !AI::load_model_artifact(path, source, source_colors, prepared->result.error, stopped)) {
                    if (stopped()) { prepared->result.error.clear(); return; }
                    fail(prepared->result.error, AI::ModelImportOutcome::InvalidArtifact); return;
                }
                if (local_colors && needs_source && request.face_color_geometry_id != AI::SurfaceSelectionPersistence::geometry_fingerprint(source.its)) {
                    fail("The local colors belong to another geometry version.", AI::ModelImportOutcome::InvalidArtifact); return;
                }
                if (stopped()) return;
                if (request.matched_colors && request.color_mode == AI::ImportColorMode::NativeMatch) {
                    const auto& matched = *request.matched_colors;
                    if (!matched.valid() || matched.source_sha256 != source_hash ||
                        matched.geometry_id != AI::SurfaceSelectionPersistence::geometry_fingerprint(source.its)) {
                        fail("The saved material assignment belongs to another model version.", AI::ModelImportOutcome::InvalidArtifact); return;
                    }
                    options.matched_source = std::make_shared<indexed_triangle_set>(source.its);
                }
                auto import_path = path;
                if (obj_match) {
                    import_path = fs::path(Slic3r::temporary_dir()) / "ai-import" / ("orcaslicer-ai-glb-" + source_hash + ".obj");
                    if (!AI::write_model_artifact(import_path, source.its, source_colors.vertex_colors, prepared->result.error)) return;
                    options.source_units_in_meters = false;
                }
                DecodedWorkbenchModel decoded;
                if (!decode_workbench_model(import_path, decoded, prepared->result.error, stopped, obj_match)) {
                    if (stopped()) prepared->result.error.clear();
                    return;
                }
                if (options.source_units_in_meters) decoded.mesh.scale(1000.f);
                std::unique_ptr<Model> detached_model;
                ui([&] { detached_model = assemble_workbench_model(decoded, import_path, options.source_units_in_meters); });
                if (stopped() || !detached_model) return;
                auto& detached = *detached_model;
                if (local_colors && request.color_mode != AI::ImportColorMode::NativeMatch) {
                    if (!notify(WorkbenchImportPhase::Colors, "等待配色确认")) return;
                    ui([&] {
                        if (show_redesign_confirmation(weak.get(), _L("此匹配方式将重新分配局部改色的耗材。继续？"), _L("配色确认"),
                            {wxYES_NO | wxNO_DEFAULT}) != wxID_YES) session->cancel();
                    });
                    if (stopped()) return;
                }
                if (obj_match && (!decoded.colors.vertex_colors.empty() ||
                    (!decoded.colors.face_colors.empty() && !decoded.colors.has_uv_png))) {
                    if (!notify(WorkbenchImportPhase::Colors, request.color_mode == AI::ImportColorMode::ManualMatch ? "等待配色确认" : "")) return;
                    ObjDialogInOut input {};
                    input.model = &detached;
                    input.first_extruder_id = 1;
                    input.deal_vertex_color = !decoded.colors.vertex_colors.empty();
                    input.is_single_color = !input.deal_vertex_color && decoded.colors.is_single_mtl;
                    input.lost_material_name = decoded.colors.lost_material_name;
                    input.input_colors = input.deal_vertex_color ? std::move(decoded.colors.vertex_colors) : std::move(decoded.colors.face_colors);
                    ui([&] {
                        obj_mapper(input);
                        if (request.color_mode == AI::ImportColorMode::ManualMatch && !input.cancelled)
                            prepared->result.colors_applied = input.deal_vertex_color
                                ? Model::obj_import_vertex_color_deal(input.filament_ids, input.first_extruder_id, input.model)
                                : Model::obj_import_face_color_deal(input.filament_ids, input.first_extruder_id, input.model);
                    });
                    if (input.cancelled) session->cancel();
                    if (!prepared->result.colors_applied && !session->cancelled()) { fail("The selected material mapping could not be applied."); return; }
                }
                if (stopped() || prepared->dialog_cancelled) { session->cancel(); return; }
                if (detached.objects.size() != 1 || detached.objects.front()->volumes.size() != 1) {
                    fail("This working copy requires one model-part volume."); return;
                }
                auto& object = *detached.objects.front();
                if (!notify(WorkbenchImportPhase::Colors)) return;
                if (request.color_mode == AI::ImportColorMode::SingleColor) {
                    ui([&] {
                        object.config.set("extruder", 1);
                        object.volumes.front()->config.set("extruder", 1);
                    });
                } else if (!obj_match) {
                    priv::TextureImportResult mapping;
                    bool accepted = false;
                    if (!options.matched_face_slots.empty()) {
                        ui([&] {
                            apply_matched_texture_colors(detached, options, prepared->filaments);
                            mapping.matched_colors = accepted = true;
                        });
                    } else {
                        ui([&] {
                            if (progress) progress(WorkbenchImportPhase::Colors, "等待配色确认");
                            accepted = weak->p->run_textured_mesh_import_dialog(detached, mapping,
                                [session] { return session->cancelled(); }, {}, &options);
                        });
                    }
                    if (stopped()) return;
                    if (!accepted) { session->cancel(); return; }
                    if (mapping.fallback_to_geometry_only) { fail("Color preparation failed. Choose single color explicitly to continue."); return; }
                    if (mapping.skipped) prepared->result.color_mode = AI::ImportColorMode::SingleColor;
                    else if (mapping.matched_colors) prepared->result.colors_applied = true;
                    else {
                        bool staged = false;
                        ui([&] {
                            staged = stage_workbench_texture_import(object, mapping.painted, mapping.matches,
                                mapping.filament_entries, mapping.new_mixed_filaments, *prepared->bundle, prepared->result.error);
                        });
                        if (!staged) return;
                        prepared->result.colors_applied = true;
                        prepared->result.source_color_count = mapping.painted.cluster_colors.size();
                        std::set<int> used;
                        for (const auto& match : mapping.matches) used.insert(match.filament_index);
                        prepared->result.mapped_color_count = used.size();
                    }
                    if (prepared->result.colors_applied && !request.subface_color_overrides.empty()) {
                        auto* volume = object.volumes.front();
                        if (!LocalPrintColorApplication::same_surface_partition(source.its, volume->mesh().its, false)) {
                            fail("The native loader changed the local color topology."); return;
                        }
                        auto painting = volume->mmu_segmentation_facets.get_data();
                        if (!apply_subface_color_overrides(volume->mesh().its, volume->mesh().its, painting,
                            request.face_color_overrides, request.subface_color_overrides, prepared->result.error)) return;
                        ui([&] { volume->mmu_segmentation_facets.set_data(std::move(painting)); });
                        prepared->result.subface_colors_applied = true;
                        prepared->result.subface_color_count = request.subface_color_overrides.size();
                    }
                }
                detached.texture_mesh.reset();
                if (!notify(WorkbenchImportPhase::Placement)) return;
                bool created = false;
                ui([&] {
                    created = LocalPrintModelImport::prepare(object, prepared->placement.placement,
                        prepared->placement.bed_size, prepared->model, prepared->result.error);
                });
                if (!created || stopped()) return;
                if (!LocalPrintModelImport::place(*prepared->model->objects.front(), prepared->placement.bed,
                    prepared->placement.obstacles, prepared->placement.config, prepared->placement.height, prepared->result.error)) return;
                prepared->source_unchanged = source_hash == AI::model_artifact_sha256(path);
                if (!prepared->source_unchanged) fail("The working model changed during import.", AI::ModelImportOutcome::InvalidArtifact);
            } catch (const std::exception& e) { fail(e.what()); }
            BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] preparation_ms=" <<
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        },
        [weak, session, prepared, progress, completion, asset_current, workspace_current](bool cancelled, std::exception_ptr& error) {
            if (!weak || !session->valid()) { error = nullptr; return; }
            if (error) {
                try { std::rethrow_exception(error); } catch (const std::exception& e) { prepared->result.error = e.what(); }
                error = nullptr;
            }
            if (cancelled || session->cancelled()) {
                if (prepared->result.error.empty()) prepared->result.outcome = AI::ModelImportOutcome::Cancelled;
            } else if (prepared->result.error.empty() && prepared->source_unchanged && prepared->model) {
                if (!asset_current() || !workspace_current()) prepared->result.error = "The model, materials or project changed during import. Reopen color matching.";
                else if (session->advance(WorkbenchImportPhase::Committing)) {
                    if (progress) progress(WorkbenchImportPhase::Committing, {});
                    size_t index = 0;
                    const auto started = std::chrono::steady_clock::now();
                    try {
                        if (weak->commit_local_print_model(*prepared->model->objects.front(), prepared->bundle.get(), index, prepared->result.error))
                            prepared->result.outcome = AI::ModelImportOutcome::Imported;
                    } catch (const std::exception& e) { prepared->result.error = e.what(); }
                    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] commit_ms=" <<
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                }
            }
            session->advance(prepared->result.imported() ? WorkbenchImportPhase::UpdatingView : WorkbenchImportPhase::Failed);
            if (completion) completion(prepared->result);
        });
}
