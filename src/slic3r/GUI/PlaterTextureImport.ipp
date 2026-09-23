// Included by Plater.cpp after Plater::priv is defined.
std::vector<size_t> Plater::load_files(const std::vector<fs::path>& input_files,
    LoadStrategy strategy, bool ask_multi, ObjImportColorFn obj_color_fn,
    ModelColorImportResult* color_result, const TextureImportOptions* texture_options)
{
    p->m_slice_all_only_has_gcode = false;
    p->preview->get_canvas3d()->reset_select_plate_toolbar_selection();
    return p->load_files(input_files, strategy, ask_multi, std::move(obj_color_fn), color_result, texture_options);
}

bool Plater::priv::run_textured_mesh_import_dialog(Slic3r::Model& loaded_model, TextureImportResult& result,
                                                   std::function<bool()> cancel_callback,
                                                   std::function<bool(int)> progress_callback,
                                                   const TextureImportOptions* texture_options)
{
    if (!loaded_model.texture_mesh || !has_importable_texture(*loaded_model.texture_mesh)) return false;

    // Defense in depth: if all geometry got dropped earlier (e.g. by a future
    // regression of the zero-volume cleanup) but the textured mesh is still
    // alive, there is nothing for the dialog to paint onto. Skip the dialog
    // gracefully so load_files() can fall through to its "no geometry"
    // message instead of making the user round-trip a meaningless matcher.
    if (loaded_model.objects.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "handle_textured_mesh_import: skipping dialog because the loaded model has no geometry objects";
        loaded_model.texture_mesh.reset();
        result.skipped = true;
        return true;
    }

    const wxString fallback_warning = _L("Texture import failed. The model appears to contain texture data, but the texture import process could not be completed. The model will be imported as geometry only.");

    BOOST_LOG_TRIVIAL(info) << "handle_textured_mesh_import: opening texture import dialog";

    std::vector<TextureFilamentEntry> filament_entries;
    {
        auto& preset_bundle = *wxGetApp().preset_bundle;
        auto& project_config = preset_bundle.project_config;
        auto* colours_opt = project_config.option<ConfigOptionStrings>("filament_colour");
        auto* is_mixed_opt = project_config.option<ConfigOptionBools>("filament_is_mixed");
        auto* components_opt = project_config.option<ConfigOptionStrings>("filament_mixed_components");
        auto* ratios_opt = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
        const size_t total = preset_bundle.filament_presets.size();
        filament_entries.reserve(total);
        for (size_t i = 0; i < total; ++i) {
            TextureFilamentEntry entry;
            entry.kind = (is_mixed_opt && i < is_mixed_opt->values.size() && is_mixed_opt->values[i]) ?
                TextureFilamentKind::ExistingMixed : TextureFilamentKind::ExistingPhysical;
            entry.dialog_index = (int)filament_entries.size();
            entry.project_config_index = i;
            entry.color_hex = (colours_opt && i < colours_opt->values.size()) ? colours_opt->values[i] : "#808080";

            std::string name;
            if (i < preset_bundle.filament_presets.size()) {
                auto* preset = preset_bundle.filaments.find_preset(preset_bundle.filament_presets[i]);
                if (preset) {
                    name = preset->label(false);
                    // Material type belongs to each filament preset, not to
                    // the project's color/mixing arrays. Match the workbench
                    // snapshot's source for this identity check.
                    const auto* types = preset->config.option<ConfigOptionStrings>("filament_type");
                    if (types && !types->values.empty()) entry.type = types->get_at(0);
                }
            }
            if (name.empty())
                name = "Filament " + std::to_string(i + 1);
            entry.name = name;

            if (entry.kind == TextureFilamentKind::ExistingMixed) {
                if (components_opt && i < components_opt->values.size())
                    entry.mixed_components = Slic3r::parse_mixed_components(components_opt->values[i]);
                std::vector<double> ratios = Slic3r::parse_mixed_ratios(
                    ratios_opt && i < ratios_opt->values.size() ? ratios_opt->values[i] : "",
                    entry.mixed_components.size());
                entry.mixed_ratios.reserve(ratios.size());
                for (double ratio : ratios)
                    entry.mixed_ratios.push_back((int)std::lround(ratio * 100.0));
            }
            filament_entries.push_back(std::move(entry));
        }
    }

    TextureImportOptions options = texture_options ? *texture_options : TextureImportOptions{};
    if(!options.matched_face_slots.empty()) {
        try {
            if(cancel_callback && cancel_callback())return false;
            apply_matched_texture_colors(loaded_model,options,filament_entries);
            result.matched_colors=true;
            result.painted.cluster_colors.resize(std::set<size_t>(options.matched_face_slots.begin(),options.matched_face_slots.end()).size());
            return true;
        }catch(const std::exception& e) {MessageDialog(q,from_u8(e.what()),_L("区域配色"),wxOK|wxICON_ERROR).ShowModal();return false;}
    }
    const std::string& source = loaded_model.objects.front()->input_file;
    if (boost::algorithm::iends_with(source, ".glb") || boost::algorithm::iends_with(source, ".gltf"))
        options.z_up = true; // The native loader has already converted glTF Y-up to Z-up.
    TextureImportDialog dlg(q, *loaded_model.texture_mesh, filament_entries,
                            std::move(cancel_callback), std::move(progress_callback),
                            options);
    if (dlg.ShowModal() != wxID_OK) {
        if (dlg.was_skipped()) {
            BOOST_LOG_TRIVIAL(info) << "handle_textured_mesh_import: user skipped texture matching";
            result.skipped = true;
            loaded_model.texture_mesh.reset();
            return true;
        }
        if (dlg.fallback_to_geometry_only()) {
            BOOST_LOG_TRIVIAL(warning) << "handle_textured_mesh_import: texture import failed, falling back to geometry-only import";
            result.fallback_to_geometry_only = true;
            result.fallback_warning = fallback_warning;
            loaded_model.texture_mesh.reset();
            return true;
        }
        BOOST_LOG_TRIVIAL(info) << "handle_textured_mesh_import: user cancelled";
        loaded_model.texture_mesh.reset();
        return false;
    }

    auto painted = dlg.get_painted_mesh();
    auto final_matches = dlg.get_matches();

    if (painted.face_colors.empty() || final_matches.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "handle_textured_mesh_import: no painting result";
        result.fallback_to_geometry_only = true;
        result.fallback_warning = fallback_warning;
        loaded_model.texture_mesh.reset();
        return true;
    }

    BOOST_LOG_TRIVIAL(info) << "handle_textured_mesh_import: got " << painted.cluster_colors.size()
                            << " clusters, skipped=" << dlg.was_skipped();

    result.painted = std::move(painted);
    result.matches = std::move(final_matches);
    result.new_filament_colors = dlg.get_new_filament_colors();
    result.new_filament_preset_names = dlg.get_new_filament_preset_names();
    result.new_mixed_filaments = dlg.get_new_mixed_filaments();
    result.filament_entries = dlg.get_filament_entries();
    result.existing_filament_count = dlg.get_existing_filament_count();
    result.skipped = dlg.was_skipped();
    return true;
}

void Plater::priv::apply_textured_mesh_import_result(Slic3r::Model& loaded_model, const std::vector<size_t>& obj_idxs,
                                                     const TextureImportResult& result,
                                                     LoadProgressCallback progress_callback, bool update_scene)
{
    if(result.matched_colors) {loaded_model.texture_mesh.reset();if(update_scene)update();return;}
    auto update_apply_progress = [&progress_callback](int percent, const wxString& message) {
        return !progress_callback || progress_callback(std::clamp(percent, 0, 100), message);
    };

    const auto& painted = result.painted;
    const auto& final_matches = result.matches;

    if (painted.face_colors.empty() || final_matches.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "handle_textured_mesh_import: no painting result";
        loaded_model.texture_mesh.reset();
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "handle_textured_mesh_import: got " << painted.cluster_colors.size()
                            << " clusters, skipped=" << result.skipped;
    if (!update_apply_progress(0, _L("Applying texture colors...")))
        return;

    auto collect_physical_color_strs = []() {
        std::vector<std::string> colors;
        auto& project_config = wxGetApp().preset_bundle->project_config;
        auto* colours_opt = project_config.option<ConfigOptionStrings>("filament_colour");
        auto* is_mixed_opt = project_config.option<ConfigOptionBools>("filament_is_mixed");
        const size_t total = wxGetApp().preset_bundle->filament_presets.size();
        for (size_t i = 0; i < total; ++i) {
            const bool is_mixed = is_mixed_opt && i < is_mixed_opt->values.size() && is_mixed_opt->values[i];
            if (!is_mixed)
                colors.push_back(colours_opt && i < colours_opt->values.size() ? colours_opt->values[i] : "#808080");
        }
        return colors;
    };

    const auto& entries = result.filament_entries;
    std::vector<int> filament_index_remap(entries.size(), -1);
    size_t existing_physical_count = 0;
    size_t new_physical_count = 0;
    for (const auto& entry : entries) {
        if (entry.kind == TextureFilamentKind::ExistingPhysical)
            ++existing_physical_count;
        else if (entry.kind == TextureFilamentKind::NewPhysical)
            ++new_physical_count;
    }

    for (const auto& entry : entries) {
        if (entry.dialog_index < 0 || entry.dialog_index >= (int)filament_index_remap.size())
            continue;
        if (entry.kind == TextureFilamentKind::ExistingPhysical) {
            filament_index_remap[entry.dialog_index] = (int)entry.project_config_index;
        } else if (entry.kind == TextureFilamentKind::ExistingMixed) {
            filament_index_remap[entry.dialog_index] = (int)(entry.project_config_index + new_physical_count);
        }
    }

    size_t new_physical_order = 0;
    for (const auto& entry : entries) {
        if (entry.kind != TextureFilamentKind::NewPhysical)
            continue;
        wxColour new_col(entry.color_hex);
        const size_t final_idx = existing_physical_count + new_physical_order;
        sidebar->add_custom_filament(new_col, entry.preset_name);
        if (entry.dialog_index >= 0 && entry.dialog_index < (int)filament_index_remap.size())
            filament_index_remap[entry.dialog_index] = (int)final_idx;
        BOOST_LOG_TRIVIAL(info) << "handle_textured_mesh_import: created pending physical filament dialog="
                                << entry.dialog_index << " final=" << final_idx
                                << " color=" << entry.color_hex
                                << " preset=" << entry.preset_name;
        ++new_physical_order;
    }

    std::vector<std::string> physical_colors_for_mixing = collect_physical_color_strs();
    for (const auto& mixed : result.new_mixed_filaments) {
        MixedFilamentResult mixed_result;
        mixed_result.ratios = mixed.ratios;
        mixed_result.components.reserve(mixed.component_dialog_indices.size());
        bool valid_components = true;
        for (int component_dialog_idx : mixed.component_dialog_indices) {
            if (component_dialog_idx < 0 || component_dialog_idx >= (int)filament_index_remap.size() ||
                filament_index_remap[component_dialog_idx] < 0) {
                valid_components = false;
                break;
            }
            mixed_result.components.push_back((unsigned int)(filament_index_remap[component_dialog_idx] + 1));
        }
        if (!valid_components || mixed_result.components.size() < 2 ||
            mixed_result.components.size() != mixed_result.ratios.size()) {
            BOOST_LOG_TRIVIAL(warning) << "handle_textured_mesh_import: invalid pending mixed filament dialog="
                                       << mixed.dialog_index;
            continue;
        }

        const int final_idx = (int)wxGetApp().preset_bundle->filament_presets.size();
        if (create_mixed_filament_from_result(sidebar, mixed_result, physical_colors_for_mixing)) {
            if (mixed.dialog_index >= 0 && mixed.dialog_index < (int)filament_index_remap.size())
                filament_index_remap[mixed.dialog_index] = final_idx;
            physical_colors_for_mixing = collect_physical_color_strs();
            BOOST_LOG_TRIVIAL(info) << "handle_textured_mesh_import: created pending mixed filament dialog="
                                    << mixed.dialog_index << " final=" << final_idx;
        }
    }

    std::vector<Slic3r::FilamentMatch> remapped_matches = final_matches;
    for (auto& m : remapped_matches) {
        if (m.filament_index < 0)
            continue;
        if (m.filament_index < (int)filament_index_remap.size() && filament_index_remap[m.filament_index] >= 0) {
            m.filament_index = filament_index_remap[m.filament_index];
        } else {
            BOOST_LOG_TRIVIAL(warning) << "handle_textured_mesh_import: invalid filament index "
                                       << m.filament_index << " in texture mapping";
            m.filament_index = -1;
        }
    }

    int min_used_filament_1based = -1;
    {
        std::map<std::array<std::size_t, 3>, int> color_to_filament;
        for (const auto& m : remapped_matches) {
            if (m.cluster_index >= 0 && m.cluster_index < (int)painted.cluster_colors.size() && m.filament_index >= 0)
                color_to_filament[painted.cluster_colors[m.cluster_index]] = m.filament_index + 1;
        }
        for (const auto& face_color : painted.face_colors) {
            auto it = color_to_filament.find(face_color);
            if (it == color_to_filament.end())
                continue;
            if (min_used_filament_1based < 0 || it->second < min_used_filament_1based)
                min_used_filament_1based = it->second;
        }
    }
    if (min_used_filament_1based < 0)
        BOOST_LOG_TRIVIAL(warning) << "handle_textured_mesh_import: cannot determine base filament from painted faces";

    if (!update_apply_progress(25, _L("Applying texture colors...")))
        return;

    for (size_t obj_order = 0; obj_order < obj_idxs.size(); ++obj_order) {
        size_t idx = obj_idxs[obj_order];
        if (idx >= loaded_model.objects.size()) continue;
        ModelObject* obj = loaded_model.objects[idx];
        if (!obj) continue;

        // painted is derived from the whole textured mesh and is meaningful
        // only against a single MODEL_PART volume. Applying it to every
        // volume of a multi-part / modifier object would overwrite each
        // volume with the same painted geometry. Restrict to the first
        // model_part and warn when the object holds more than one.
        ModelVolume* target = nullptr;
        int part_count = 0;
        for (ModelVolume* vol : obj->volumes) {
            if (vol && vol->is_model_part()) {
                ++part_count;
                if (!target) target = vol;
            }
        }
        if (!target) continue;
        if (part_count > 1) {
            BOOST_LOG_TRIVIAL(warning)
                << "handle_textured_mesh_import: object has " << part_count
                << " model parts; painting only applied to the first part.";
        }
        if (Slic3r::apply_painted_mesh_to_volume(painted, remapped_matches, *target)
            && min_used_filament_1based > 0) {
            target->config.set("extruder", min_used_filament_1based);
            obj->config.set("extruder", min_used_filament_1based);
            if (update_scene) {
                if (auto* obj_list = wxGetApp().obj_list()) {
                    obj_list->update_objects_list_filament_column(std::max<size_t>(
                        wxGetApp().filaments_cnt(), (size_t)min_used_filament_1based));
                    obj_list->update_info_items(idx);
                }
            }
            BOOST_LOG_TRIVIAL(info) << "handle_textured_mesh_import: set base filament to "
                                    << min_used_filament_1based << " for object index " << idx
                                    << ", object extruder=" << obj->config.extruder()
                                    << ", volume extruder=" << target->config.extruder();
        }
        // bbox invalidation is performed inside apply_painted_mesh_to_volume.
        obj->ensure_on_bed();
        const int object_percent = 25 + (int)(60 * (obj_order + 1) / std::max<size_t>(obj_idxs.size(), 1));
        if (!update_apply_progress(object_percent, _L("Applying texture colors...")))
            return;
    }

    BOOST_LOG_TRIVIAL(info) << "handle_textured_mesh_import: painting applied to model volumes";
    loaded_model.texture_mesh.reset();
    if (update_scene) {
        if (!update_apply_progress(90, _L("Updating 3D view...")))
            return;
        update();
    }
    update_apply_progress(100, _L("Texture colors applied."));
}

void Plater::priv::handle_textured_mesh_import(Slic3r::Model& loaded_model, const std::vector<size_t>& obj_idxs,
                                               std::function<bool()> cancel_callback)
{
    TextureImportResult result;
    if (!run_textured_mesh_import_dialog(loaded_model, result, std::move(cancel_callback)))
        return;
    if (!result.painted.face_colors.empty())
        apply_textured_mesh_import_result(loaded_model, obj_idxs, result);
}
