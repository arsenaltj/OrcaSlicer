// Native workspace appearance and explicit slice cancellation.
void Plater::priv::apply_color_mode()
{
    const bool is_dark         = wxGetApp().dark_mode();
    const bool redesign = main_frame && main_frame->is_redesign_shell_active();
    wxColour   orca_color      = wxColour(59, 68, 70);//wxColour(ColorRGBA::ORCA().r_uchar(), ColorRGBA::ORCA().g_uchar(), ColorRGBA::ORCA().b_uchar());
    orca_color                 = is_dark ? StateColor::darkModeColorFor(orca_color) : StateColor::lightModeColorFor(orca_color);
    wxColour sash_color = redesign ? wxColour(35, 35, 38)
        : is_dark ? wxColour(38, 46, 48) : wxColour(206, 206, 206);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, sash_color);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, *wxWHITE);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_SASH_COLOUR, sash_color);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_BORDER_COLOUR,
        redesign ? wxColour(49, 49, 54) : is_dark ? *wxBLACK : wxColour(165, 165, 165));
}

bool Plater::cancel_current_slice()
{
    if (p->background_process.idle()) return false;
    p->background_process.stop();
    return true;
}
