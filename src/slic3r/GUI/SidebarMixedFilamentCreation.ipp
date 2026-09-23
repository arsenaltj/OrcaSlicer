// Included once by Plater.cpp inside Slic3r::GUI after the color/gradient helpers.
// Native mixed-slot staging is shared by manual creation and texture import.
static bool create_mixed_filament_from_result(
    Sidebar* sidebar,
    const MixedFilamentResult& result,
    const std::vector<std::string>& color_strs, bool undoable = false,
    const std::function<bool(const PresetBundle&)>& before_commit = {})
{
    if (!sidebar || result.components.size() < 2 || result.ratios.size() < 2)
        return false;
    if (!dynamic_cast<Plater*>(sidebar->GetParent()))
        return false;

    size_t num_physical = sidebar->combos_filament().size();
    if (!wxGetApp().preset_bundle->can_add_mixed_filament()) return false;

    PresetBundle staged(*wxGetApp().preset_bundle);
    auto& project_config = staged.project_config;
    size_t total = wxGetApp().preset_bundle->filament_presets.size();
    size_t new_idx = total;

    std::string mixed_color = blend_mixed_color(result.components, result.ratios, color_strs);
    staged.set_num_filaments(total + 1, mixed_color);

    auto* multi_colour_opt = project_config.option<ConfigOptionStrings>("filament_multi_colour");
    if (multi_colour_opt) {
        while (multi_colour_opt->values.size() <= new_idx) multi_colour_opt->values.push_back("");
        multi_colour_opt->values[new_idx] = mixed_color;
    }

    // set_num_filaments() above already grows these parallel arrays; the writes are still
    // size-guarded so a sizing bug degrades into a no-op rather than a heap overwrite.
    {
        auto* is_mixed_opt = project_config.option<ConfigOptionBools>("filament_is_mixed");
        while (is_mixed_opt->values.size() <= new_idx) is_mixed_opt->values.push_back(false);
        is_mixed_opt->values[new_idx] = true;
    }

    std::string comp_str;
    for (size_t i = 0; i < result.components.size(); ++i) {
        if (i > 0) comp_str += ",";
        comp_str += std::to_string(result.components[i]);
    }
    {
        auto* comp_opt = project_config.option<ConfigOptionStrings>("filament_mixed_components");
        while (comp_opt->values.size() <= new_idx) comp_opt->values.push_back(std::string{});
        comp_opt->values[new_idx] = comp_str;
    }

    int ratio_sum = 0;
    for (int r : result.ratios) ratio_sum += r;
    if (ratio_sum <= 0) ratio_sum = 100;

    std::string ratio_str;
    {
        CNumericLocalesSetter c_locale_setter;
        for (size_t i = 0; i < result.ratios.size(); ++i) {
            if (i > 0) ratio_str += ",";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", (float)result.ratios[i] / ratio_sum);
            ratio_str += buf;
        }
    }
    {
        auto* ratios_opt = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
        while (ratios_opt->values.size() <= new_idx) ratios_opt->values.push_back(std::string{});
        ratios_opt->values[new_idx] = ratio_str;
    }

    if (!project_config.option("filament_mixed_gradient"))
        project_config.set_key_value("filament_mixed_gradient", new ConfigOptionBools({false}));
    if (!project_config.option("filament_mixed_gradient_range"))
        project_config.set_key_value("filament_mixed_gradient_range", new ConfigOptionStrings({""}) );
    if (!project_config.option("filament_mixed_gradient_curve"))
        project_config.set_key_value("filament_mixed_gradient_curve", new ConfigOptionStrings({""}) );
    if (!project_config.option("filament_mixed_gradient_per_part"))
        project_config.set_key_value("filament_mixed_gradient_per_part", new ConfigOptionBools({false}));

    {
        auto* grad_opt = project_config.option<ConfigOptionBools>("filament_mixed_gradient");
        while (grad_opt->values.size() <= new_idx) grad_opt->values.push_back(false);
        grad_opt->values[new_idx] = result.gradient_enabled;
    }
    {
        auto* grad_range_opt = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_range");
        while (grad_range_opt->values.size() <= new_idx) grad_range_opt->values.push_back("");
        if (result.gradient_enabled && result.components.size() == 2) {
            const char* fmt = (result.gradient_direction == 0) ? "0.9000,0.1000" : "0.1000,0.9000";
            grad_range_opt->values[new_idx] = fmt;
        } else {
            grad_range_opt->values[new_idx] = "";
        }
    }
    {
        auto* grad_curve_opt = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_curve");
        while (grad_curve_opt->values.size() <= new_idx) grad_curve_opt->values.push_back("");
        grad_curve_opt->values[new_idx] = serialize_mixed_gradient_curve_if_custom(result);
    }
    {
        auto* per_part_opt = project_config.option<ConfigOptionBools>("filament_mixed_gradient_per_part");
        while (per_part_opt->values.size() <= new_idx) per_part_opt->values.push_back(false);
        per_part_opt->values[new_idx] = result.gradient_enabled && result.per_part_gradient;
    }

    auto& presets = staged.filament_presets;
    if (result.components[0] >= 1 && result.components[0] <= num_physical && presets.size() > new_idx)
        presets[new_idx] = presets[result.components[0] - 1];

    if (before_commit && !before_commit(staged)) return false;
    if (undoable) {
        std::string error;
        if (!wxGetApp().plater()->apply_project_config(std::move(staged.project_config), std::move(staged.filament_presets), "Add Mixed Filament", error)) {
            GUI::show_error(sidebar, wxString::FromUTF8(error.c_str()));
            return false;
        }
    } else {
        // Texture import and clone flows already own their model transaction.
        staged.project_config.swap(wxGetApp().preset_bundle->project_config);
        staged.filament_presets.swap(wxGetApp().preset_bundle->filament_presets);
        staged.ams_multi_color_filment.swap(wxGetApp().preset_bundle->ams_multi_color_filment);
    }
    size_t filament_count = wxGetApp().preset_bundle->filament_presets.size();
    wxGetApp().plater()->get_partplate_list().on_filament_added(filament_count);
    wxGetApp().plater()->on_filament_count_change(filament_count);
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

    sidebar->update_mixed_filament_list();
    wxGetApp().plater()->update_project_dirty_from_presets();
    wxPostEvent(sidebar, SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, sidebar));
    return true;
}
