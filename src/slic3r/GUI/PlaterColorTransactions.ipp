// Included in Plater's public interface. Implemented in PlaterUndoRedo.ipp.
bool apply_project_config(DynamicPrintConfig config, std::vector<std::string> presets,
    const std::string& action, std::string& error);
bool apply_local_print_colors(ModelVolume& volume, LocalPrintColorCommit::Prepared& painting,
    const PresetBundle* staged, std::string& error);
// One new color-assigned model and its optional material table share an Action.
bool adopt_local_print_model(const ModelObject& object, const PresetBundle* staged,
    size_t& index, std::string& error);
void finish_local_print_model_import(size_t index);
