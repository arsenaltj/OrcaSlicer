// Included inside the Plater translation unit; preserves native project-save semantics.
// BBS: save logic
int GUI::Plater::close_with_confirm(std::function<bool(bool)> second_check)
{
    if (up_to_date(false, false)) {
        if (second_check && !second_check(false)) return wxID_CANCEL;
        model().set_backup_path("");
        return wxID_NO;
    }

    auto choise = wxGetApp().app_config->get("save_project_choise");
    bool remember_choice = false;
    int result;
    if (choise.empty()) {
        RedesignConfirmationOptions options;
        options.style = wxYES_NO | wxCANCEL | wxYES_DEFAULT;
        options.actions = {{wxID_YES, _L("是"), true}, {wxID_NO, _L("否")}, {wxID_CANCEL, _L("取消")}};
        options.checkbox_label = _L("记住我的选择。");
        options.checkbox_state = &remember_choice;
        options.standard_layout = true;
        result = show_redesign_confirmation(static_cast<wxWindow*>(this),
            _L("当前项目包含未保存的修改，是否先保存？"), _L("保存"), options);
    } else {
        result = choise == "yes" ? wxID_YES : wxID_NO;
    }
    if (result == wxID_CANCEL)
        return result;
    else {
        if (remember_choice)
            wxGetApp().app_config->set("save_project_choise", result == wxID_YES ? "yes" : "no");
        if (result == wxID_YES) {
            result = save_project();
            if (result == wxID_CANCEL) {
                if (choise.empty())
                    return result;
                else
                    result = wxID_NO;
            }
        }
    }

    if (second_check && !second_check(result == wxID_YES)) return wxID_CANCEL;

    model().set_backup_path("");
    up_to_date(true, false);
    up_to_date(true, true);

    return result;
}
