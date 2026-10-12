// Native mixed-filament row rendering; included after Sidebar::priv is defined.
void Sidebar::update_mixed_filament_list()
{
    auto* plater = dynamic_cast<Plater*>(GetParent());
    if (!plater) return;

    wxWindowUpdateLocker noUpdates(this);

    // The native workspace owns its own dark palette while the rest of Orca may
    // still be in the light system theme. These rows are owner-drawn, so the
    // palette must be resolved here instead of relying on a later window theme
    // pass to recolor pixels that the paint handler draws itself.
    const bool redesign_workspace = wxGetApp().mainframe &&
        wxGetApp().mainframe->is_redesign_shell_active();
    const wxColour mc_bg = redesign_workspace ? RedesignTheme::panel_colour() :
        StateColor::darkModeColorFor(*wxWHITE);
    const wxColour mc_border = redesign_workspace ? wxColour(82, 82, 87) :
        StateColor::darkModeColorFor(wxColour("#CECECE"));
    const wxColour mc_text = redesign_workspace ? wxColour(230, 230, 233) :
        StateColor::darkModeColorFor(wxColour("#262E30"));
    const wxColour mc_dim = redesign_workspace ? wxColour(176, 176, 182) :
        StateColor::darkModeColorFor(wxColour("#ACACAC"));

    auto& project_config = wxGetApp().preset_bundle->project_config;
    auto mixed_indices = plater->mixed_filament_config_indices();
    size_t num_physical = p->combos_filament.size();

    auto* is_mixed_opt    = project_config.option<ConfigOptionBools>("filament_is_mixed");
    auto* components_opt  = project_config.option<ConfigOptionStrings>("filament_mixed_components");
    auto* ratios_opt      = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
    auto* colours_opt     = project_config.option<ConfigOptionStrings>("filament_colour");
    auto* grad_opt        = project_config.option<ConfigOptionBools>("filament_mixed_gradient");
    auto* grad_range_opt  = project_config.option<ConfigOptionStrings>("filament_mixed_gradient_range");

    bool can_mix = (num_physical >= 2);
    bool has_mixed = can_mix && !mixed_indices.empty();

    // Check integrity of mixed filament component references
    std::vector<size_t> broken_slots;
    if (is_mixed_opt && components_opt)
        broken_slots = check_mixed_filament_integrity(is_mixed_opt->values, components_opt->values, num_physical);
    std::set<size_t> broken_set(broken_slots.begin(), broken_slots.end());

    // Type consistency check
    if (is_mixed_opt && components_opt) {
        std::vector<std::string> physical_types;
        auto& preset_bundle = *wxGetApp().preset_bundle;
        for (size_t i = 0; i < num_physical; ++i) {
            std::string ft;
            if (i < preset_bundle.filament_presets.size()) {
                auto* preset = preset_bundle.filaments.find_preset(preset_bundle.filament_presets[i]);
                if (preset) {
                    std::string display_type;
                    ft = preset->config.get_filament_type(display_type);
                }
            }
            if (ft.empty()) ft = "PLA";
            physical_types.push_back(ft);
        }
        auto type_mismatch_slots = check_mixed_filament_type_consistency(
            is_mixed_opt->values, components_opt->values, physical_types);
        for (size_t s : type_mismatch_slots)
            broken_set.insert(s);
        broken_slots.insert(broken_slots.end(), type_mismatch_slots.begin(), type_mismatch_slots.end());
    }

    bool at_limit = !wxGetApp().preset_bundle->can_add_mixed_filament();
    p->m_btn_add_mixed_filament->Show(can_mix && !has_mixed && !at_limit);
    p->m_panel_mixed_title->Show(has_mixed);
    p->m_mixed_scroll_area->Show(has_mixed);
    p->m_panel_mixed_content->Show(has_mixed);
    if (p->m_btn_mixed_add)
        p->m_btn_mixed_add->Enable(!at_limit);
    p->m_panel_mixed_warning->Show(false);

    // Show/dismiss 3D canvas notification for broken mixed filaments
    if (has_mixed && !broken_set.empty()) {
        auto* notify = wxGetApp().plater()->get_notification_manager();
        if (notify)
            notify->push_notification(NotificationType::BBLMixedFilamentBroken,
                NotificationManager::NotificationLevel::ErrorNotificationLevel,
                _u8L("Mixed filament has invalid or mismatched components. Please re-edit affected entries."));
    } else {
        auto* notify = wxGetApp().plater()->get_notification_manager();
        if (notify)
            notify->close_notification_of_type(NotificationType::BBLMixedFilamentBroken);
    }

    if (has_mixed) {
        auto* left_col  = p->m_sizer_mixed_filaments->GetItem(size_t(0))->GetSizer();
        auto* right_col = p->m_sizer_mixed_filaments->GetItem(size_t(1))->GetSizer();
        left_col->Clear(true);
        right_col->Clear(true);

        std::vector<std::string> physical_colors;
        if (colours_opt) {
            for (size_t i = 0; i < num_physical && i < colours_opt->values.size(); ++i)
                physical_colors.push_back(colours_opt->values[i]);
        }

        auto make_swatch_panel = [this](wxWindow* parent, const wxColour& col, unsigned int num) -> wxPanel* {
            int swatch_sz = FromDIP(20);
            auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(swatch_sz, swatch_sz));
            panel->SetMinSize(wxSize(swatch_sz, swatch_sz));
            bool is_dark = wxGetApp().dark_mode();
            panel->Bind(wxEVT_PAINT, [panel, col, num, is_dark](wxPaintEvent&) {
                wxPaintDC dc(panel);
                wxSize sz = panel->GetClientSize();
                dc.SetBackground(wxBrush(col));
                dc.Clear();
                if (!is_dark && col.Red() > 224 && col.Green() > 224 && col.Blue() > 224) {
                    dc.SetPen(wxPen(wxColour(130, 130, 128), 1));
                    dc.SetBrush(*wxTRANSPARENT_BRUSH);
                    dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());
                }
                if (is_dark && col.Red() < 45 && col.Green() < 45 && col.Blue() < 45) {
                    dc.SetPen(wxPen(wxColour(207, 207, 207), 1));
                    dc.SetBrush(*wxTRANSPARENT_BRUSH);
                    dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());
                }
                wxString txt = wxString::Format("%u", num);
                dc.SetFont(::Label::Body_14);
                wxSize txt_sz = dc.GetTextExtent(txt);
                dc.SetTextForeground(col.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);
                dc.DrawText(txt, (sz.GetWidth() - txt_sz.GetWidth()) / 2,
                                 (sz.GetHeight() - txt_sz.GetHeight()) / 2);
            });
            return panel;
        };

        for (size_t i = 0; i < mixed_indices.size(); ++i) {
            size_t cfg_idx = mixed_indices[i];
            auto* combo_and_btn_sizer = new wxBoxSizer(wxHORIZONTAL);

            combo_and_btn_sizer->Add(FromDIP(10), 0, 0, 0, 0);

            // Parse components and ratios from config strings (supports 2-N components)
            std::vector<unsigned int> comp_ids;
            std::vector<int> comp_ratios;
            if (components_opt && cfg_idx < components_opt->values.size()) {
                std::istringstream iss(components_opt->values[cfg_idx]);
                std::string tok;
                while (std::getline(iss, tok, ',')) {
                    unsigned int v = 0;
                    if (std::sscanf(tok.c_str(), "%u", &v) == 1)
                        comp_ids.push_back(v);
                }
            }
            if (ratios_opt && cfg_idx < ratios_opt->values.size()) {
                CNumericLocalesSetter c_locale_setter;
                std::istringstream iss(ratios_opt->values[cfg_idx]);
                std::string tok;
                while (std::getline(iss, tok, ',')) {
                    float v = 0;
                    if (std::sscanf(tok.c_str(), "%f", &v) == 1)
                        comp_ratios.push_back((int)(v * 100 + 0.5f));
                }
            }
            if (!comp_ids.empty() && comp_ratios.size() != comp_ids.size()) {
                BOOST_LOG_TRIVIAL(warning) << "Mixed filament slot " << cfg_idx
                    << ": ratio count (" << comp_ratios.size()
                    << ") != component count (" << comp_ids.size()
                    << "), resetting to even distribution";
                int n = (int)comp_ids.size();
                comp_ratios.assign(n, 100 / n);
                comp_ratios[0] += 100 - (100 / n) * n;
            }

            bool is_broken = broken_set.count(cfg_idx) > 0;

            // Recalculate mixed color based on current physical colors
            if (!is_broken && !comp_ids.empty() && comp_ids.size() == comp_ratios.size()) {
                std::string new_mixed_color = blend_mixed_color(comp_ids, comp_ratios, physical_colors);

                if (colours_opt && cfg_idx < colours_opt->values.size() && colours_opt->values[cfg_idx] != new_mixed_color) {
                    colours_opt->values[cfg_idx] = new_mixed_color;

                    auto* multi_colour_opt = project_config.option<ConfigOptionStrings>("filament_multi_colour");
                    if (multi_colour_opt && cfg_idx < multi_colour_opt->values.size()) {
                        multi_colour_opt->values[cfg_idx] = new_mixed_color;
                    }
                }
            }

            bool is_gradient = false;
            int  gradient_direction = 0;
            if (grad_opt && cfg_idx < grad_opt->values.size())
                is_gradient = grad_opt->values[cfg_idx];
            if (is_gradient && grad_range_opt && cfg_idx < grad_range_opt->values.size()) {
                CNumericLocalesSetter c_locale_setter;
                float v0 = 0, v1 = 0;
                if (std::sscanf(grad_range_opt->values[cfg_idx].c_str(), "%f,%f", &v0, &v1) == 2)
                    gradient_direction = (v0 > v1) ? 0 : 1;
            }

            std::string mix_color_str = (colours_opt && cfg_idx < colours_opt->values.size())
                ? colours_opt->values[cfg_idx] : "#888888";
            wxColour mix_col(mix_color_str);
            unsigned int mix_num = (unsigned int)(cfg_idx + 1);

            // The swatch fades bottom to top over the model's height, sampled the same way
            // the slicer builds the sublayers, so it matches the editor's Effect Preview. The
            // ramp comes back empty for every slot that is not a two component gradient mix.
            const int swatch_sz = FromDIP(20);
            const std::vector<wxColour> gradient_ramp = mixed_gradient_ramp(project_config, cfg_idx, swatch_sz);

            if (!gradient_ramp.empty()) {
                auto* grad_panel = new wxPanel(p->m_panel_mixed_content, wxID_ANY,
                                               wxDefaultPosition, wxSize(swatch_sz, swatch_sz));
                grad_panel->SetMinSize(wxSize(swatch_sz, swatch_sz));
                grad_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
                grad_panel->Bind(wxEVT_PAINT, [grad_panel, gradient_ramp, mix_num](wxPaintEvent&) {
                    wxBufferedPaintDC dc(grad_panel);
                    wxSize sz = grad_panel->GetClientSize();
                    fill_gradient_ramp_rect(dc, wxRect(0, 0, sz.GetWidth(), sz.GetHeight()), gradient_ramp);
                    wxString txt = wxString::Format("%u", mix_num);
                    dc.SetFont(::Label::Body_14);
                    wxSize txt_sz = dc.GetTextExtent(txt);
                    // The number sits at the swatch's middle, so take its contrast from the
                    // colour printed at mid height rather than from either endpoint.
                    dc.SetTextForeground(gradient_ramp[gradient_ramp.size() / 2].GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);
                    dc.DrawText(txt, (sz.GetWidth() - txt_sz.GetWidth()) / 2,
                                     (sz.GetHeight() - txt_sz.GetHeight()) / 2);
                });
                combo_and_btn_sizer->Add(grad_panel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
            } else {
                combo_and_btn_sizer->Add(make_swatch_panel(p->m_panel_mixed_content, mix_col, mix_num),
                                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
            }

            auto* content_panel = new wxPanel(p->m_panel_mixed_content, wxID_ANY);
            content_panel->SetBackgroundColour(mc_bg);
            content_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);

            // Pre-compute all values the paint lambda needs (avoid capturing `this` for FromDIP)
            int cp_pad        = FromDIP(4);
            int cp_swatch_sz  = FromDIP(20);
            int cp_sep_margin = FromDIP(3);
            int cp_pct_left   = FromDIP(2);
            int cp_gap        = FromDIP(2);
            int cp_pct_gap    = FromDIP(4);
            bool cp_is_dark   = wxGetApp().dark_mode();
            bool cp_redesign  = redesign_workspace;
            int  cp_radius    = FromDIP(6);

            // Build per-component colour list for the lambda
            std::vector<wxColour> cp_colours;
            std::vector<bool>     cp_valid;
            std::vector<unsigned int> cp_ids = comp_ids;
            std::vector<int>          cp_ratios = comp_ratios;
            bool cp_is_gradient = is_gradient;
            int  cp_gradient_dir = gradient_direction;
            for (size_t ci = 0; ci < comp_ids.size(); ++ci) {
                bool valid = (comp_ids[ci] >= 1 && comp_ids[ci] <= physical_colors.size());
                cp_valid.push_back(valid);
                cp_colours.push_back(valid ? wxColour(physical_colors[comp_ids[ci] - 1]) : wxColour("#D9D9D9"));
            }

            // Reorder for gradient display: from -> to
            std::vector<unsigned int> draw_ids;
            std::vector<int>          draw_ratios;
            std::vector<wxColour>     draw_colours;
            std::vector<bool>         draw_valid;
            if (cp_is_gradient && cp_ids.size() == 2) {
                int fi = (cp_gradient_dir == 0) ? 0 : 1;
                int ti = 1 - fi;
                draw_ids     = { cp_ids[fi], cp_ids[ti] };
                draw_ratios  = { cp_ratios.size() > (size_t)fi ? cp_ratios[fi] : 0,
                                 cp_ratios.size() > (size_t)ti ? cp_ratios[ti] : 0 };
                draw_colours = { cp_colours[fi], cp_colours[ti] };
                draw_valid   = { cp_valid[fi], cp_valid[ti] };
            } else {
                draw_ids     = cp_ids;
                draw_ratios  = cp_ratios;
                draw_colours = cp_colours;
                draw_valid   = cp_valid;
            }

            content_panel->Bind(wxEVT_PAINT, [content_panel, mc_bg, mc_border, mc_text, mc_dim,
                                               cp_pad, cp_swatch_sz, cp_sep_margin, cp_pct_left,
                                               cp_gap, cp_pct_gap, cp_is_dark, cp_redesign, cp_radius,
                                               cp_is_gradient,
                                               draw_ids, draw_ratios, draw_colours, draw_valid](wxPaintEvent&) {
                wxBufferedPaintDC dc(content_panel);
                wxSize sz = content_panel->GetClientSize();

                dc.SetBrush(wxBrush(mc_bg));
                dc.SetPen(wxPen(mc_border, 1));
                if (cp_redesign)
                    dc.DrawRoundedRectangle(0, 0, sz.GetWidth(), sz.GetHeight(), cp_radius);
                else
                    dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

                dc.SetFont(::Label::Body_13);
                int x = cp_pad;
                int y_swatch = (sz.GetHeight() - cp_swatch_sz) / 2;
                int text_h = dc.GetTextExtent(wxT("A")).GetHeight();
                int y_text = y_swatch + (cp_swatch_sz - text_h) / 2;
                int avail = sz.GetWidth() - cp_pad;
                wxString ellipsis = wxT("...");
                int ellipsis_w = dc.GetTextExtent(ellipsis).GetWidth();

                auto fits = [&](int needed) -> bool {
                    return (x + needed) <= (avail - ellipsis_w);
                };

                size_t n = draw_ids.size();
                for (size_t ci = 0; ci < n; ++ci) {
                    // Separator: "+" or arrow
                    if (ci > 0) {
                        wxString sep = cp_is_gradient ? wxT("\u2192") : wxT("+");
                        int sep_w = dc.GetTextExtent(sep).GetWidth() + cp_sep_margin * 2;
                        if (!fits(sep_w + cp_swatch_sz)) {
                            dc.SetTextForeground(mc_text);
                            dc.DrawText(ellipsis, x, y_text);
                            break;
                        }
                        dc.SetTextForeground(mc_text);
                        dc.DrawText(sep, x + cp_sep_margin, y_text);
                        x += sep_w;
                    }

                    // Swatch
                    if (!fits(cp_swatch_sz)) {
                        dc.SetTextForeground(mc_text);
                        dc.DrawText(ellipsis, x, y_text);
                        break;
                    }

                    if (draw_valid[ci]) {
                        wxColour col = draw_colours[ci];
                        dc.SetBrush(wxBrush(col));
                        dc.SetPen(*wxTRANSPARENT_PEN);
                        dc.DrawRectangle(x, y_swatch, cp_swatch_sz, cp_swatch_sz);
                        if (!cp_is_dark && col.Red() > 224 && col.Green() > 224 && col.Blue() > 224) {
                            dc.SetPen(wxPen(wxColour(130, 130, 128), 1));
                            dc.SetBrush(*wxTRANSPARENT_BRUSH);
                            dc.DrawRectangle(x, y_swatch, cp_swatch_sz, cp_swatch_sz);
                        }
                        if (cp_is_dark && col.Red() < 45 && col.Green() < 45 && col.Blue() < 45) {
                            dc.SetPen(wxPen(wxColour(207, 207, 207), 1));
                            dc.SetBrush(*wxTRANSPARENT_BRUSH);
                            dc.DrawRectangle(x, y_swatch, cp_swatch_sz, cp_swatch_sz);
                        }
                        dc.SetFont(::Label::Body_14);
                        wxString num = wxString::Format("%u", draw_ids[ci]);
                        wxSize num_sz = dc.GetTextExtent(num);
                        dc.SetTextForeground(col.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);
                        dc.DrawText(num, x + (cp_swatch_sz - num_sz.GetWidth()) / 2,
                                         y_swatch + (cp_swatch_sz - num_sz.GetHeight()) / 2);
                        dc.SetFont(::Label::Body_13);
                    } else {
                        dc.SetBrush(wxBrush(mc_bg));
                        dc.SetPen(wxPen(mc_dim, 1));
                        dc.DrawRectangle(x, y_swatch, cp_swatch_sz, cp_swatch_sz);
                        wxString dash = wxT("\u2014");
                        wxSize dash_sz = dc.GetTextExtent(dash);
                        dc.SetTextForeground(mc_dim);
                        dc.DrawText(dash, x + (cp_swatch_sz - dash_sz.GetWidth()) / 2,
                                          y_swatch + (cp_swatch_sz - dash_sz.GetHeight()) / 2);
                    }
                    x += cp_swatch_sz + cp_gap;

                    // Ratio text (skip for gradient)
                    if (!cp_is_gradient) {
                        int r = (ci < draw_ratios.size()) ? draw_ratios[ci] : 0;
                        wxString pct = wxString::Format("%d%%", r);
                        int pct_w = dc.GetTextExtent(pct).GetWidth();
                        if (!fits(pct_w)) {
                            dc.SetTextForeground(mc_text);
                            dc.DrawText(ellipsis, x, y_text);
                            break;
                        }
                        dc.SetTextForeground(mc_text);
                        dc.DrawText(pct, x + cp_pct_left, y_text);
                        x += pct_w + cp_pct_gap;
                    }
                }
            });

            // Tooltip: always show full info
            {
                wxString tip;
                for (size_t ci = 0; ci < draw_ids.size(); ++ci) {
                    if (ci > 0) tip += cp_is_gradient ? wxT(" \u2192 ") : wxT(" + ");
                    int r = (ci < draw_ratios.size()) ? draw_ratios[ci] : 0;
                    tip += wxString::Format("%u (%d%%)", draw_ids[ci], r);
                }
                content_panel->SetToolTip(tip);
            }

            // Repaint on resize so truncation updates
            content_panel->Bind(wxEVT_SIZE, [content_panel](wxSizeEvent& e) {
                content_panel->Refresh();
                e.Skip();
            });

            content_panel->SetCursor(wxCursor(wxCURSOR_HAND));
            size_t panel_idx = i;
            content_panel->Bind(wxEVT_LEFT_UP, [this, panel_idx](wxMouseEvent&) { edit_mixed_filament(panel_idx); });

            combo_and_btn_sizer->Add(content_panel, 1, wxALL | wxEXPAND, FromDIP(2))->SetMinSize({-1, FromDIP(30)});

            auto* menu_btn = new ScalableButton(p->m_panel_mixed_content, wxID_ANY,
                is_broken ? "error" : "menu_filament");
            menu_btn->SetToolTip(is_broken ? _L("Mixed filament has broken component references") : _L("Edit / Delete / Merge"));
            menu_btn->Bind(wxEVT_BUTTON, [this, panel_idx, cfg_idx](wxCommandEvent&) {
                wxMenu menu;

                auto* edit_item = menu.Append(wxID_ANY, _L("Edit"));
                menu.Bind(wxEVT_MENU, [this, panel_idx](wxCommandEvent&) {
                    edit_mixed_filament(panel_idx);
                }, edit_item->GetId());

                wxMenu* sub_menu = new wxMenu();
                std::vector<wxBitmap*> icons = get_extruder_color_icons(true);
                int filaments_cnt = icons.size();
                for (int j = 0; j < filaments_cnt; ++j) {
                    if ((size_t)j == cfg_idx)
                        continue;

                    wxString item_name;
                    bool is_target_mixed = wxGetApp().preset_bundle->is_mixed_filament(j);
                    if (is_target_mixed) {
                        item_name = wxString::Format(_L("Filament %d"), j + 1);
                    } else {
                        auto preset = wxGetApp().preset_bundle->filaments.find_preset(
                            wxGetApp().preset_bundle->filament_presets[j]);
                        item_name = preset ? from_u8(preset->label(false))
                                           : wxString::Format(_L("Filament %d"), j + 1);
                    }

                    auto* mi = new wxMenuItem(sub_menu, wxID_ANY, item_name);
#ifndef __linux__
                    mi->SetBitmap(*icons[j]);
#endif
                    sub_menu->Append(mi);
                    sub_menu->Bind(wxEVT_MENU, [this, cfg_idx, j](wxCommandEvent&) {
                        change_filament(cfg_idx, j);
                    }, mi->GetId());
                }
                if (filaments_cnt > 1)
                    menu.AppendSubMenu(sub_menu, _L("Merge with"));
                else
                    delete sub_menu;

                menu.AppendSeparator(); // ORCA use seperator for reducing accidental clicks to delete

                // ORCA use delete item on end of menu to prevent accidental clicks. clicking to submenus(merge) already not allowed by OS
                auto* del_item = menu.Append(wxID_ANY, _L("Delete"));
                menu.Bind(wxEVT_MENU, [this, panel_idx](wxCommandEvent&) {
                    delete_mixed_filament_at(panel_idx);
                }, del_item->GetId());

                PopupMenu(&menu);
            });
            combo_and_btn_sizer->Add(menu_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));

            combo_and_btn_sizer->Add(FromDIP(16), 0, 0, 0, 0);

            int side = i % 2;
            auto* col = (side == 0) ? left_col : right_col;
            if (side == 1 && i > 1) col->Remove(i / 2);
            col->Add(combo_and_btn_sizer, 1, wxEXPAND);
            if (side == 0 && i > 0) {
                right_col->AddStretchSpacer(1);
            }
        }
    }

    recalc_filament_scroll_sizes();

    p->m_panel_filament_content->FitInside();
    p->m_mixed_scroll_area->FitInside();
    p->scrolled->Layout();
    m_scrolled_sizer->Layout();
    p->scrolled->Layout();

    size_t total = wxGetApp().preset_bundle->filament_presets.size();
    obj_list()->update_objects_list_filament_column(total);

    // Sync mixed filament colors into the config used by 3D view rendering.
    plater->update_filament_colors_in_full_config();
    obj_list()->update_filament_colors();

    // Check if any broken mixed filament is used by objects on current plate.
    // Scan raw extruder assignments (object / volume / height-range / painting)
    // instead of get_extruders() which expands mixed slots and loses their IDs.
    p->m_mixed_filament_broken = false;
    if (!broken_slots.empty()) {
        std::set<size_t> broken_1based;
        for (size_t s : broken_slots) broken_1based.insert(s + 1);

        auto* curr_plate = plater->get_partplate_list().get_curr_plate();
        if (curr_plate) {
            for (auto& obj : plater->model().objects) {
                if (!curr_plate->contain_instance_totally(obj, 0))
                    continue;
                // Check object-level extruder
                int obj_ext = obj->config.has("extruder") ? obj->config.extruder() : 1;
                if (broken_1based.count((size_t)obj_ext)) {
                    p->m_mixed_filament_broken = true;
                    break;
                }
                bool found = false;
                for (auto* vol : obj->volumes) {
                    // Check volume-level extruder
                    int vol_ext = vol->config.has("extruder") ? vol->config.extruder() : obj_ext;
                    if (broken_1based.count((size_t)vol_ext)) { found = true; break; }
                    // Check color painting data (mmu segmentation facets)
                    if (vol->is_model_part() && !vol->mmu_segmentation_facets.empty()) {
                        for (size_t broken_slot : broken_1based) {
                            if (vol->mmu_segmentation_facets.has_facets(*vol, EnforcerBlockerType(broken_slot)))
                                { found = true; break; }
                        }
                        if (found) break;
                    }
                }
                if (found) { p->m_mixed_filament_broken = true; break; }
                // Check height range modifier extruder overrides
                for (auto& [range, cfg] : obj->layer_config_ranges) {
                    if (cfg.has("extruder")) {
                        int layer_ext = cfg.option("extruder")->getInt();
                        if (layer_ext > 0 && broken_1based.count((size_t)layer_ext))
                            { found = true; break; }
                    }
                }
                if (found) { p->m_mixed_filament_broken = true; break; }
            }
        }
    }

    if (plater->canvas3D()) {
        plater->canvas3D()->set_as_dirty();
        plater->get_view3D_canvas3D()->reload_scene(false);
    }

    if (p->m_mixed_filament_broken) {
        auto* mf = wxGetApp().mainframe;
        if (mf)
            mf->update_slice_print_status(MainFrame::eEventObjectUpdate, false);
    }

    if (auto *tab = dynamic_cast<TabPrintPlate *>(wxGetApp().plate_tab))
        tab->update_mixed_filament_seq_state();

}
