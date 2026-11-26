// BBS: backup & restore
std::vector<size_t> Plater::priv::load_files(const std::vector<fs::path>& input_files,
                                             LoadStrategy strategy,
                                             bool ask_multi,
                                             ObjImportColorFn obj_color_fn,
                                             ModelColorImportResult* color_result,
                                             const TextureImportOptions* texture_options,
                                             bool* published_out,
                                             bool* config_loaded_out,
                                             const std::function<void()>& before_publish,
                                             const std::function<bool(int)>& library_progress)
{
    if (color_result != nullptr)
        *color_result = {};
    if (published_out != nullptr)
        *published_out = false;
    if (config_loaded_out != nullptr)
        *config_loaded_out = false;

    std::vector<size_t> empty_result;
    bool dlg_cont = true;
    bool is_user_cancel = false;
    bool translate_old = false;
    bool invalid_bed_exclude_area = false;
    int current_width = 0, current_depth = 0;
    double current_height = 0;

    if (input_files.empty())
        return std::vector<size_t>();

    const bool previous_thumbnail_state = q->skip_thumbnail_invalid;
    ScopeGuard restore_thumbnail_state([this, previous_thumbnail_state]() {
        // In particular, parser errors and cancelled preparation must restore this flag.
        q->skip_thumbnail_invalid = previous_thumbnail_state;
    });

    if ((strategy & LoadStrategy::LoadModel) && (strategy & LoadStrategy::LoadConfig) && !(strategy & LoadStrategy::Restore)) {
        const bool has_3mf = std::any_of(input_files.begin(), input_files.end(), [](const fs::path& file) {
            return boost::algorithm::iends_with(file.string(), ".3mf");
        });
        const bool geometry_only = has_3mf && std::all_of(input_files.begin(), input_files.end(), [](const fs::path& file) {
            return !boost::algorithm::iends_with(file.string(), ".3mf") || is_geometry_only_3mf(file.string());
        });
        if (geometry_only) {
            // A plain CAD 3MF has no project to restore, even if its caller used default load flags.
            const int project_flags = static_cast<int>(LoadStrategy::LoadConfig) | static_cast<int>(LoadStrategy::LoadAuxiliary);
            strategy = static_cast<LoadStrategy>(static_cast<int>(strategy) & ~project_flags);
        }
    }
    // New import dialogs run before any project path, calibration or material changes.
    const bool stage_geometry = (strategy & LoadStrategy::LoadModel) && !(strategy & LoadStrategy::LoadConfig) &&
        (library_progress || std::any_of(input_files.begin(), input_files.end(), [](const fs::path& file) {
            return !geometry_import_format(file.string()).empty();
        }));


    // BBS
    int filaments_cnt = config->opt<ConfigOptionStrings>("filament_colour")->values.size();
    bool one_by_one = input_files.size() == 1 || printer_technology == ptSLA/* || filaments_cnt <= 1*/;
    if (! one_by_one) {
        for (const auto &path : input_files) {
            if (std::regex_match(path.string(), pattern_bundle)) {
                one_by_one = true;
                break;
            }
        }
    }

    bool load_model = strategy & LoadStrategy::LoadModel;
    bool load_config = strategy & LoadStrategy::LoadConfig;
    bool imperial_units = strategy & LoadStrategy::ImperialUnits;
    bool silence = strategy & LoadStrategy::Silence;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": load_model %1%, load_config %2%, input_files size %3%")%load_model %load_config %input_files.size();

    std::unique_ptr<ProgressDialog> load_dialog;
    if (!library_progress)
        load_dialog = std::make_unique<ProgressDialog>(_L("Loading") + dots, "", 100,
            find_toplevel_parent(q), wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_APP_MODAL);
    bool publishing_library_model = false;
    auto update_progress = [&](int value, const wxString& label = wxEmptyString) {
        if (load_dialog) return load_dialog->Update(value, label);
        if (!publishing_library_model) is_user_cancel = is_user_cancel || !library_progress(std::min(value, 98));
        return !is_user_cancel;
    };
    auto progress_was_cancelled = [&] { return is_user_cancel || (load_dialog && load_dialog->WasCancelled()); };
    auto report_error = [&](const wxString& message) {
        if (library_progress) BOOST_LOG_TRIVIAL(warning) << "Model library import: " << into_u8(message);
        else GUI::show_error(q, message);
    };
    wxBusyCursor busy;

    // Owned here so that early returns of the import preparation do not leak it.
    std::unique_ptr<Slic3r::Model> new_model_owner((!load_model || one_by_one) ? nullptr : new Slic3r::Model());
    auto *new_model = new_model_owner.get();
    std::vector<size_t> obj_idxs;

    std::string  designer_model_id;
    std::string  designer_country_code;

    int answer_convert_from_meters          = wxOK_DEFAULT;
    int answer_convert_from_imperial_units  = wxOK_DEFAULT;
    int tolal_model_count                   = 0;
    // Whether one of the files being loaded here carried a CAD recipe. A statement about these
    // files, not about the plater — q->model() may still hold the previous project's recipe.
    bool loaded_cad_recipe                  = false;

    int progress_percent = 0;
    int total_files = input_files.size();
    const int stage_percent[IMPORT_STAGE_MAX+1] = {
            5,      // IMPORT_STAGE_RESTORE
            10,     // IMPORT_STAGE_OPEN
            30,     // IMPORT_STAGE_READ_FILES
            50,     // IMPORT_STAGE_EXTRACT
            60,     // IMPORT_STAGE_LOADING_OBJECTS
            70,     // IMPORT_STAGE_LOADING_PLATES
            80,     // IMPORT_STAGE_FINISH
            85,     // IMPORT_STAGE_ADD_INSTANCE
            90,      // IMPORT_STAGE_UPDATE_GCODE
            92,     // IMPORT_STAGE_CHECK_MODE_GCODE
            95,     // UPDATE_GCODE_RESULT
            98,     // IMPORT_LOAD_CONFIG
            99,     // IMPORT_LOAD_MODEL_OBJECTS
            100
     };
    const int step_percent[LOAD_STEP_STAGE_NUM+1] = {
            5,     // LOAD_STEP_STAGE_READ_FILE
            30,     // LOAD_STEP_STAGE_GET_SOLID
            60,     // LOAD_STEP_STAGE_GET_MESH
            100
     };

    const float INPUT_FILES_RATIO            = 0.7;
    const float INIT_MODEL_RATIO             = 0.75;
    const float CENTER_AROUND_ORIGIN_RATIO   = 0.8;

    const float LOAD_MODEL_RATIO             = 0.9;

    struct StagedGeometryFile {
        // Reader outputs have one owner even when parsing throws or the user cancels.
        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
        PlateDataPtrs plates;
        std::vector<Preset*> presets;
        Semver version;
        En3mfType file_type = En3mfType::From_BBS;
        bool is_inches = false;
        std::string format;
        // Keep original names actually applied to this file, not only the remembered preference.
        bool names_kept = false;
        // A 3MF without project data; only its CAD part labels are offered as original names.
        bool cad_3mf = false;
        std::string designer_id;
        std::string country_code;
        ~StagedGeometryFile() {
            release_PlateData_list(plates);
            for (Preset* preset : presets)
                delete preset;
        }
    };
    std::vector<std::unique_ptr<StagedGeometryFile>> staged_files;
    std::map<std::string, GeometryImportOptions> import_options;
    GeometryImportNamePolicy name_policy = library_progress ? GeometryImportNamePolicy{} : geometry_import_name_policy(*wxGetApp().app_config);
    std::map<ObjectID, bool> import_display_options;
    // Applies the presentation choices; returns true when part settings keep a single object shown as an assembly.
    auto apply_import_presentation = [&import_display_options](ModelObject& object, bool single_object, bool sort) {
        const bool has_part_settings = object.has_individual_part_settings();
        // Never hide effective part-specific print controls in the single-object mode.
        object.imported_parts_hidden = single_object && object.volumes.size() > 1 && !has_part_settings;
        import_display_options[object.id()] = sort;
        return single_object && object.volumes.size() > 1 && has_part_settings;
    };
    // Staged files are read before the load loop: the reading takes this share of the bar and the loop the rest.
    const int staged_read_share = stage_geometry ? static_cast<int>(INPUT_FILES_RATIO * 100) : 0;
    auto loop_progress = [staged_read_share](int percent) { return staged_read_share + percent * (100 - staged_read_share) / 100; };
    if (stage_geometry) {
        staged_files.reserve(input_files.size());
        bool visible_part_settings = false;
        try {
            auto file_options = [&](const StagedGeometryFile& file) -> GeometryImportOptions* {
                return file.format.empty() ? nullptr : &import_options.at(file.format);
            };
            // Keep original names is applied by the readers, so every choice is made before a file is read.
            auto read_staged = [&](size_t index, StagedGeometryFile& target) {
                auto file_path = input_files[index];
                file_path.make_preferred();
                // Readers restart their count at each stage, so the bar keeps the furthest point of this file.
                int file_percent = 0;
                auto progress = [&](int percent) {
                    file_percent = std::max(file_percent, std::clamp(percent, 0, 100));
                    const int overall = static_cast<int>(staged_read_share * (static_cast<double>(index) + file_percent / 100.0) / input_files.size());
                    return update_progress(overall, format_wxstr(_L("Loading file: %s"), from_path(file_path.filename())));
                };
                if (!progress(0))
                    return false;
                GeometryImportOptions* options = file_options(target);
                LoadStrategy read_strategy = strategy;
                if (options != nullptr) {
                    // Assembly modes keep each group's bodies together as one object; the presentation is chosen separately.
                    if (!options->separate_objects)
                        read_strategy = read_strategy | LoadStrategy::AssembleParts;
                    // Project part names already win in a 3MF project, so only a CAD 3MF takes its labels.
                    if (options->keep_names && (target.format != "3mf" || target.cad_3mf))
                        read_strategy = read_strategy | LoadStrategy::KeepOriginalNames;
                }
                if (target.format == "step") {
                    target.model = Model::read_from_step(file_path.string(), read_strategy,
                        [&](int, int current, int total, bool& cancel) {
                            cancel = !progress(total > 0 ? current * 100 / total : 0);
                            is_user_cancel = is_user_cancel || cancel;
                        }, nullptr,
                        [&](Step& file, double& linear, double& angle, bool& split) {
                            if (!options->chosen) {
                                if (options->show_dialog) {
                                    StepMeshDialog mesh_dialog(q, file, options->linear_deflection, options->angle_deflection, options);
                                    if (mesh_dialog.ShowModal() != wxID_OK) {
                                        is_user_cancel = true;
                                        return -1;
                                    }
                                }
                                options->chosen = true;
                            }
                            linear = options->linear_deflection;
                            angle = options->angle_deflection;
                            split = options->split_compound;
                            // Names of bodies merged by an unsplit compound cannot be kept, so that file gets Orca's names.
                            file.set_keep_original_names(options->keep_names && file.has_original_names() &&
                                                         (split || !file.has_multi_body_compound()));
                            target.names_kept = file.keep_original_names();
                            return 1;
                        }, options->linear_deflection, options->angle_deflection, options->split_compound);
                } else if (target.format == "3mf") {
                    target.model = Model::read_from_archive(file_path.string(), &target.config, &target.substitutions,
                        target.file_type, read_strategy, &target.plates, &target.presets, &target.version,
                        [&](int, int current, int total, bool& cancel) {
                            cancel = !progress(total > 0 ? current * 100 / total : 0);
                            is_user_cancel = is_user_cancel || cancel;
                        });
                } else {
                    target.model = Model::read_from_file(file_path.string(), nullptr, nullptr, read_strategy,
                        &target.plates, &target.presets, &target.is_inches, &target.version, nullptr,
                        [&](int current, int total, bool& cancel, std::string& designer, std::string& country) {
                            target.designer_id = designer;
                            target.country_code = country;
                            cancel = !progress(total > 0 ? current * 100 / total : 0);
                            is_user_cancel = is_user_cancel || cancel;
                        });
                }
                return !progress_was_cancelled();
            };

            for (size_t index = 0; index < input_files.size(); ++index) {
                auto staged = std::make_unique<StagedGeometryFile>();
                staged->format = geometry_import_format(input_files[index].string());
                if (!staged->format.empty()) {
                    auto inserted = import_options.emplace(staged->format, library_progress ?
                        GeometryImportOptions{} : geometry_import_options(*wxGetApp().app_config, staged->format));
                    if (silence)
                        inserted.first->second.show_dialog = false;
                }
                staged->cad_3mf = staged->format == "3mf" && is_geometry_only_3mf(input_files[index].string());
                // One AMF/3MF dialog per format, at its first file; a 3MF project has nothing to choose.
                // The STEP dialog runs inside the reader, which needs the document for its triangle count.
                if ((staged->format == "amf" || staged->cad_3mf) &&
                    !show_geometry_import_options(q, staged->format, import_options.at(staged->format)))
                    return empty_result;
                if (!read_staged(index, *staged))
                    return empty_result;
                staged_files.push_back(std::move(staged));
            }

            for (const auto& staged : staged_files) {
                const GeometryImportOptions* options = file_options(*staged);
                if (options == nullptr)
                    continue;
                if (staged->format != "step")
                    // A 3MF project keeps its own part names; only a CAD 3MF takes the labels from the file.
                    staged->names_kept = options->keep_names && (staged->format != "3mf" || staged->cad_3mf);
                // Orca's own names carry no order worth sorting by.
                for (ModelObject* object : staged->model.objects)
                    visible_part_settings = apply_import_presentation(*object, options->single_object(),
                                                                      options->keep_names && options->sort_names) || visible_part_settings;
            }
            std::vector<Model*> named_models;
            for (const auto& staged : staged_files)
                if (staged->names_kept)
                    named_models.push_back(&staged->model);
            if (silence)
                name_policy.show_dialog = false;
            if (!prepare_geometry_import_names(q, named_models, name_policy))
                return empty_result;
            if (visible_part_settings && !silence)
                MessageDialog(q, _L("Some imported objects contain modifiers or parts with individual settings. They are shown as assemblies so that these settings remain visible."),
                              _L("Geometry import"), wxOK | wxICON_INFORMATION).ShowModal();
        } catch (const std::exception& error) {
            if (!progress_was_cancelled())
                report_error(from_u8(error.what()));
            return empty_result;
        }
    }

    // The new import dialogs and the staged reading are done: a deferred project replacement starts here.
    if (before_publish) {
        // A deferred geometry import must finish reading before changing the plate.
        assert(stage_geometry);
        // Replacing the project needs something to load; objects below this volume are dropped while loading,
        // as in Model::removed_objects_with_zero_volume().
        const bool has_geometry = std::any_of(staged_files.begin(), staged_files.end(), [](const auto& staged) {
            return std::any_of(staged->model.objects.begin(), staged->model.objects.end(), [](const ModelObject* object) {
                return object->get_object_stl_stats().volume >= 0.0000000001;
            });
        });
        if (!has_geometry) {
            report_error(_L("The file does not contain any geometry data."));
            return empty_result;
        }
        if (library_progress) {
            if (!library_progress(99)) return empty_result;
            publishing_library_model = true;
        }
        before_publish();
    }
    q->m_3mf_path = input_files.front().string();
    // Leave the previous calibration mode intact when preparation was cancelled.
    background_process.fff_print()->calib_mode() = CalibMode::Calib_None;

    for (size_t i = 0; i < input_files.size(); ++i) {
        int project_filament_count = 1;

#ifdef _WIN32
        auto path = input_files[i];
        // On Windows, we swap slashes to back slashes, see GH #6803 as read_from_file() does not understand slashes on Windows thus it assignes full path to names of loaded objects.
        path.make_preferred();
#else  // _WIN32
       // Don't make a copy on Posix. Slash is a path separator, back slashes are not accepted as a substitute.
        const auto &path = input_files[i];
#endif // _WIN32
        const auto filename         = path.filename();
        int  progress_percent = static_cast<int>(100.0f * static_cast<float>(i) / static_cast<float>(input_files.size()));
        const auto real_filename    = (strategy & LoadStrategy::Restore) ? input_files[++i].filename() : filename;
        const auto dlg_info         = _L("Loading file") + ": " + from_path(real_filename);
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << boost::format(": load file %1%") % filename;
        dlg_cont = update_progress(loop_progress(progress_percent), dlg_info);
        if (!dlg_cont) return empty_result;

        const bool type_3mf = std::regex_match(path.string(), pattern_3mf);
        // const bool type_zip_amf = !type_3mf && std::regex_match(path.string(), pattern_zip_amf);
        const bool type_any_amf = !type_3mf && std::regex_match(path.string(), pattern_any_amf);
        // const bool type_prusa   = std::regex_match(path.string(), pattern_prusa);

        Slic3r::Model model;
        StagedGeometryFile legacy_read;
        StagedGeometryFile& parsed_file = stage_geometry ? *staged_files[i] : legacy_read;
        // BBS: add auxiliary files related logic
        bool load_aux = strategy & LoadStrategy::LoadAuxiliary, load_old_project = false;
        if (load_model && load_config && type_3mf) {
            load_aux = true;
            strategy = strategy | LoadStrategy::LoadAuxiliary;
        }
        if (load_config) strategy = strategy | LoadStrategy::CheckVersion;
        bool is_project_file = false;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": is_project_file %1%, type_3mf %2%") % is_project_file % type_3mf;
        try {
            if (type_3mf) {
                DynamicPrintConfig config;
                Semver&            file_version = parsed_file.version;
                En3mfType&         en_3mf_file_type = parsed_file.file_type;
                // BBS: a "published" 3MF carries a flag plus the author-selected setting keys;
                // on load keep the user's current presets and overlay only those keys. Declared
                // here (outside the config block below) so it stays alive for the embedded-preset
                // gate, the metadata strip and the preset overlay after the block closes.
                PublishedConfig published_config;
                {
                    DynamicPrintConfig& config_loaded = parsed_file.config;

                    // Reader outputs remain owned through every downstream early return.
                    PlateDataPtrs& plate_data = parsed_file.plates;
                    ConfigSubstitutionContext& config_substitutions = parsed_file.substitutions;
                    std::vector<Preset*>& project_presets = parsed_file.presets;
                    // BBS: backup & restore
                    q->skip_thumbnail_invalid = true;
                    if (stage_geometry) {
                        model = std::move(parsed_file.model);
                    } else {
                        model = Slic3r::Model::read_from_archive(path.string(), &config_loaded, &config_substitutions, en_3mf_file_type, strategy, &plate_data, &project_presets,
                                                             &file_version,
                                                             [&update_progress, real_filename, &progress_percent, stage_percent, INPUT_FILES_RATIO, total_files, i,
                                                              &is_user_cancel](int import_stage, int current, int total, bool &cancel) {
                                                                 bool     cont = true;
                                                                 float percent_float = (100.0f * (float)i / (float)total_files) + INPUT_FILES_RATIO * ((float)stage_percent[import_stage] + (float)current * (float)(stage_percent[import_stage + 1] - stage_percent[import_stage]) /(float) total) / (float)total_files;
                                                                 BOOST_LOG_TRIVIAL(trace) << "load_3mf_file: percent(float)=" << percent_float << ", stage = " << import_stage << ", curr = " << current << ", total = " << total;
                                                                 progress_percent = (int)percent_float;
                                                                 wxString msg  = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                                                                 cont          = update_progress(progress_percent, msg);
                                                                 cancel        = !cont;
                                                                 if (cancel)
                                                                     is_user_cancel = cancel;
                                                             });
                    }
                          BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__
                                      << boost::format(", plate_data.size %1%, project_preset.size %2%, is_bbs_or_orca_3mf %3%, file_version %4% \n") % plate_data.size() %
                                          project_presets.size() % (en_3mf_file_type == En3mfType::From_BBS || en_3mf_file_type == En3mfType::From_Orca) % file_version.to_string();

                    // BBS: a "published" 3MF carries a flag plus the author-selected setting keys;
                    // on load keep the user's current presets and overlay only those keys. Parsed
                    // here (before the version/fallback chain below) because a published file has
                    // no project_settings.config: its values travel in the published_config
                    // metadata payload, which must fill config_loaded before the chain decides
                    // whether to import geometry only.
                    if (model.model_info != nullptr) {
                        auto published_it = model.model_info->metadata_items.find(ORCA_PUBLISHED_TAG);
                        if (published_it != model.model_info->metadata_items.end() && is_published_3mf_flag(published_it->second)) {
                            published_config.published = true;
                            auto keys_it               = model.model_info->metadata_items.find(ORCA_PUBLISHED_KEYS_TAG);
                            if (keys_it != model.model_info->metadata_items.end()) {
                                try {
                                    auto j = nlohmann::json::parse(keys_it->second);
                                    if (j.is_array())
                                        for (const auto& k : j)
                                            if (k.is_string())
                                                published_config.published_keys.emplace_back(k.get<std::string>());
                                } catch (...) {
                                    // Ignore malformed published_keys; the project still loads normally.
                                }
                            }

                            auto material_keys_it = model.model_info->metadata_items.find(ORCA_PUBLISHED_MATERIAL_TAG);
                            if (material_keys_it != model.model_info->metadata_items.end()) {
                                try {
                                    auto jm = nlohmann::json::parse(material_keys_it->second);
                                    if (jm.is_array())
                                        for (const auto& m : jm) {
                                            try {
                                                // Malformed entries are isolated so one bad item
                                                // cannot discard valid entries that follow it.
                                                if (!m.is_object())
                                                    continue;
                                                PublishedMaterialEntry entry;
                                                const auto mat_it = m.find("material");
                                                if (mat_it != m.end() && mat_it->is_object()) {
                                                    const auto& mat = *mat_it;
                                                    if (mat.contains("filament_type") && mat["filament_type"].is_string())
                                                        entry.filament_type = mat["filament_type"].get<std::string>();
                                                    if (mat.contains("filament_vendor") && mat["filament_vendor"].is_string())
                                                        entry.filament_vendor = mat["filament_vendor"].get<std::string>();
                                                    if (mat.contains("filament_id") && mat["filament_id"].is_string())
                                                        entry.filament_id = mat["filament_id"].get<std::string>();
                                                    if (mat.contains("setting_id") && mat["setting_id"].is_string())
                                                        entry.setting_id = mat["setting_id"].get<std::string>();
                                                    if (mat.contains("name") && mat["name"].is_string())
                                                        entry.preset_name = mat["name"].get<std::string>();
                                                }
                                                if (m.contains("slot") && m["slot"].is_number_integer())
                                                    entry.slot = m["slot"].get<int>();
                                                const auto entry_keys_it = m.find("keys");
                                                if (entry_keys_it != m.end() && entry_keys_it->is_array())
                                                    for (const auto& k : *entry_keys_it)
                                                        if (k.is_string())
                                                            entry.keys.emplace_back(k.get<std::string>());
                                                // Fields always written by the current exporter.
                                                if (m.contains("full") && m["full"].is_boolean())
                                                    entry.full = m["full"].get<bool>();
                                                const auto entry_full_keys_it = m.find("full_keys");
                                                if (entry_full_keys_it != m.end() && entry_full_keys_it->is_array())
                                                    for (const auto& k : *entry_full_keys_it)
                                                        if (k.is_string())
                                                            entry.full_keys.emplace_back(k.get<std::string>());
                                                if (m.contains("publish_type") && m["publish_type"].is_boolean())
                                                    entry.publish_type = m["publish_type"].get<bool>();
                                                if (m.contains("type") && m["type"].is_string())
                                                    entry.publish_type_value = m["type"].get<std::string>();
                                                if (m.contains("publish_color") && m["publish_color"].is_boolean())
                                                    entry.publish_color = m["publish_color"].get<bool>();
                                                if (m.contains("color") && m["color"].is_string())
                                                    entry.color = m["color"].get<std::string>();
                                                published_config.material_keys.emplace_back(std::move(entry));
                                            } catch (const nlohmann::json::exception&) {
                                                // Ignore only this malformed material entry.
                                            }
                                        }
                                } catch (const nlohmann::json::exception&) {
                                    // Ignore malformed published_material_keys; the project still loads normally.
                                }
                            }

                            auto layout_it = model.model_info->metadata_items.find(ORCA_PUBLISHED_LAYOUT_TAG);
                            if (layout_it != model.model_info->metadata_items.end()) {
                                try {
                                    auto jl = nlohmann::json::parse(layout_it->second);
                                    PublishedExtruderLayout layout;
                                    layout.extruder_ids = jl.at("extruder_ids").get<std::vector<int>>();
                                    layout.variants     = jl.at("variants").get<std::vector<std::string>>();
                                    published_config.printer_layout = std::move(layout);
                                    if (jl.contains("print_extruder_ids") && jl.contains("print_variants")) {
                                        PublishedExtruderLayout print_layout;
                                        print_layout.extruder_ids = jl.at("print_extruder_ids").get<std::vector<int>>();
                                        print_layout.variants     = jl.at("print_variants").get<std::vector<std::string>>();
                                        published_config.print_layout = std::move(print_layout);
                                    }
                                } catch (const nlohmann::json::exception&) {
                                    // Malformed layout: the keys without a parsed layout fall back to the positional import.
                                }
                            }

                            // Rebuild the published values from the metadata payload: a published
                            // file carries no project_settings.config, so config_loaded is filled
                            // from here; a missing or malformed payload leaves it empty and the
                            // fallback chain below imports the geometry only.
                            auto payload_it = model.model_info->metadata_items.find(ORCA_PUBLISHED_CONFIG_TAG);
                            if (payload_it != model.model_info->metadata_items.end()) {
                                try {
                                    ConfigSubstitutions payload_substitutions =
                                        config_loaded.load_from_ini_string(payload_it->second, ForwardCompatibilitySubstitutionRule::Enable);
                                    config_substitutions.substitutions.insert(config_substitutions.substitutions.end(),
                                                                              std::make_move_iterator(payload_substitutions.begin()),
                                                                              std::make_move_iterator(payload_substitutions.end()));
                                } catch (...) {
                                    // Ignore malformed published_config; the project still loads normally.
                                }
                            }
                        }
                    }

                    std::string import_project_action = wxGetApp().app_config->get("import_project_action");
                    LoadType load_type;
                    if (import_project_action.empty())
                        load_type = LoadType::Unknown;
                    else
                        load_type  = static_cast<LoadType>(std::stoi(import_project_action));

                    // BBS: version check
                    Semver app_version = *(Semver::parse(SoftFever_VERSION));
                    const wxString load_3mf_title              = _L("Load 3MF");
                    const wxString newer_3mf_title             = _L("Newer 3MF version");
                    const wxString msg_unsupported_geometry    = _L("The 3MF is not supported by OrcaSlicer, loading geometry data only.");
                    const wxString msg_old_orca_geometry       = _L("The 3MF file was generated by an old OrcaSlicer version, loading geometry data only.");
                    const wxString msg_older_geometry          = _L("The 3MF file was generated by an older version, loading geometry data only.");
                    const wxString msg_bambu_geometry          = _L("The 3MF file was generated by BambuStudio, loading geometry data only.");
                    const wxString msg_no_project_settings     = _L("The 3MF file has no project settings that OrcaSlicer can import.");
                    auto log_and_show_3mf_info = [&](const wxString& text, const wxString& title) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__
                                                << " "
                                                << boost::format("3MF import message [%1%]: %2% | file: %3%") % into_u8(title) % into_u8(text) % path.string();
                        if (!library_progress) show_info(q, text, title);
                    };
                    auto skip_project_config = [&](const wxString& geometry_msg) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__
                                                << boost::format(" 3MF project config not loaded, importing geometry only: %1%") % path.string();
                        load_config = false;
                        if (!load_model)
                            log_and_show_3mf_info(msg_no_project_settings, load_3mf_title);
                        else if (!geometry_msg.empty())
                            log_and_show_3mf_info(geometry_msg, load_3mf_title);
                    };
                    auto migrate_2_3_1_alpha_rotate_template = [&]() {
                        auto* rotate_template = config_loaded.option<ConfigOptionString>("sparse_infill_rotate_template");
                        if (rotate_template == nullptr || rotate_template->value.empty())
                            return;
                        const auto* pattern_opt = config_loaded.option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern");
                        const auto _sparse_infill_pattern =
                            pattern_opt != nullptr ? pattern_opt->value : FullPrintConfig::defaults().sparse_infill_pattern.value;
                        bool is_safe_to_rotate = _sparse_infill_pattern == ipRectilinear || _sparse_infill_pattern == ipLine ||
                                                 _sparse_infill_pattern == ipZigZag || _sparse_infill_pattern == ipCrossZag ||
                                                 _sparse_infill_pattern == ipLockedZag;
                        if (!is_safe_to_rotate) {
                            wxString msg_text = _(
                                L("This project was created with an OrcaSlicer 2.3.1-alpha and uses "
                                  "infill rotation template settings that may not work properly with your current infill pattern. "
                                  "This could result in weak support or print quality issues."));
                            msg_text += "\n\n" +
                                        _(L("Would you like OrcaSlicer to automatically fix this by clearing the rotation template settings?"));
                            MessageDialog dialog(wxGetApp().plater(), msg_text, "", wxICON_WARNING | wxYES | wxNO);
                            dialog.SetButtonLabel(wxID_YES, _L("Yes"));
                            dialog.SetButtonLabel(wxID_NO, _L("No"));
                            if (dialog.ShowModal() == wxID_YES) {
                                rotate_template->value.clear();
                            }
                        }
                    };
                    // Untagged files up to 2.3.2 may also come from OrcaSlicer, which only started tagging its 3MFs after it.
                    const bool is_bambu_studio_project = en_3mf_file_type == En3mfType::From_BBS && file_version > Semver(2, 3, 2);
                    if (en_3mf_file_type == En3mfType::From_Prusa) {
                        // do not reset the model config
                        skip_project_config(load_type != LoadType::LoadGeometry ? msg_unsupported_geometry : wxString());
                    }
                    else if (en_3mf_file_type == En3mfType::From_Orca) {
                        // OrcaSlicer file (has OrcaSlicer tag) - compare file_version with SoftFever_VERSION
                        if (load_config && !config_loaded.empty() && (file_version < app_version) && file_version == Semver("2.3.1-alpha")) {
                            migrate_2_3_1_alpha_rotate_template();
                        } else if (load_config && (file_version > app_version)) {
                            if (config_substitutions.unrecogized_keys.size() > 0) {
                                wxString text  = wxString::Format(_L("The 3MF file version %s is newer than %s's version %s, found the following unrecognized keys:"),
                                                                 file_version.to_string_sf(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string_sf());
                                text += "\n" + from_u8(boost::algorithm::join(config_substitutions.unrecogized_keys, ", ")) + "\n\n";
                                text += _L("You should update your software.\n");
                                log_and_show_3mf_info(text, newer_3mf_title);
                            }
                            else {
                                //if the minor version is not matched
                                if (file_version.min() != app_version.min()) {
                                    wxString text  = wxString::Format(_L("The 3MF file version %s is newer than %s's version %s, we suggest to upgrade your software."),
                                                     file_version.to_string_sf(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string_sf());
                                    text += "\n";
                                    log_and_show_3mf_info(text, newer_3mf_title);
                                }
                            }
                            if (config_loaded.empty())
                                skip_project_config(wxString());
                        }
                        else if (load_config && config_loaded.empty()) {
                            skip_project_config(file_version < app_version ? msg_old_orca_geometry : wxString());
                        }
                    }
                    else if (en_3mf_file_type == En3mfType::From_BBS) {
                        // No OrcaSlicer tag - check Bambu/Application version
                        if (!is_bambu_studio_project) {
                            // Compatible old version (before OrcaSlicer tagging was introduced after 2.3.2).
                            // Any version prior or equal to 2.3.2 is older than the current one, no version warnings needed.
                            // Still apply migration fixes for known old versions.
                            if (load_config && !config_loaded.empty() && (file_version == Semver("2.3.1-alpha"))) {
                                migrate_2_3_1_alpha_rotate_template();
                            }
                            else if (load_config && config_loaded.empty()) {
                                skip_project_config(msg_older_geometry);
                            }
                        } else {
                            // BambuStudio project (version > 2.3.2 without OrcaSlicer tag)
                            if (load_config && config_loaded.empty()) {
                                skip_project_config(msg_bambu_geometry);
                            }
                            else if (load_config) {
                                // Logged, not shown: it is the same for every BambuStudio project and needs no action.
                                const std::string unrecognized = boost::algorithm::join(config_substitutions.unrecogized_keys, ", ");
                                BOOST_LOG_TRIVIAL(info) << "BambuStudio " << file_version.to_string() << " project " << path.string()
                                                        << ", unrecognized settings: " << (unrecognized.empty() ? "none" : unrecognized);
                            }
                        }
                    }
                    else if (en_3mf_file_type == En3mfType::From_Other) {
                        // Generic CAD/other 3MF without slicer metadata: import geometry silently.
                        if (load_config && config_loaded.empty()) {
                            skip_project_config(wxString());
                        }
                    }

                    // 1. add extruder for prusa model if the number of existing extruders is not enough
                    // 2. add extruder for BBS or Other model if only import geometry, by choice or by the checks above
                    if (en_3mf_file_type == En3mfType::From_Prusa || (load_model && !load_config)) {
                        std::set<int> extruderIds;
                        for (ModelObject *o : model.objects) {
                            if (o->config.option("extruder")) extruderIds.insert(o->config.extruder());
                            for (auto volume : o->volumes) {
                                if (volume->config.option("extruder")) extruderIds.insert(volume->config.extruder());
                                for (int extruder : volume->get_extruders()) { extruderIds.insert(extruder); }
                            }
                        }
                        int size = extruderIds.size() == 0 ? 0 : *(extruderIds.rbegin());

                        int filament_size = sidebar->combos_filament().size();
                        while (filament_size < MAXIMUM_EXTRUDER_NUMBER && filament_size < size) {
                            int         filament_count = filament_size + 1;
                            wxColour    new_col        = Plater::get_next_color_for_filament();
                            std::string new_color      = new_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
                            wxGetApp().preset_bundle->set_num_filaments(filament_count, new_color);
                            wxGetApp().plater()->on_filament_count_change(filament_count);
                            ++filament_size;
                        }
                        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
                    }

                    // ORCA: legacy feature-filament default migration (1 -> 0) is now handled
                    // uniformly in PrintConfigDef::handle_legacy() via the old->new key rename
                    // (wall_filament -> wall_filament_id, etc.), which also covers saved presets.

                    // plate data
                    if (plate_data.size() > 0) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", import 3mf UPDATE_GCODE_RESULT \n");
                        wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                        dlg_cont     = update_progress(loop_progress(progress_percent), msg);
                        if (!dlg_cont) {
                            q->skip_thumbnail_invalid = false;
                            return empty_result;
                        }

                        Semver old_version(1, 5, 9);
                        // A published 3MF has no project config to migrate: skip the old-version
                        // translations even if a slicer tag slipped through classification.
                        if ((en_3mf_file_type == En3mfType::From_BBS || en_3mf_file_type == En3mfType::From_Orca) && (file_version < old_version) && !published_config.published && load_model && load_config && !config_loaded.empty()) {
                            translate_old = true;
                            partplate_list.get_plate_size(current_width, current_depth, current_height);
                        }

                        if (load_config) {
                            if (translate_old) {
                                //set the size back
                                partplate_list.reset_size(current_width + Bed3D::Axes::DefaultTipRadius, current_depth + Bed3D::Axes::DefaultTipRadius, current_height, false);
                            }
                            if (const auto* colours_opt = config_loaded.option<ConfigOptionStrings>("filament_colour"))
                                project_filament_count = colours_opt->size();
                            partplate_list.load_from_3mf_structure(plate_data, project_filament_count);
                            partplate_list.update_slice_context_to_current_plate(background_process);
                            this->preview->update_gcode_result(partplate_list.get_current_slice_result());
                            release_PlateData_list(plate_data);
                            sidebar->obj_list()->reload_all_plates();
                            q->suppress_background_process(true);
                        } else {
                            partplate_list.reload_all_objects();
                        }
                    }

                    // BBS:: project embedded presets (skipped for published projects: the author's
                    // embedded presets must not pollute the receiver's library, the overlay applies
                    // the published keys to the receiver's own presets instead).
                    if ((project_presets.size() > 0) && load_config && !published_config.published) {
                        // load project embedded presets
                        PresetsConfigSubstitutions preset_substitutions;
                        PresetBundle &             preset_bundle = *wxGetApp().preset_bundle;
                        preset_substitutions                     = preset_bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::Enable);
                        if (is_bambu_studio_project)
                            log_substitutions(preset_substitutions, path.string());
                        else if (!preset_substitutions.empty())
                            show_substitutions_info(preset_substitutions);
                    }
                    if (project_presets.size() > 0) {
                        for (unsigned int i = 0; i < project_presets.size(); i++) { delete project_presets[i]; }
                        project_presets.clear();
                    }

                    if (load_config && !config_loaded.empty()) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", import 3mf IMPORT_LOAD_CONFIG \n");
                        wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                        dlg_cont     = update_progress(loop_progress(progress_percent), msg);
                        if (!dlg_cont) {
                            q->skip_thumbnail_invalid = false;
                            return empty_result;
                        }

                        // Based on the printer technology field found in the loaded config, select the base for the config,
                        PrinterTechnology printer_technology = Preset::printer_technology(config_loaded);

                        config.apply(static_cast<const ConfigBase &>(FullPrintConfig::defaults()));
                        // and place the loaded config over the base.
                        config += std::move(config_loaded);
                        if (ConfigOptionPoints *bed_exclude_area = config.option<ConfigOptionPoints>("bed_exclude_area");
                            bed_exclude_area != nullptr && bed_exclude_area->values.size() > 1 &&
                            !is_valid_bed_exclude_area(bed_exclude_area->values)) {
                            bed_exclude_area->values.clear();
                            invalid_bed_exclude_area = true;
                        }
                        std::map<std::string, std::string> validity = config.validate();
                        if (!validity.empty()) {
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << " " << boost::format("Param values in 3mf error: ");
                            for (std::map<std::string, std::string>::iterator it=validity.begin(); it!=validity.end(); ++it)
                                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << " " << boost::format("%1%: %2%")%it->first %it->second;
                            //
                            NotificationManager *notify_manager = q->get_notification_manager();
                            std::string error_message = _u8L("Invalid values found in the 3MF:");
                            error_message += "\n";
                            for (std::map<std::string, std::string>::iterator it=validity.begin(); it!=validity.end(); ++it)
                                error_message += "-" + it->first + ": " + it->second + "\n";
                            error_message += "\n";
                            error_message += _u8L("Please correct them in the Param tabs");
                            notify_manager->bbl_show_3mf_warn_notification(error_message);
                        }
                    }
                    // BambuStudio projects routinely carry values Orca replaces; log them rather than showing a dialog on every open.
                    if (is_bambu_studio_project)
                        log_substitutions(config_substitutions.substitutions, path.string());
                    else if (!config_substitutions.empty())
                        show_substitutions_info(config_substitutions.substitutions, filename.string());

                    // BBS
                    if (load_model && !load_config) {
                        ;
                    }
                    else {
                        this->model.plates_custom_gcodes = model.plates_custom_gcodes;
                        this->model.design_info = model.design_info;
                        this->model.model_info = model.model_info;
                    }
                }

                // BBS: a "published" 3MF loads as a new project: its path must not become the
                // project filename (Save/Ctrl-S would overwrite the shared file), and the
                // published metadata is consumed above and stripped so a later save is a normal
                // unpublished 3MF.
                if (published_out != nullptr && published_config.published)
                    *published_out = true;
                if (published_config.published && load_config && this->model.model_info != nullptr) {
                    this->model.model_info->metadata_items.erase(ORCA_PUBLISHED_TAG);
                    this->model.model_info->metadata_items.erase(ORCA_PUBLISHED_KEYS_TAG);
                    this->model.model_info->metadata_items.erase(ORCA_PUBLISHED_MATERIAL_TAG);
                    this->model.model_info->metadata_items.erase(ORCA_PUBLISHED_CONFIG_TAG);
                    this->model.model_info->metadata_items.erase(ORCA_PUBLISHED_LAYOUT_TAG);
                }

                if (load_config) {
                    if (!config.empty()) {
                        Preset::normalize(config);
                        PresetBundle *preset_bundle = wxGetApp().preset_bundle;

                        {
                            // BBS: modify the prime tower params for old version file
                            Semver old_version3(2, 0, 0);
                            if ((en_3mf_file_type == En3mfType::From_BBS || en_3mf_file_type == En3mfType::From_Orca) && !published_config.published && file_version < old_version3) {
                                double old_filament_prime_volume = 0.;
                                int    filament_count            = 0;
                                {
                                    ConfigOptionFloats  *filament_prime_volume_option = config.option<ConfigOptionFloats>("filament_prime_volume");
                                    ConfigOptionStrings *filament_colors_option       = config.option<ConfigOptionStrings>("filament_colour", true);
                                    filament_count                                    = filament_colors_option->values.size();
                                    if (filament_prime_volume_option) {
                                        std::vector<double> &filament_prime_volume_values = filament_prime_volume_option->values;
                                        if (!filament_prime_volume_values.empty()) {
                                            old_filament_prime_volume = filament_prime_volume_values[0];
                                            if (filament_count > 1) filament_prime_volume_values.resize(filament_count, old_filament_prime_volume);
                                        }
                                    }
                                }
                                ConfigOptionEnum<WipeTowerWallType> *prime_tower_rib_wall_option = config.option<ConfigOptionEnum<WipeTowerWallType>>("wipe_tower_wall_type", true);
                                prime_tower_rib_wall_option->value            = WipeTowerWallType::wtwRectangle;

                                ConfigOptionPercent *prime_tower_infill_gap_option = config.option<ConfigOptionPercent>("prime_tower_infill_gap", true);
                                prime_tower_infill_gap_option->value               = 100;

                                ConfigOptionInts *filament_adhesiveness_category_option = config.option<ConfigOptionInts>("filament_adhesiveness_category", true);
                                std::vector<int> &filament_adhesiveness_category_values = filament_adhesiveness_category_option->values;
                                filament_adhesiveness_category_values.resize(filament_count);
                                for (int index = 0; index < filament_count; index++)
                                    filament_adhesiveness_category_values[index] = 100;

                                std::vector<std::string> &diff_settings = config.option<ConfigOptionStrings>("different_settings_to_system", true)->values;
                                diff_settings.resize(filament_count + 2);

                                std::vector<std::string> diff_process_keys;
                                std::string              diff_process_settings = diff_settings[0];
                                Slic3r::unescape_strings_cstyle(diff_process_settings, diff_process_keys);
                                diff_process_keys.emplace_back("wipe_tower_wall_type");
                                diff_process_keys.emplace_back("prime_tower_infill_gap");
                                diff_process_settings = Slic3r::escape_strings_cstyle(diff_process_keys);
                                diff_settings[0] = diff_process_settings;

                                for (int index = 0; index < filament_count; index++) {
                                    std::vector<std::string> diff_filament_keys;
                                    std::string              diff_filament_settings = diff_settings[index + 1];
                                    Slic3r::unescape_strings_cstyle(diff_filament_settings, diff_filament_keys);
                                    diff_filament_keys.emplace_back("filament_adhesiveness_category");
                                    diff_filament_settings   = Slic3r::escape_strings_cstyle(diff_filament_keys);
                                    diff_settings[index + 1] = diff_filament_settings;
                                }
                            }
                        }

                        auto choise = wxGetApp().app_config->get("no_warn_when_modified_gcodes");
                        if (!published_config.published && (choise.empty() || choise != "true")) {
                            // BBS: first validate the printer
                            // validate the system profiles
                            std::set<std::string> modified_gcodes;
                            int validated = preset_bundle->validate_presets(filename.string(), config, modified_gcodes);
                            if (validated == VALIDATE_PRESETS_MODIFIED_GCODES) {
                                std::string warning_message;
                                warning_message += "\n";
                                for (std::set<std::string>::iterator it=modified_gcodes.begin(); it!=modified_gcodes.end(); ++it)
                                    warning_message += "-" + *it + "\n";
                                warning_message += "\n";
                                //show_info(q, _L("The 3MF has the following modified G-code in filament or printer presets:") + warning_message + _L("Please confirm that all modified G-code is safe to prevent any damage to the machine!"), _L("Modified G-code"));
                                MessageDialog dlg(q, _L("The 3MF has the following modified G-code in filament or printer presets:") + warning_message + _L("Please confirm that all modified G-code is safe to prevent any damage to the machine!"), _L("Modified G-code"));
                                dlg.show_dsa_button();
                                auto  res = dlg.ShowModal();
                                if (dlg.get_checkbox_state())
                                    wxGetApp().app_config->set("no_warn_when_modified_gcodes", "true");
                            }
                            else if ((validated == VALIDATE_PRESETS_PRINTER_NOT_FOUND) || (validated == VALIDATE_PRESETS_FILAMENTS_NOT_FOUND)) {
                                std::string warning_message;
                                warning_message += "\n";
                                for (std::set<std::string>::iterator it=modified_gcodes.begin(); it!=modified_gcodes.end(); ++it)
                                    warning_message += "-" + *it + "\n";
                                warning_message += "\n";
                                //show_info(q, _L("The 3MF has the following customized filament or printer presets:") + warning_message + _L("Please confirm that the G-code within these presets is safe to prevent any damage to the machine!"), _L("Customized Preset"));
                                MessageDialog dlg(q, _L("The 3MF has the following customized filament or printer presets:") + from_u8(warning_message)+ _L("Please confirm that the G-code within these presets is safe to prevent any damage to the machine!"), _L("Customized Preset"));
                                dlg.show_dsa_button();
                                auto  res = dlg.ShowModal();
                                if (dlg.get_checkbox_state())
                                    wxGetApp().app_config->set("no_warn_when_modified_gcodes", "true");
                            }
                        }

                        //always load config
                        {
                            // BBS: save the wipe tower pos in file here, will be used later
                            ConfigOptionFloats* wipe_tower_x_opt = config.opt<ConfigOptionFloats>("wipe_tower_x");
                            ConfigOptionFloats* wipe_tower_y_opt = config.opt<ConfigOptionFloats>("wipe_tower_y");
                            std::optional<ConfigOptionFloats>file_wipe_tower_x;
                            std::optional<ConfigOptionFloats>file_wipe_tower_y;
                            if (wipe_tower_x_opt)
                                file_wipe_tower_x = *wipe_tower_x_opt;
                            if (wipe_tower_y_opt)
                                file_wipe_tower_y = *wipe_tower_y_opt;

                            // Convert the printer's filament ids to Orca ids before loading.
                            if (auto* agent = wxGetApp().getAgent()) {
                                if (auto* ids = config.opt<ConfigOptionStrings>("filament_ids"))
                                    for (std::string& id : ids->values)
                                        id = agent->to_orca_filament_id(id);
                            }
                            preset_bundle->load_config_model(filename.string(), std::move(config), file_version, &published_config);
                            if (config_loaded_out != nullptr)
                                *config_loaded_out = true;

                            // Mixed-filament definitions that collided with one of the
                            // receiver's real slots were relocated during the preset load.
                            // Re-point the freshly parsed model's extruder references and
                            // color painting from the author's slot numbers to where each
                            // definition landed, so volumes colored with a mix follow it.
                            // Runs before the objects are handed over to the plater below.
                            if (load_model && !published_config.mixed_slot_relocations.empty())
                                Slic3r::remap_model_filament_slots(model, published_config.mixed_slot_relocations);

                            // BBS: notify the user about published settings that could not be applied.
                            if (!published_config.skipped_keys.empty()) {
                                NotificationManager* notify_manager = q->get_notification_manager();
                                std::string message                 = _u8L("Some published settings could not be applied:");
                                for (const std::string& key : published_config.skipped_keys)
                                    message += "\n-" + key;
                                // Informational: the load succeeded, these keys were skipped.
                                notify_manager
                                    ->bbl_show_3mf_warn_notification(message,
                                                                     NotificationManager::NotificationLevel::WarningNotificationLevel);
                            }

                            // BBS: notify the user about slot materials that were replaced while
                            // loading a published project (type mismatch / no same-type match).
                            if (!published_config.material_replacements.empty()) {
                                NotificationManager* notify_manager = q->get_notification_manager();
                                std::string message                 = _u8L("Some filament slots were changed:");
                                for (const std::string& replacement : published_config.material_replacements)
                                    message += "\n-" + replacement;
                                // Informational: the load succeeded, the slots were adapted.
                                notify_manager
                                    ->bbl_show_3mf_warn_notification(message,
                                                                     NotificationManager::NotificationLevel::WarningNotificationLevel);
                            }

                            // Remember the imported published selection so the Publish dialog is
                            // pre-seeded with the file's settings. Stored after the load so any
                            // per-slot relocations are already reflected in material_keys.
                            if (published_config.published) {
                                this->m_has_pending_published = true;
                                this->m_pending_published_keys = published_config.published_keys;
                                this->m_pending_material_keys  = published_config.material_keys;
                                this->m_pending_published_legacy_process = published_config.print_layout.extruder_ids.empty();
                            }

                            ConfigOption* bed_type_opt = preset_bundle->project_config.option("curr_bed_type");
                            if (bed_type_opt != nullptr) {
                                BedType bed_type = (BedType)bed_type_opt->getInt();
                                // update app config for bed type
                                bool is_bbl_preset = preset_bundle->is_bbl_vendor();
                                if (is_bbl_preset) {
                                    AppConfig* app_config = wxGetApp().app_config;
                                    if (app_config)
                                        app_config->set("curr_bed_type", std::to_string(int(bed_type)));
                                }
                                q->on_bed_type_change(bed_type);
                            }

                            // BBS: moved this logic to presetcollection
                            //{
                            //    // After loading of the presets from project, check if they are visible.
                            //    // Set them to visible if they are not.

                            //    auto update_selected_preset_visibility = [](PresetCollection& presets, std::vector<std::string>& names) {
                            //        if (!presets.get_selected_preset().is_visible) {
                            //            assert(presets.get_selected_preset().name == presets.get_edited_preset().name);
                            //            presets.get_selected_preset().is_visible = true;
                            //            presets.get_edited_preset().is_visible = true;
                            //            names.emplace_back(presets.get_selected_preset().name);
                            //        }
                            //    };

                            //    std::vector<std::string> names;
                            //    if (printer_technology == ptFFF) {
                            //        update_selected_preset_visibility(preset_bundle->prints, names);
                            //        for (const std::string& filament : preset_bundle->filament_presets) {
                            //            Preset* preset = preset_bundle->filaments.find_preset(filament);
                            //            if (preset && !preset->is_visible) {
                            //                preset->is_visible = true;
                            //                names.emplace_back(preset->name);
                            //                if (preset->name == preset_bundle->filaments.get_edited_preset().name)
                            //                    preset_bundle->filaments.get_selected_preset().is_visible = true;
                            //            }
                            //        }
                            //    }
                            //    else {
                            //        update_selected_preset_visibility(preset_bundle->sla_prints, names);
                            //        update_selected_preset_visibility(preset_bundle->sla_materials, names);
                            //    }
                            //    update_selected_preset_visibility(preset_bundle->printers, names);

                            //    preset_bundle->update_compatible(PresetSelectCompatibleType::Never);

                            //    // show notification about temporarily installed presets
                            //    if (!names.empty()) {
                            //        std::string notif_text = into_u8(_L_PLURAL("The preset below was temporarily installed on the active instance of PrusaSlicer",
                            //                                                   "The presets below were temporarily installed on the active instance of PrusaSlicer",
                            //                                                   names.size())) + ":";
                            //        for (std::string& name : names)
                            //            notif_text += "\n - " + name;
                            //        notification_manager->push_notification(NotificationType::CustomNotification,
                            //            NotificationManager::NotificationLevel::PrintInfoNotificationLevel, notif_text);
                            //    }
                            //}

                            // BBS
                            // if (printer_technology == ptFFF)
                            //    CustomGCode::update_custom_gcode_per_print_z_from_config(model.custom_gcode_per_print_z, &preset_bundle->project_config);

                            // For exporting from the amf/3mf we shouldn't check printer_presets for the containing information about "Print Host upload"
                            // BBS: add preset combo box re-active logic
                            // currently found only needs re-active here
                            wxGetApp().load_current_presets(false, false);
                            // Update filament colors for the MM-printer profile in the full config
                            // to avoid black (default) colors for Extruders in the ObjectList,
                            // when for extruder colors are used filament colors
                            q->on_filament_count_change(preset_bundle->filament_presets.size());
                            is_project_file = true;

                            DynamicConfig& proj_cfg = preset_bundle->project_config;
                            // do some post process after loading config
                            {
                                //BBS: rewrite wipe tower pos stored in 3mf file , the code above should be seriously reconsidered
                                 ConfigOptionFloats* wipe_tower_x = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x");
                                ConfigOptionFloats* wipe_tower_y = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y");
                                if (file_wipe_tower_x)
                                    *wipe_tower_x = *file_wipe_tower_x;
                                if (file_wipe_tower_y)
                                    *wipe_tower_y = *file_wipe_tower_y;
                                if (file_wipe_tower_x && file_wipe_tower_y) fix_loaded_wipe_tower_positions();
                                sync_loaded_project_filament_config(proj_cfg);
                            }
                            // Update filament combobox after loading config
                            if (published_config.published) {
                                q->update_filament_colors_in_full_config();
                                wxGetApp().plater()->sidebar().update_all_preset_comboboxes();
                                wxGetApp().plater()->sidebar().update_dynamic_filament_list();
                            } else {
                                wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);
                            }
                            // The loaded project supplies nozzle_volume_type; refresh the sidebar
                            // nozzle-count badges against it.
                            if (auto *nozzle_volumes = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")) {
                                const int extruder_count = wxGetApp().preset_bundle->get_printer_extruder_count();
                                for (int extruder_id = 0; extruder_id < extruder_count && extruder_id < (int) nozzle_volumes->values.size(); ++extruder_id)
                                    updateNozzleCountDisplay(wxGetApp().preset_bundle, extruder_id, NozzleVolumeType(nozzle_volumes->values[extruder_id]));
                            }
                        }
                    }
                    if (!silence) wxGetApp().app_config->update_config_dir(path.parent_path().string());
                }
            } else {
                // BBS: add plate data related logic
                PlateDataPtrs& plate_data = parsed_file.plates;
                // Reader outputs stay alive through the existing import branches.
                std::vector<Preset*>& project_presets = parsed_file.presets;
                bool& is_xxx = parsed_file.is_inches;
                Semver& file_version = parsed_file.version;

                if (stage_geometry) {
                    model = std::move(parsed_file.model);
                    designer_model_id = parsed_file.designer_id;
                    designer_country_code = parsed_file.country_code;
                } else if (boost::iends_with(path.string(), ".stp") ||
                    boost::iends_with(path.string(), ".step")) {
                        double linear = string_to_double_decimal_point(wxGetApp().app_config->get("linear_deflection"));
                        if (linear <= 0) linear = 0.003;
                        double angle = string_to_double_decimal_point(wxGetApp().app_config->get("angle_deflection"));
                        if (angle <= 0) angle = 0.5;
                        bool split_compound = wxGetApp().app_config->get_bool("is_split_compound");
                        model = Slic3r::Model:: read_from_step(path.string(), strategy,
                        [&update_progress, real_filename, &progress_percent, step_percent, INPUT_FILES_RATIO, total_files, i](int load_stage, int current, int total, bool &cancel)
                        {
                                bool     cont = true;
                                float percent_float = (100.0f * (float)i / (float)total_files) + INPUT_FILES_RATIO * ((float)step_percent[load_stage] + (float)current * (float)(step_percent[load_stage + 1] - step_percent[load_stage]) / (float)total) / (float)total_files;
                                BOOST_LOG_TRIVIAL(trace) << "load_step_file: percent(float)=" << percent_float << ", stage = " << load_stage << ", curr = " << current << ", total = " << total;
                                progress_percent = (int)percent_float;
                                wxString msg  = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                                cont          = update_progress(progress_percent, msg);
                                cancel        = !cont;
                        },
                        [](int isUtf8StepFile) {
                            if (!isUtf8StepFile) {
                                const auto no_warn = wxGetApp().app_config->get_bool("step_not_utf8_no_warn");
                                if (!no_warn) {
                                    MessageDialog dlg(nullptr, _L("Component name(s) inside STEP file not in UTF-8 format!") + "\n\n" + _L("Because of unsupported text encoding, garbage characters may appear!"),
                                                      wxString(SLIC3R_APP_FULL_NAME " - ") + _L("Attention!"), wxOK | wxICON_INFORMATION);
                                    dlg.show_dsa_button(_L("Remember my choice."));
                                    dlg.ShowModal();
                                    if (dlg.get_checkbox_state()) {
                                        wxGetApp().app_config->set_bool("step_not_utf8_no_warn", true);
                                    }
                                }
                            }
                        },
                        [&is_user_cancel, &linear, &angle, &split_compound](Slic3r::Step& file, double& linear_value, double& angle_value, bool& is_split)-> int {
                            if (wxGetApp().app_config->get_bool("enable_step_mesh_setting")) {
                                StepMeshDialog mesh_dlg(nullptr, file, linear, angle);
                                if (mesh_dlg.ShowModal() == wxID_OK) {
                                    linear_value = mesh_dlg.get_linear_deflection();
                                    angle_value  = mesh_dlg.get_angle_deflection();
                                    is_split     = mesh_dlg.get_split_compound_value();
                                    return 1;
                                }
                            }else {
                                linear_value = linear;
                                angle_value = angle;
                                is_split = split_compound;
                                return 1;
                            }
                            is_user_cancel = true;
                            return -1;
                        }, linear, angle, split_compound);
                }else {
                    model = Slic3r::Model:: read_from_file(
                    path.string(), nullptr, nullptr, strategy, &plate_data, &project_presets, &is_xxx, &file_version, nullptr,
                    [&update_progress, real_filename, &progress_percent, INPUT_FILES_RATIO, total_files, i, &designer_model_id, &designer_country_code](int current, int total, bool &cancel, std::string &mode_id, std::string &code)
                    {
                            designer_model_id = mode_id;
                            designer_country_code = code;

                            bool     cont = true;
                            float percent_float = (100.0f * (float)i / (float)total_files) + INPUT_FILES_RATIO * 100.0f * ((float)current / (float)total) / (float)total_files;
                            BOOST_LOG_TRIVIAL(trace) << "load_stl_file: percent(float)=" << percent_float << ", curr = " << current << ", total = " << total;
                            progress_percent = (int)percent_float;
                            wxString msg  = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                            cont          = update_progress(progress_percent, msg);
                            cancel        = !cont;
                    },
                    nullptr, 0, obj_color_fn);
                    if (obj_color_fn && model.objects.empty()) {
                        q->skip_thumbnail_invalid = false;
                        return empty_result;
                    }
                }

                if (designer_model_id.empty() && boost::algorithm::iends_with(path.string(), ".stl")) {
                    read_binary_stl(path.string(), designer_model_id, designer_country_code);
                }

                if (type_any_amf && is_xxx) imperial_units = true;

                for (auto obj : model.objects) {
                    if (obj->name.empty()) {
                        obj->name = fs::path(obj->input_file).filename().string();
                    }
                }

                if (plate_data.size() > 0) {
                    partplate_list.load_from_3mf_structure(plate_data, project_filament_count);
                    partplate_list.update_slice_context_to_current_plate(background_process);
                    this->preview->update_gcode_result(partplate_list.get_current_slice_result());
                    release_PlateData_list(plate_data);
                    sidebar->obj_list()->reload_all_plates();
                }

                // BBS:: project embedded presets
                if (project_presets.size() > 0) {
                    // load project embedded presets
                    PresetsConfigSubstitutions preset_substitutions;
                    PresetBundle &             preset_bundle = *wxGetApp().preset_bundle;
                    preset_substitutions                     = preset_bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::Enable);
                    if (!preset_substitutions.empty()) show_substitutions_info(preset_substitutions);

                    for (unsigned int i = 0; i < project_presets.size(); i++) { delete project_presets[i]; }
                    project_presets.clear();
                }
            }
        } catch (const ConfigurationError &e) {
            std::string message = GUI::format(_L("Failed loading file \"%1%\". An invalid configuration was found."), filename.string()) + "\n\n" + e.what();
            report_error(from_u8(message));
            continue;
        } catch (const std::exception &e) {
            if (!is_user_cancel)
                report_error(from_u8(e.what()));
            continue;
        }

        progress_percent = 100.0f * (float)i / (float)total_files + INIT_MODEL_RATIO * 100.0f / (float)total_files;
        dlg_cont = update_progress(loop_progress(progress_percent));
        if (!dlg_cont) {
            q->skip_thumbnail_invalid = false;
            return empty_result;
        }

        if (load_model) {
            // The model should now be initialized

            // BBS: add load_old_project logic
            if ((!is_project_file) && (!load_old_project)) {
                prepare_texture_import_units(model, texture_options, imperial_units, q, filename);
                // if (!is_project_file) {
                if (int deleted_objects = model.removed_objects_with_zero_volume(); deleted_objects > 0 && !library_progress) {
                    MessageDialog(q, _L("Objects with zero volume removed"), _L("The volume of the object is zero"), wxICON_INFORMATION | wxOK).ShowModal();
                }
                // Turn the file's content as one piece by the printer's preferred orientation, once the objects
                // with no volume are gone. The objects keep their heights for the multi-part check below.
                if (!type_3mf)
                    model.rotate_and_center_objects(Geometry::deg2rad(config->opt_float("preferred_orientation")));
                // else if (model.looks_like_imperial_units()) {
                // BBS do not handle look like in imperial
                // auto convert_model_if = [convert_from_imperial_units](Model& model, bool condition) {
                //    if (condition)
                //        //FIXME up-scale only the small parts?
                //        convert_from_imperial_units(model, true);
                //};
                // if (answer_convert_from_imperial_units == wxOK_DEFAULT) {
                //    RichMessageDialog dlg(q, format_wxstr(_L_PLURAL(
                //        "The dimensions of the object from file %s seem to be defined in inches.\n"
                //        "The internal unit of PrusaSlicer is a millimeter. Do you want to recalculate the dimensions of the object?",
                //        "The dimensions of some objects from file %s seem to be defined in inches.\n"
                //        "The internal unit of PrusaSlicer is a millimeter. Do you want to recalculate the dimensions of these objects?", model.objects.size()), from_path(filename))
                //        + "\n", _L("The object is too small"), wxICON_QUESTION | wxYES_NO);
                //    dlg.ShowCheckBox(_L("Apply to all the remaining small objects being loaded."));
                //    int answer = dlg.ShowModal();
                //    if (dlg.IsCheckBoxChecked())
                //        answer_convert_from_imperial_units = answer;
                //    else
                //        convert_model_if(model, answer == wxID_YES);
                //}
                // convert_model_if(model, answer_convert_from_imperial_units == wxID_YES);
            }

             // An AMF group with non-printable bodies was kept apart on purpose; merging it would make them printable.
             // Only the assembly modes keep such groups apart; Separate objects asks as before.
             const auto amf_options = import_options.find("amf");
             const bool amf_assembly = stage_geometry && parsed_file.format == "amf" && amf_options != import_options.end() &&
                                       !amf_options->second.separate_objects;
             const bool has_excluded_bodies = amf_assembly && std::any_of(model.objects.begin(), model.objects.end(), [](const ModelObject* object) {
                 return std::any_of(object->instances.begin(), object->instances.end(), [](const ModelInstance* instance) { return !instance->printable; });
             });
             if (!library_progress && !is_project_file && !has_excluded_bodies && model.looks_like_multipart_object()) {
               MessageDialog msg_dlg(q, _L("This file contains several objects positioned at multiple heights.\nInstead of considering them as multiple objects, should the file be loaded as a single object with multiple parts\?") + "\n",
                    _L("Multi-part object detected"), wxICON_WARNING | wxYES | wxNO);
                if (msg_dlg.ShowModal() == wxID_YES) {
                    // Kept original part names survive Orca's own merge; other files keep its naming.
                    model.convert_multipart_object(filaments_cnt, parsed_file.names_kept);
                    // The merged object is new, so the choices of this file's import dialog apply to it again.
                    if (auto options = import_options.find(parsed_file.format); stage_geometry && options != import_options.end())
                        for (ModelObject* object : model.objects)
                            apply_import_presentation(*object, options->second.single_object(),
                                                      options->second.keep_names && options->second.sort_names);
                }
            }
        }
        // else if ((wxGetApp().get_mode() == comSimple) && (type_3mf || type_any_amf) && model_has_advanced_features(model)) {
        //    MessageDialog msg_dlg(q, _L("This file cannot be loaded in a simple mode. Do you want to switch to an advanced mode?")+"\n",
        //        _L("Detected advanced data"), wxICON_WARNING | wxYES | wxNO);
        //    if (msg_dlg.ShowModal() == wxID_YES) {
        //        Slic3r::GUI::wxGetApp().save_mode(comAdvanced);
        //        view3D->set_as_dirty();
        //    }
        //    else
        //        return obj_idxs;
        //}

        progress_percent = 100.0f * (float)i / (float)total_files + CENTER_AROUND_ORIGIN_RATIO * 100.0f / (float)total_files;
        dlg_cont = update_progress(loop_progress(progress_percent));
        if (!dlg_cont) {
            q->skip_thumbnail_invalid = false;
            return empty_result;
        }

        int model_idx = 0;
        for (ModelObject *model_object : model.objects) {
            if (!type_3mf && !type_any_amf)
                model_object->center_around_origin(false);

            // BBS
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << " " << boost::format("import 3mf IMPORT_LOAD_MODEL_OBJECTS \n");
            wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
            model_idx++;
            dlg_cont = update_progress(loop_progress(progress_percent), msg);
            if (!dlg_cont) {
                q->skip_thumbnail_invalid = false;
                return empty_result;
            }

            if (!model_object->instances.empty())
                model_object->ensure_on_bed(is_project_file);
        }

        tolal_model_count += model_idx;

        progress_percent = 100.0f * (float)i / (float)total_files + LOAD_MODEL_RATIO * 100.0f / (float)total_files;
        dlg_cont = update_progress(loop_progress(progress_percent));
        if (!dlg_cont) {
            q->skip_thumbnail_invalid = false;
            return empty_result;
        }

        if (one_by_one) {
            // BBS: add load_old_project logic
            if (type_3mf && !is_project_file && !load_old_project)
                // if (type_3mf && !is_project_file)
                model.center_instances_around_point(this->bed.build_volume().bed_center());
            // BBS: add auxiliary files logic
            // BBS: backup & restore
            if (load_aux) {
                q->model().load_from(model);
                load_auxiliary_files();
            }
            // Texture-to-color: a mesh that arrived with UVs and a decoded texture gets its
            // faces clustered into printable colours and matched against the filament list,
            // before the objects are handed to the plater. Inert for every other model.
            if (model.texture_mesh && has_importable_texture(*model.texture_mesh)) {
                TextureImportResult texture_import_result;
                auto cancel_cb   = [&progress_was_cancelled, &dlg_cont]() { return !dlg_cont || progress_was_cancelled(); };
                auto progress_cb = [&update_progress, &dlg_cont, &progress_percent, &loop_progress](int percent) {
                    progress_percent = std::clamp(percent, 0, 100);
                    dlg_cont = update_progress(loop_progress(progress_percent), _L("Matching textures to filaments"));
                    return dlg_cont;
                };
                if (!run_textured_mesh_import_dialog(model, texture_import_result, cancel_cb, progress_cb, texture_options, color_result)) {
                    if (color_result != nullptr) color_result->cancelled = color_result->error.empty();
                    q->skip_thumbnail_invalid = false;
                    return empty_result;
                }
                if (texture_import_result.fallback_to_geometry_only && !texture_import_result.fallback_warning.empty()) {
                    MessageDialog(q, texture_import_result.fallback_warning,
                                  _L("Texture Import Warning"),
                                  wxOK | wxICON_WARNING).ShowModal();
                }
                if (texture_import_result.matched_colors || !texture_import_result.painted.face_colors.empty()) {
                    std::vector<size_t> texture_object_idxs(model.objects.size());
                    std::iota(texture_object_idxs.begin(), texture_object_idxs.end(), 0);
                    auto apply_progress_cb = [&update_progress, &loop_progress](int percent, const wxString& msg) {
                        update_progress(loop_progress(std::clamp(percent, 0, 100)), msg);
                        return true;
                    };
                    apply_textured_mesh_import_result(model, texture_object_idxs, texture_import_result,
                                                      apply_progress_cb, false);
                    collect_model_color_import_result(color_result, model, texture_import_result.painted.cluster_colors.size());
                }
            }

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", before load_model_objects, count %1%")%model.objects.size();
            auto loaded_idxs = load_model_objects(model.objects, is_project_file, false, true,
                stage_geometry ? &import_display_options : nullptr);
            if (is_project_file && load_config) partplate_list.restore_workbench_layer_object_ids(model.objects, loaded_idxs);
            obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());

            // load_model_objects only transfers ModelObjects; carry the Model-level CAD recipe
            // onto the plater model so the Design tab can rehydrate the editable feature tree on
            // reopen. Assigned unconditionally on the project-replacing path so that opening a
            // project without a recipe clears whatever the previous one left behind; importing a
            // plain model into the open project leaves the current recipe alone.
            if (is_project_file) {
                q->model().cad_recipe = model.cad_recipe;
                loaded_cad_recipe     = !model.cad_recipe.empty();
            }

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", finished load_model_objects");
            wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
            dlg_cont     = update_progress(loop_progress(progress_percent), msg);
            if (!dlg_cont) {
                q->skip_thumbnail_invalid = false;
                return empty_result;
            }
        } else {
            // This must be an .stl or .obj file, which may contain a maximum of one volume.
            for (const ModelObject *model_object : model.objects) {
                ModelObject* copied_object = new_model->add_object(*model_object);
                const auto display = import_display_options.find(model_object->id());
                if (display != import_display_options.end())
                    import_display_options.emplace(copied_object->id(), display->second);

                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":" << __LINE__ << boost::format(", added object %1%")%model_object->name;
                wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                dlg_cont     = update_progress(loop_progress(progress_percent), msg);
                if (!dlg_cont) {
                    q->skip_thumbnail_invalid = false;
                    return empty_result;
                }
            }
        }
    }

    if (new_model != nullptr && new_model->objects.size() > 1) {
        //BBS do not popup this dialog

        bool new_model_auto_drop = true;
        int single_object_answer = false;
        if (ask_multi) {
            RichMessageDialog dlg(q, _L("Load these files as a single object with multiple parts?\n"),
                _L("An object with multiple parts was detected"), wxICON_QUESTION | wxYES_NO);

            dlg.ShowCheckBox(_L("Auto-Drop"), true);
            single_object_answer = dlg.ShowModal();

            if (dlg.IsCheckBoxChecked() == false)
                new_model_auto_drop = false;

            // convert to multipart and split after load_model_objects
            // to keep relative positioning if auto_drop == false
            if (single_object_answer == wxID_YES || new_model_auto_drop == false) {
                // Parts keep their imported names when any geometry import asked to keep them.
                new_model->convert_multipart_object(filaments_cnt, std::any_of(staged_files.begin(), staged_files.end(),
                    [](const auto& staged) { return staged->names_kept; }));
                // Files of several kinds share the merged object; it takes a choice only when every file made it.
                const bool all_staged = stage_geometry && std::all_of(staged_files.begin(), staged_files.end(),
                    [](const auto& staged) { return !staged->format.empty(); });
                bool single_object = all_staged;
                bool sort = all_staged;
                for (const auto& staged : staged_files)
                    if (const auto options = import_options.find(staged->format); options != import_options.end()) {
                        single_object = single_object && options->second.single_object();
                        sort = sort && options->second.keep_names && options->second.sort_names;
                    }
                if (stage_geometry)
                    for (ModelObject* object : new_model->objects)
                        apply_import_presentation(*object, single_object, sort);
            }
        }

        // TODO
        // DONE always convert to multipart, split afterwards to retain relative position
        // DONE if !auto_drop move all objects over the z-position 0, so that none are clipped by the bed.
        // DONE retain auto_drop (and printable) state when assembling or splitting objects.
        // DONE when manually split to object ask users if looks_like_multipart and none have auto_drob disabled if they want to disable auto_drop for all resulting objects.
        // - add icon in object list, similar to fuzzy painting, etc.

        auto loaded_idxs = load_model_objects(new_model->objects, false, false, new_model_auto_drop,
            stage_geometry ? &import_display_options : nullptr);
        obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());

        if (single_object_answer == wxID_NO && new_model_auto_drop == false) {
            split_object(loaded_idxs[0], new_model_auto_drop);
        }
    }

    if (load_config) {
        DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            MachineObject *obj = dev->get_selected_machine();
            if (obj && obj->is_info_ready()) {
                if (obj->GetExtderSystem()->GetTotalExtderCount() > 0) {
                    PresetBundle *preset_bundle  = wxGetApp().preset_bundle;
                    Preset       &printer_preset = preset_bundle->printers.get_selected_preset();

                    double              preset_nozzle_diameter = 0.4;
                    const ConfigOption *opt                    = printer_preset.config.option("nozzle_diameter");
                    if (opt) preset_nozzle_diameter = static_cast<const ConfigOptionFloats *>(opt)->values[0];

                    std::string machine_type = obj->printer_type;
                    if (obj->is_support_upgrade_kit && obj->installed_upgrade_kit) machine_type = "C12";

                    bool nozzle_mismatch = !obj->GetExtderSystem()->NozzleDiameterMatchesOrUnknown(0, (float) preset_nozzle_diameter);
                    if (printer_preset.get_current_printer_type(preset_bundle) != machine_type || nozzle_mismatch) {
                        Preset *machine_preset = get_printer_preset(obj);
                        if (machine_preset != nullptr) {
                            std::string printer_model = machine_preset->config.option<ConfigOptionString>("printer_model")->value;
                            bool sync_printer_info = false;
                            if (!wxGetApp().app_config->has("sync_after_load_file_show_flag")) {
                                wxString tips = from_u8((boost::format(_u8L("Connected printer is %s. It must match the project preset for printing.\n")) % printer_model).str());

                                tips += _L("Do you want to sync the printer information and automatically switch the preset?");
                                TipsDialog dlg(wxGetApp().mainframe, _L("Tips"), tips, "sync_after_load_file_show_flag", wxYES_NO);
                                if (dlg.ShowModal() == wxID_YES) { sync_printer_info = true; }
                            }
                            else {
                                sync_printer_info = wxGetApp().app_config->get("sync_after_load_file_show_flag") == "true";
                            }
                            if (sync_printer_info) {
                                Tab *printer_tab = GUI::wxGetApp().get_tab(Preset::Type::TYPE_PRINTER);
                                printer_tab->select_preset(machine_preset->name);
                                if (obj->is_multi_extruders()) GUI::wxGetApp().sidebar().sync_extruder_list();
                            }
                        }
                    }
                }
            }
        }
    }


    //BBS: translate old 3mf to correct positions
    if (translate_old) {
        //translate the objects
        int plate_count = partplate_list.get_plate_count();
        for (int index = 1; index < plate_count; index ++) {
            PartPlate* cur_plate = (PartPlate *)partplate_list.get_plate(index);

            Vec3d cur_origin = cur_plate->get_origin();
            Vec3d new_origin = partplate_list.compute_origin_using_new_size(index, current_width, current_depth);

            cur_plate->translate_all_instance(new_origin - cur_origin);
        }
        view3D->get_canvas3d()->remove_raycasters_for_picking(SceneRaycaster::EType::Bed);
        partplate_list.reset_size(current_width, current_depth, current_height, true, true);
        partplate_list.register_raycasters_for_picking(*view3D->get_canvas3d());
    }

    //BBS: add gcode loading logic in the end
    q->m_exported_file = false;
    q->skip_thumbnail_invalid = false;
    if (load_model && load_config) {
        if (model.objects.empty()) {
            partplate_list.load_gcode_files();
            PartPlate * first_plate = nullptr, *cur_plate = nullptr;
            int plate_cnt = partplate_list.get_plate_count();
            int index = 0, first_plate_index = 0;
            q->m_valid_plates_count = 0;
            for (index = 0; index < plate_cnt; index ++)
            {
                cur_plate = partplate_list.get_plate(index);
                if (!first_plate && cur_plate->is_slice_result_valid()) {
                    first_plate = cur_plate;
                    first_plate_index = index;
                }
                if (cur_plate->is_slice_result_valid())
                    q->m_valid_plates_count ++;
            }
            if (first_plate&&first_plate->is_slice_result_valid()) {
                q->m_exported_file = true;
                //select plate 0 as default
                q->select_plate(first_plate_index);
                //set to 3d tab
                q->select_view_3D("Preview");
                wxGetApp().mainframe->select_tab(TAB_ID_PREVIEW);
            }
            else {
                //set to 3d tab
                q->select_view_3D("3D");
                //select plate 0 as default
                q->select_plate(0);
            }
        }
        else {
            //set to 3d tab
            q->select_view_3D("3D");
            //select plate 0 as default
            q->select_plate(0);
        }
    }
    else {
        //always set to 3D after loading files
        q->select_view_3D("3D");
        wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
    }

    if (load_model) {
        if (!q->m_exported_file && view3D != nullptr) {
            // Force a 3D scene refresh after view/plate selection to avoid losing the first load
            // on platforms where the GL canvas mapping lags behind model loading.
            view3D->reload_scene(true);
            view3D->set_as_dirty();
        }
        if (!silence) wxGetApp().app_config->update_skein_dir(input_files[input_files.size() - 1].parent_path().make_preferred().string());
        // XXX: Plater.pm had @loaded_files, but didn't seem to fill them with the filenames...
    }

    // automatic selection of added objects
    if (!obj_idxs.empty() && view3D != nullptr) {
        // update printable state for new volumes on canvas3D
        wxGetApp().plater()->canvas3D()->update_instance_printable_state_for_objects(obj_idxs);

        if (!load_config) {
            Selection& selection = view3D->get_canvas3d()->get_selection();
            selection.clear();
            for (size_t idx : obj_idxs) {
                selection.add_object((unsigned int)idx, false);
            }
        }
        // BBS: update object list selection
        this->sidebar->obj_list()->update_selections();

        if (view3D->get_canvas3d()->get_gizmos_manager().is_enabled())
            // this is required because the selected object changed and the flatten on face an sla support gizmos need to be updated accordingly
            view3D->get_canvas3d()->update_gizmos_on_off_state();
    }

    GLGizmoSimplify::add_simplify_suggestion_notification(
        obj_idxs, model.objects, *notification_manager);

    //set designer_model_id
    q->model().stl_design_id = designer_model_id;
    q->model().stl_design_country = designer_country_code;
    //if (!designer_model_id.empty() && q->model().stl_design_id.empty() && !designer_country_code.empty()) {
    //    q->model().stl_design_id = designer_model_id;
    //    q->model().stl_design_country = designer_country_code;
    //}
    //else {
    //    q->model().stl_design_id = "";
    //    q->model().stl_design_country = "";
    //}

    // A CAD project legitimately carries no mesh: the model lives in the feature tree until it is
    // committed to the plate. Warning "no geometry data" for one is false, and it is the LAST thing
    // a user sees after opening a design they spent an hour on — it reads as "your work is gone"
    // when the recipe has in fact just been loaded and the Design tab will rehydrate it. Count a
    // recipe that came from THESE files as geometry.
    //
    // Only where the Design tab can actually show it, though. With the CAD feature switched off
    // (or not built) the recipe is still carried through to the next save, but nothing will
    // display it, so an empty plate needs saying — and saying why.
    bool cad_can_show = false;
#ifdef SLIC3R_CAD
    cad_can_show = wxGetApp().is_enable_cad_feature();
#endif
    if (tolal_model_count <= 0 && !q->m_exported_file && (!loaded_cad_recipe || !cad_can_show)) {
        if (load_dialog) load_dialog->Hide();
        if (!is_user_cancel) {
            const wxString text = loaded_cad_recipe
                ? _L("This project contains a model made in the Design tab and no other geometry. "
                     "Enable \"CAD feature (experimental)\" in Preferences and restart to see and edit it; "
                     "it is kept when the project is saved.")
                : _L("The file does not contain any geometry data.");
            if (library_progress) report_error(text);
            else MessageDialog(wxGetApp().mainframe, text, _L("Warning"), wxOK | wxICON_WARNING).ShowModal();
        }
    }
    if (!library_progress && stage_geometry && !obj_idxs.empty()) {
        // Save only after all new questions succeeded and the prepared models were published.
        for (const auto& entry : import_options)
            save_geometry_import_options(*wxGetApp().app_config, entry.first, entry.second);
        save_geometry_import_name_policy(*wxGetApp().app_config, name_policy);
    }
    q->schedule_background_process(true);
    q->mark_plate_toolbar_image_dirty();
    if (invalid_bed_exclude_area) {
        GUI::show_error(q, _L("The excluded bed area is invalid. It must be a simple, non-self-intersecting polygon.") +
            "\n" + _L("The invalid area has been removed, so please define it again using a valid polygon."));
    }
    return obj_idxs;
}

bool Plater::import_library_model(const fs::path& path, const std::function<bool(int)>& progress)
{
    if (m_only_gcode || m_exported_file || !progress) return false;
    std::unique_ptr<TakeSnapshot> snapshot;
    const auto before_publish = [&] {
        snapshot = std::make_unique<TakeSnapshot>(this, "Load File: " + encode_path(path.filename().string().c_str()));
    };
    return !p->load_files({path}, LoadStrategy::LoadModel | LoadStrategy::Silence, false,
        ObjImportColorFn{}, nullptr, nullptr, nullptr, nullptr, before_publish, progress).empty();
}
