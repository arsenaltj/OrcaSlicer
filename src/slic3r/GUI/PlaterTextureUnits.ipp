// Included by Plater.cpp after its native mixed-filament transaction helper.
static void prepare_texture_import_units(Model& model, const TextureImportOptions* options,
                                         bool imperial_units, wxWindow* parent, const boost::filesystem::path& filename)
{
    if (apply_texture_import_units(model, options)) return;
    if (imperial_units) {
        model.convert_from_imperial_units(false);
        return;
    }
    const bool meters = model.looks_like_saved_in_meters();
    if (!meters && !model.looks_like_imperial_units()) return;
    MessageDialog dlg(parent,
        format_wxstr(_L("The object from file %s is too small, and may be in meters or inches.\n Do you want to scale to millimeters\?"), from_path(filename)),
        _L("Object too small"), wxICON_QUESTION | wxYES_NO);
    if (dlg.ShowModal() != wxID_YES) return;
    if (meters) model.convert_from_meters(true);
    else model.convert_from_imperial_units(true);
}
