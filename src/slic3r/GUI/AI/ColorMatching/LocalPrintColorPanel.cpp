#include "LocalPrintColorPanel.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintColorApplication.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintColorCommit.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintRecipeTransition.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintColorRestore.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintColorLayers.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreview3D.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorQuality.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorBoundaryRefinement.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintColorState.hpp"
#include "slic3r/GUI/AI/Model/BeautyPrintColorHandoff.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchControls.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticDraft.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalSemanticWorkerClient.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem/fstream.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/colordlg.h>
#include <wx/filedlg.h>
#include <wx/spinctrl.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <atomic>
#include <thread>

namespace Slic3r::GUI {
namespace Matching = LocalPrintColorMatching;
namespace fs = boost::filesystem;
namespace {
wxString u8(const std::string& text) { return wxString::FromUTF8(text.c_str()); }
fs::path version_directory() { return fs::path(data_dir()) / "local_color_versions"; }
fs::path record_for(const std::string& source)
{
    const fs::path path(source);
    if (path.stem().string().find("orca-color-") != 0) return {};
    return version_directory() / (path.stem().string() + ".json");
}
nlohmann::json read_record(const fs::path& path)
{
    boost::system::error_code error;
    const auto bytes = fs::file_size(path, error);
    if (error || bytes > 128ULL * 1024 * 1024) throw std::runtime_error("Color version is missing or too large.");
    fs::ifstream stream(path);
    auto record = nlohmann::json::parse(stream);
    if (record.at("schema") != "orcaslicer.local-color-version.v1") throw std::runtime_error("Unsupported color version.");
    return record;
}
}

struct LocalPrintColorPanel::Impl {
    Impl(LocalPrintColorPanel* page, Plater* workspace, std::function<void()> navigation,
         std::function<void()> back_to_workbench)
        : owner(page), plater(workspace), adapter(workspace, {}), navigate(std::move(navigation))
    {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* top = new wxBoxSizer(wxHORIZONTAL);
        if (back_to_workbench) {
            add_button(top, _L("返回美颜工具"), std::move(back_to_workbench));
        } else {
        add_button(top, _L("打开准备页选中模型"), [this] { open_selected(); });
        add_button(top, _L("打开本地彩色模型"), [this] {
            if (busy) return;
            wxFileDialog dialog(owner, _L("打开彩色模型"), {}, {}, "Color models (*.glb;*.obj)|*.glb;*.obj", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() == wxID_OK) {
                AI::GeneratedModelArtifact item; item.local_path = fs::path(dialog.GetPath().ToUTF8().data()); open(item);
            }
        });
        }
        layer_button = add_button(top, _L("核验模型层序"), [this] { inspect_layers(); });
        layer_cancel_button = add_button(top, _L("取消层序核验"), [this] {
            if (layer_busy) { cancel = true; refresh(); message(_L("正在取消层序核验，准备页未修改。")); }
        });
        top->AddStretchSpacer();
        add_button(top, _L("返回准备页"), [this] { if (navigate) navigate(); });
        root->Add(top, 0, wxEXPAND | wxALL, owner->FromDIP(8));
        auto* content = new wxBoxSizer(wxHORIZONTAL);
        preview = new ModelPreview3D(owner, true);
        preview->set_preview_background(wxColour(184, 184, 184));
        content->Add(preview, 1, wxEXPAND | wxALL, owner->FromDIP(8));
        auto* sidebar = new wxScrolledWindow(owner, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        sidebar->SetMinSize(owner->FromDIP(wxSize(405, 100))); sidebar->SetScrollRate(0, owner->FromDIP(16));
        controls_parent = sidebar;
        auto* controls = new wxBoxSizer(wxVERTICAL);
        controls->Add(new wxStaticText(sidebar, wxID_ANY, _L("本地颜色匹配")), 0, wxALL, owner->FromDIP(6));
        controls->Add(new wxStaticText(sidebar, wxID_ANY, _L("目标颜色数（与实体耗材数量独立）")), 0, wxALL, owner->FromDIP(6));
        count = new wxSpinCtrl(sidebar, wxID_ANY); count->SetRange(1, 32); count->SetValue(6);
        controls->Add(count, 0, wxEXPAND | wxALL, owner->FromDIP(6));
        count->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
            if (busy || input.faces.empty()) return;
            checkpoint(); input.identity.requested_color_count = size_t(count->GetValue()); compute();
        });
        add_button(controls, _L("按当前耗材重新匹配"), [this] { if (!busy && !input.faces.empty()) compute(); });
        original = new wxCheckBox(sidebar, wxID_ANY, _L("对照模型原色"));
        controls->Add(original, 0, wxALL, owner->FromDIP(6));
        original->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { display(); });
        auto* views = new wxBoxSizer(wxHORIZONTAL);
        add_button(views, _L("整体视角"), [this] { preview->reset_view(); });
        add_button(views, _L("正面视角"), [this] { preview->front_view(); });
        controls->Add(views, 0, wxEXPAND);
        select = new wxCheckBox(sidebar, wxID_ANY, _L("框选局部（关闭后旋转）"));
        controls->Add(select, 0, wxALL, owner->FromDIP(6));
        select->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            preview->set_selection_gesture(ModelPreview3D::SelectionGesture::Lasso);
            preview->set_selection_enabled(select->GetValue());
        });
        slots = new wxChoice(sidebar, wxID_ANY); controls->Add(slots, 0, wxEXPAND | wxALL, owner->FromDIP(6));
        add_button(controls, _L("选区锁定到此耗材"), [this] { edit_selection(true); });
        add_button(controls, _L("保护选区颜色"), [this] { edit_selection(false); });
        add_button(controls, _L("选区指定颜色"), [this] {
            if (busy) return;
            const auto faces = preview->selected_face_indices();
            if (faces.empty()) { message(_L("请先框选需要修改的表面。")); return; }
            wxColourDialog dialog(owner);
            if (dialog.ShowModal() != wxID_OK) return;
            checkpoint(); const auto color = dialog.GetColourData().GetColour();
            auto& edits = input.identity.user_overrides;
            const std::set<size_t> selected(faces.begin(), faces.end());
            BeautyPrintColorHandoff::unlock_faces(input.identity.regions, selected);
            edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const auto& item) { return selected.count(item.first); }), edits.end());
            edits.reserve(edits.size() + faces.size());
            for (size_t face : faces) {
                edits.push_back({face, {color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f}});
            }
            compute();
        });
        auto* history = new wxBoxSizer(wxHORIZONTAL);
        add_button(history, _L("撤销配色"), [this] { restore(undo, redo); });
        add_button(history, _L("重做配色"), [this] { restore(redo, undo); });
        controls->Add(history, 0, wxEXPAND);
        add_button(controls, _L("清除局部调整，恢复原色意图"), [this] {
            if (busy || input.faces.empty()) return;
            checkpoint(); input.identity.regions.clear(); input.identity.contrasts.clear(); input.identity.user_overrides.clear(); compute();
        });
        report = new wxTextCtrl(sidebar, wxID_ANY, _L("从生成页进入，或打开准备页选中模型。"), wxDefaultPosition,
            owner->FromDIP(wxSize(370, 230)), wxTE_MULTILINE | wxTE_READONLY);
        controls->Add(report, 1, wxEXPAND | wxALL, owner->FromDIP(6));
        accept_error = new wxCheckBox(sidebar, wxID_ANY, _L("接受预测色差（仍须全部分配）"));
        controls->Add(accept_error, 0, wxALL, owner->FromDIP(6));
        accept_error->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { refresh(); });
        apply_button = add_button(controls, _L("确认配色并应用到准备页"), [this] { apply(false); });
        defer_button = add_button(controls, _L("稍后匹配，保留原色资产"), [this] { apply(true); });
        add_button(controls, _L("取消本次调整"), [this] {
            if (busy) { cancel = true; message(_L("正在取消计算，准备页未修改。")); return; }
            if (!input.faces.empty()) {
                input.identity = initial; undo.clear(); redo.clear();
                count->SetValue(int(input.identity.requested_color_count)); compute();
            }
            if (navigate) navigate();
        });
        sidebar->SetSizer(controls); sidebar->FitInside();
        content->Add(sidebar, 0, wxEXPAND | wxALL, owner->FromDIP(8));
        root->Add(content, 1, wxEXPAND); owner->SetSizer(root); refresh();
    }

    wxButton* add_button(wxSizer* sizer, const wxString& label, std::function<void()> action)
    {
        auto* button = new wxButton(controls_parent ? controls_parent : owner, wxID_ANY, label);
        button->Bind(wxEVT_BUTTON, [action = std::move(action)](wxCommandEvent&) { action(); });
        sizer->Add(button, 0, wxALL | wxEXPAND, owner->FromDIP(4)); return button;
    }
    void message(const wxString& text) { report->SetValue(text); }
    void refresh()
    {
        layer_button->Enable(!busy && target_volume.valid());
        layer_cancel_button->Enable(layer_busy && !cancel.load());
        count->Enable(!busy); slots->Enable(!busy); select->Enable(!busy);
        const bool covered = has_result && std::all_of(computed.targets.begin(), computed.targets.end(), [](const auto& target) { return target.executable; });
        const bool physical_application = std::none_of(computed.targets.begin(), computed.targets.end(),
            [](const auto& target) { return bool(target.recipe); });
        accept_error->Enable(!busy && covered);
        apply_button->Enable(!busy && covered && physical_application && (computed.unresolved_count() == 0 || accept_error->GetValue()));
        defer_button->Enable(!busy && !input.faces.empty());
    }
    void shutdown() { stopped = true; ++operation_revision; cancel = true; if (worker.joinable()) worker.join(); }
    void checkpoint() { undo.push_back(input.identity); redo.clear(); }
    void restore(std::vector<AI::LocalPrintColorResult>& from, std::vector<AI::LocalPrintColorResult>& to)
    {
        if (busy || from.empty()) return;
        to.push_back(input.identity); input.identity = std::move(from.back()); from.pop_back();
        count->SetValue(int(input.identity.requested_color_count)); compute();
    }
    void edit_selection(bool lock)
    {
        if (busy || !has_result) return;
        const auto faces = preview->selected_face_indices();
        if (faces.empty()) { message(_L("请先框选需要保护或锁定的表面。")); return; }
        if (lock && slots->GetSelection() == wxNOT_FOUND) return;
        checkpoint();
        AI::PrintColorRegion region;
        region.id = "manual-" + fs::unique_path("%%%%%%%%-%%%%%%%%").string();
        region.label = lock ? "manual material lock" : "manual protected region";
        region.faces = faces; region.user_protected = true; region.confidence = 1;
        if (lock) {
            region.locked_physical_slot = snapshot.physical_channels[size_t(slots->GetSelection())].slot;
            // A new explicit lock replaces earlier material locks on its faces.
            const std::set<size_t> selected(faces.begin(), faces.end());
            for (auto& prior : input.identity.regions) if (prior.locked_physical_slot)
                prior.faces.erase(std::remove_if(prior.faces.begin(), prior.faces.end(), [&](size_t f) { return selected.count(f); }), prior.faces.end());
        }
        input.identity.regions.push_back(std::move(region)); compute();
    }

    void open_selected()
    {
        if (busy || plater == nullptr) return;
        const int index = plater->get_selected_object_idx();
        if (index < 0 || size_t(index) >= plater->model().objects.size()) { message(_L("请在准备页选择一个模型。")); return; }
        auto* object = plater->model().objects[size_t(index)];
        if (object->volumes.size() != 1 || !object->volumes.front()->is_model_part()) {
            message(_L("目前请选中只有一个实体部件的模型；多部件需要逐部件处理。")); return;
        }
        auto* volume = object->volumes.front();
        try {
            const auto path = record_for(volume->source.input_file);
            if (path.empty()) {
                const fs::path source(volume->source.input_file);
                if (!fs::is_regular_file(source) || !AI::is_model_artifact(source)) {
                    message(_L("此模型的原始彩色文件不可用。请从生成页或本地原始 GLB / OBJ 打开后匹配。")); return;
                }
                AI::GeneratedModelArtifact item; item.local_path = source; open(item, volume->id()); return;
            }
            const auto record = read_record(path);
            AI::GeneratedModelArtifact item; item.local_path = record.at("source_path").get<std::string>();
            open(item, volume->id(), record);
        } catch (const std::exception& error) { message(u8(error.what())); }
    }

    void open(const AI::GeneratedModelArtifact& item, ObjectID destination = {}, nlohmann::json saved = {})
    {
        if (busy || stopped) { message(_L("请等待当前本地计算完成后再打开模型。")); return; }
        std::string title, parent_version;
        try {
            // A first-time import has no version record. JSON's default value
            // is null, and value() only accepts objects.
            if (saved.is_null()) saved = nlohmann::json::object();
            if (!saved.is_object()) throw std::runtime_error("Invalid color version record.");
            title = saved.value("source_name", item.local_path.stem().string());
            parent_version = saved.value("version_id", "");
        } catch (const std::exception& error) { message(u8(error.what())); return; }
        if (worker.joinable()) worker.join();
        busy = true; cancel = false; const auto revision = ++operation_revision; refresh();
        message(_L("正在读取原色模型和表面数据……"));
        auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
        auto mesh = std::make_shared<indexed_triangle_set>();
        auto native = std::make_shared<indexed_triangle_set>();
        auto colors = std::make_shared<std::vector<RGBA>>();
        auto samples = std::make_shared<std::vector<Matching::FaceSample>>();
        auto restored = std::make_shared<std::optional<AI::LocalPrintColorResult>>();
        auto beauty = std::make_shared<std::optional<AI::ModelMatchedColors>>();
        auto destination_mesh = std::make_shared<indexed_triangle_set>();
        auto destination_paint = std::make_shared<TriangleSelector::TriangleSplittingData>();
        auto saved_paint_matches = std::make_shared<bool>(!destination.valid());
        // Capture native state on the UI thread; recovery in the worker must
        // never read a changing live PresetBundle or borrow saved evidence.
        const auto restore_snapshot = adapter.printable_palette();
        std::shared_ptr<PresetBundle> restore_bundle;
        PrintColorNozzleRouting restore_routing;
        if (destination.valid() && wxGetApp().preset_bundle) {
            restore_bundle = std::make_shared<PresetBundle>(*wxGetApp().preset_bundle);
            if (const auto* plate = plater->get_partplate_list().get_curr_plate()) {
                restore_routing.mode = plate->get_real_filament_map_mode(restore_bundle->project_config);
                restore_routing.filament_maps = plate->get_real_filament_maps(restore_bundle->project_config);
            }
        }
        std::string expected_source; ObjectBase::Timestamp expected_paint = 0, expected_config = 0;
        if (destination.valid()) for (const auto* object : plater->model().objects)
            for (const auto* volume : object->volumes) if (volume->id() == destination) {
                expected_source = volume->source.input_file;
                *destination_mesh = volume->mesh().its;
                *destination_paint = volume->mmu_segmentation_facets.get_data();
                expected_paint = volume->mmu_segmentation_facets.timestamp(); expected_config = static_cast<const ObjectBase&>(volume->config).timestamp();
            }
        wxWeakRef<LocalPrintColorPanel> weak(owner);
        worker = std::thread([this, weak, item = AI::GeneratedModelArtifact(item), destination, saved, title, parent_version, prepared, mesh, native, colors, samples, restored,
                              expected_source, expected_paint, expected_config, destination_mesh, destination_paint, saved_paint_matches,
                              restore_snapshot, restore_bundle, restore_routing, beauty, revision]() mutable {
            std::string error, hash;
            try {
                hash = AI::model_artifact_sha256(item.local_path);
                if (!saved.empty() && saved.at("source_sha256") != hash) throw std::runtime_error("Original color asset changed.");
                // Read accepted metadata before archiving the GLB under its hash:
                // that copy deliberately has no neighboring history record.
                if (saved.empty() && !destination.valid()) {
                    AI::ModelImportRequest request;
                    BeautyWorkbenchControls::prepare_import(item.local_path, request);
                    *beauty = std::move(request.matched_colors);
                }
                // Generated GLBs can live in a generation download directory.
                // Keep their original bytes independently of subsequent cleanup.
                if (AI::model_artifact_format(item.local_path) == "glb") {
                    const auto assets = version_directory() / "originals"; fs::create_directories(assets);
                    const auto preserved = assets / (hash + ".glb");
                    if (!fs::exists(preserved)) fs::copy_file(item.local_path, preserved);
                    if (AI::model_artifact_sha256(preserved) != hash) throw std::runtime_error("Preserved original hash mismatch.");
                    item.local_path = preserved;
                }
                if (!ModelPreview3D::prepare_model(item.local_path, *prepared, error, {}, {}, false)) throw std::runtime_error(error);
                *native = prepared->mesh;
                if ((!saved.empty() && saved.at("binding_geometry_id") != prepared->geometry_id) ||
                    (destination.valid() && !LocalPrintColorApplication::same_surface_partition(*native, *destination_mesh)))
                    throw std::runtime_error("The prepared model geometry changed; saved face groups cannot be reused.");
                // Preserve face order and exact positions; coincident duplicated
                // vertices retain the source geometry fingerprint.
                const auto& vertices = prepared->geometry.vertices;
                for (size_t f = 0; f < prepared->triangles; ++f) {
                    AI::PrintRgb mean {};
                    for (size_t c = 0; c < 3; ++c) {
                        const size_t offset = (f * 3 + c) * 8;
                        mesh->vertices.emplace_back(vertices[offset], vertices[offset + 1], vertices[offset + 2]);
                        const auto packed = uint32_t(vertices[offset + 6]);
                        RGBA rgb {float((packed >> 16) & 255)/255.f, float((packed >> 8) & 255)/255.f, float(packed & 255)/255.f, 1.f};
                        colors->push_back(rgb);
                        for (size_t ch = 0; ch < 3; ++ch) mean[ch] += rgb[ch] / 3.f;
                    }
                    mesh->indices.emplace_back(int(f * 3), int(f * 3 + 1), int(f * 3 + 2));
                    const auto& a = mesh->vertices[f * 3]; const auto& b = mesh->vertices[f * 3 + 1]; const auto& c = mesh->vertices[f * 3 + 2];
                    samples->push_back({mean, double((b-a).cross(c-a).norm()) / 2});
                }
                if (AI::model_artifact_sha256(item.local_path) != hash) throw std::runtime_error("Source changed while loading.");
                if (AI::SurfaceSelectionPersistence::geometry_fingerprint(*mesh) != prepared->geometry_id)
                    throw std::runtime_error("Preview face order does not match source geometry.");
                if (saved.contains("result") && !saved["result"].is_null()) {
                    const auto& record = saved["result"]; AI::LocalPrintColorResult value;
                    if (!LocalPrintColorState::decode(record, hash, prepared->geometry_id, record.at("material_fingerprint").get<std::string>(),
                        record.at("process_fingerprint").get<std::string>(), value, error, record.at("algorithm_version").get<std::string>()))
                        throw std::runtime_error(error);
                    const bool has_recipes = std::any_of(value.targets.begin(), value.targets.end(),
                        [](const auto& target) { return bool(target.recipe); });
                    if (has_recipes) *saved_paint_matches = false;
                    if (destination.valid() && value.confirmed && !cancel) {
                        if (has_recipes && restore_bundle) {
                            auto current = value;
                            current.physical_channels = restore_snapshot.physical_channels;
                            current.material_fingerprint = restore_snapshot.material_fingerprint;
                            current.process_fingerprint = restore_snapshot.process_fingerprint;
                            AI::LocalPrintColorResult verified; std::string reason;
                            *saved_paint_matches = LocalPrintColorRestore::restore(value, current, restore_snapshot,
                                *restore_bundle, restore_routing, TriangleMesh(*destination_mesh), *native,
                                *destination_paint, {}, verified, reason);
                        } else if (!has_recipes)
                            *saved_paint_matches = LocalPrintColorApplication::matches_saved_painting(
                                TriangleMesh(*destination_mesh), *native, *destination_paint, value);
                    }
                    *restored = std::move(value);
                }
            } catch (const std::exception& exception) { error = exception.what(); }
            wxGetApp().CallAfter([this, weak, item, destination, title, parent_version, prepared, mesh, native, colors, samples, hash, error, restored,
                                   expected_source, expected_paint, expected_config, saved_paint_matches, restore_snapshot, beauty, revision] {
                if (!weak || stopped || revision != operation_revision) return;
                if (worker.joinable()) worker.join(); busy = false;
                if (cancel || !error.empty()) { message(cancel ? _L("已取消，准备页未修改。") : u8(error)); refresh(); return; }
                if (destination.valid()) {
                    const ModelVolume* current = nullptr;
                    for (const auto* object : plater->model().objects)
                        for (const auto* volume : object->volumes) if (volume->id() == destination) current = volume;
                    if (!current || current->source.input_file != expected_source ||
                        current->mmu_segmentation_facets.timestamp() != expected_paint ||
                        static_cast<const ObjectBase&>(current->config).timestamp() != expected_config ||
                        !LocalPrintColorApplication::same_surface_partition(*native, current->mesh().its)) {
                        message(_L("读取期间准备页模型或配色已改变，请重新打开当前模型。")); refresh(); return;
                    }
                }
                size_t triangles = 0, color_count = 0; Vec3d dimensions; std::string load_error;
                if (!preview->load_prepared_model(std::move(*prepared), {}, triangles, dimensions, color_count, load_error)) {
                    message(u8(load_error)); refresh(); return;
                }
                preview->set_color_controls_visible(false);
                preview->reset_view();
                artifact = item; model_name = title; target_volume = destination; source_mesh = std::move(*mesh); source_colors = std::move(*colors);
                native_mesh = std::move(*native);
                semantic_evidence.reset(); semantic_notice.clear();
                target_source = expected_source; target_paint_stamp = expected_paint; target_config_stamp = expected_config;
                input = {}; input.faces = std::move(*samples);
                input.identity.requested_color_count = size_t(count->GetValue());
                if (*restored) input.identity = **restored;
                // Actual loaded identities always win over historical fields.
                input.identity.source_sha256 = hash; input.identity.geometry_id = preview->geometry_id();
                input.identity.face_count = triangles; input.identity.source_color_count = color_count;
                has_result = false;
                input.identity.parent_version = parent_version;
                initial = input.identity; undo.clear(); redo.clear(); count->SetValue(int(input.identity.requested_color_count));
                original->SetValue(false); accept_error->SetValue(false);
                refresh_palette();
                if (*beauty) {
                    try {
                        BeautyPrintColorHandoff::seed(**beauty, input.identity);
                        initial = input.identity;
                        count->SetValue(int(input.identity.requested_color_count));
                    } catch (const std::exception& failure) {
                        // Do not leave a usable unprotected draft after a stale
                        // handoff. Reopening is required; ordinary actions must
                        // not silently bypass the rejected saved assignments.
                        input = {}; has_result = false;
                        message(u8(failure.what())); refresh(); return;
                    }
                }
                // The worker checked recipes against an immutable native
                // capture. Invalidate that result if live evidence changed.
                if (*restored && std::any_of((**restored).targets.begin(), (**restored).targets.end(),
                        [](const auto& target) { return bool(target.recipe); }) &&
                    (snapshot.material_fingerprint != restore_snapshot.material_fingerprint ||
                     snapshot.process_fingerprint != restore_snapshot.process_fingerprint ||
                     snapshot.material_metadata_complete != restore_snapshot.material_metadata_complete ||
                     snapshot.sublayer_process.sublayers_enabled != restore_snapshot.sublayer_process.sublayers_enabled ||
                     LocalPrintRecipeProofState::context(snapshot.sublayer_materials, snapshot.sublayer_process) !=
                         LocalPrintRecipeProofState::context(restore_snapshot.sublayer_materials, restore_snapshot.sublayer_process)))
                    *saved_paint_matches = false;
                std::string restore_reason;
                if (*restored && LocalPrintColorState::restore_confirmed(**restored, input.identity,
                        *saved_paint_matches, computed, restore_reason)) {
                    // Keep the exact historical partition and algorithm version.
                    // Its previous confirmation includes accepting recorded error.
                    has_result = true; restored_confirmation = true;
                    accept_error->SetValue(computed.unresolved_count() != 0);
                    display(); refresh();
                } else compute(!beauty->has_value());
            });
        });
    }

    void refresh_palette()
    {
        layer_notice.clear();
        snapshot = adapter.printable_palette(); slots->Clear();
        for (const auto& slot : snapshot.physical_channels)
            slots->Append(wxString::Format(_L("耗材 %d  "), int(slot.slot + 1)) + u8(slot.display_color + "  " + slot.material_type));
        if (!snapshot.physical_channels.empty()) slots->SetSelection(0);
        input.identity.physical_channels = snapshot.physical_channels;
        if (!snapshot.material_metadata_complete)
            for (auto& channel : input.identity.physical_channels) channel.compatible = false;
        input.identity.material_fingerprint = snapshot.material_fingerprint;
        input.identity.process_fingerprint = snapshot.process_fingerprint;
        input.tolerance = input.identity.color_tolerance; input.important_area_floor = input.identity.important_area_floor;
    }

    struct LayerRequest {
        std::shared_ptr<Model> model;
        DynamicPrintConfig config;
        std::string identity;
        Vec3d plate_origin; int plate_index {0};
        ObjectID target_id;
        std::vector<size_t> target_labels;
        std::vector<std::optional<size_t>> target_slots;
    };
    LayerRequest layer_request() const
    {
        if (!plater || !target_volume.valid() || !wxGetApp().preset_bundle)
            throw std::runtime_error("Open a preparation-page model before inspecting its layers.");
        const ModelObject* source = nullptr;
        for (const auto* object : plater->model().objects)
            for (const auto* volume : object->volumes) if (volume->id() == target_volume) source = object;
        if (!source) throw std::runtime_error("The target model was removed.");
        auto* bundle = wxGetApp().preset_bundle;
        auto* plate = plater->get_partplate_list().get_curr_plate();
        if (!plate) throw std::runtime_error("No active plate exists.");
        const auto position=std::find(plater->model().objects.begin(),plater->model().objects.end(),source);
        const int object_index=int(std::distance(plater->model().objects.begin(),position));
        for(size_t i=0;i<source->instances.size();++i)
            if(!plate->contain_instance(object_index,int(i)))
                throw std::runtime_error("Layer inspection requires the target object's instances to belong to the active plate.");
        auto maps = plate->get_real_filament_maps(bundle->project_config);
        auto volume_maps = plate->get_filament_volume_maps();
        if (volume_maps.empty()) volume_maps = bundle->get_default_nozzle_volume_types_for_filaments(maps);
        LayerRequest request;
        request.config = bundle->get_printer_extruder_count() > 1 ? bundle->full_config(false,maps,volume_maps) : bundle->full_config(false);
        request.config.apply(*plate->config());
        request.plate_origin=plate->get_origin();request.plate_index=plate->get_index();
        request.identity = LocalPrintRecipeProofState::digest({{"object",OrcaPrintColorLayers::identity(*source,request.config)},
            {"plate",request.plate_index},{"origin",{request.plate_origin.x(),request.plate_origin.y(),request.plate_origin.z()}}});
        if(has_result) {
            std::string error;
            if(!computed.valid(error))throw std::runtime_error(error);
            const ModelVolume* volume=nullptr;
            for(const auto* candidate:source->volumes)if(candidate->id()==target_volume)volume=candidate;
            if(!volume || !volume->is_model_part() || !LocalPrintColorApplication::same_surface_partition(native_mesh,volume->mesh().its))
                throw std::runtime_error("The logical color partition no longer matches the target surface.");
            request.target_id=target_volume;request.target_labels=computed.face_targets;
            nlohmann::json slots=nlohmann::json::array();
            for(const auto& target:computed.targets) {
                request.target_slots.push_back(target.physical_slot);
                slots.push_back(target.physical_slot ? nlohmann::json(*target.physical_slot) : nlohmann::json(nullptr));
            }
            request.identity=LocalPrintRecipeProofState::digest({{"native",request.identity},{"target_volume",target_volume.id},
                {"logical_faces",request.target_labels},{"logical_slots",std::move(slots)}});
        }
        request.model = OrcaPrintColorLayers::snapshot_model(*source);
        if(!request.target_labels.empty())for(size_t index=0;index<source->volumes.size();++index)
            if(source->volumes[index]->id()==target_volume)request.target_id=request.model->objects.front()->volumes[index]->id();
        return request;
    }
    void inspect_layers()
    {
        if (busy || stopped) return;
        try {
            auto request = layer_request();
            if (worker.joinable()) worker.join();
            cancel=false;busy=true;layer_busy=true;layer_notice.clear();refresh();
            message(_L("正在模型副本上核验实际层序，可取消；准备页未修改。"));
            const auto revision=++operation_revision;
            wxWeakRef<LocalPrintColorPanel> weak(owner);
            worker=std::thread([this,weak,revision,request=std::move(request)] {
                nlohmann::json result;std::string error;
                try {
                    std::optional<OrcaPrintColorTargets::Partition> logical;
                    if(!request.target_labels.empty()) {
                        for(const auto* object:request.model->objects)for(const auto* volume:object->volumes)
                            if(volume->id()==request.target_id)logical.emplace(OrcaPrintColorTargets::partition(*volume,request.target_labels,request.target_slots,cancel));
                        if(!logical)throw std::runtime_error("The logical target snapshot is missing.");
                    }
                    result=OrcaPrintColorLayers::inspect(*request.model,request.config,cancel,request.plate_origin,request.plate_index,logical ? &*logical : nullptr);
                    result["input_identity"]=request.identity;
                }
                catch(const std::exception& e){error=e.what();}
                wxGetApp().CallAfter([this,weak,revision,result=std::move(result),identity=request.identity,error] {
                    if(!weak || stopped || revision!=operation_revision) return;
                    if(worker.joinable())worker.join();busy=false;layer_busy=false;refresh();
                    if(cancel || !error.empty()){message(cancel ? _L("层序核验已取消，准备页未修改。") : u8(error));return;}
                    try {
                        if(layer_request().identity!=identity) throw std::runtime_error("Model or process changed during layer inspection. Inspect again.");
                        const auto directory=version_directory()/"layer_inspections";fs::create_directories(directory);
                        const auto path=directory/(fs::unique_path("layers-%%%%%%%%-%%%%%%%%").string()+".json");
                        {fs::ofstream stream(path);stream<<result.dump(2);stream.close();if(!stream)throw std::runtime_error("Cannot save layer inspection.");}
                        double minimum=std::numeric_limits<double>::infinity(),maximum=0.;
                        for(const auto& object:result.at("objects"))for(const auto& layer:object.at("layers")) {
                            const double h=layer.at("height_mm");minimum=std::min(minimum,h);maximum=std::max(maximum,h);
                        }
                        layer_notice=wxString::Format(_L("本次层序快照：%d 层，有模型几何 %d 层；层厚 %.6f–%.6f mm。\n"),
                            result.at("layer_count").get<int>(),result.at("occupied_layer_count").get<int>(),minimum,maximum);
                        layer_notice+=wxString::Format(_L("已记录 %d 个材料区域层的轮廓和孔洞；按配置外墙线宽内缩后，%d 个区域岛无剩余截面。\n"),
                            result.at("region_layer_count").get<int>(),result.at("islands_without_inset_core").get<int>());
                        size_t projected=0;
                        for(const auto& object:result.at("objects"))if(object.contains("logical_projection"))
                            projected+=object.at("logical_projection").at("region_layer_count").get<size_t>();
                        if(projected)layer_notice+=wxString::Format(_L("逻辑目标已投影到 %d 个实际材料区域层；分组保持独立，共用耗材不会合并目标。投影差异已记录，尚未证明配方可打印。\n"),int(projected));
                        layer_notice+=_L("此项不代表可变线宽或叠色可打印；逻辑颜色区域对应及最终工具分配仍待核验。修改模型或工艺后请重新核验。\n");
                        display();
                    }catch(const std::exception& e){message(u8(e.what()));}
                });
            });
        }catch(const std::exception& e){busy=false;layer_busy=false;refresh();message(u8(e.what()));}
    }

    void compute(bool recognize_details = false)
    {
        if (busy || input.faces.empty() || stopped) return;
        if (worker.joinable()) worker.join();
        refresh_palette();
        cancel = false; busy = true; has_result = false; restored_confirmation = false;
        const auto revision = ++operation_revision;
        accept_error->SetValue(false); refresh();
        message(recognize_details ? _L("正在检查本地细节识别并匹配颜色，可取消……") : _L("正在计算逐面色差与整体耗材分配……"));
        auto task = input; task.cancelled = [this] { return cancel.load(); };
        auto recipe_input = LocalPrintColorRecipes::from_workspace(snapshot);
        recipe_input.cancelled = task.cancelled;
        // Freeze topology with the surface/color snapshot. The worker never
        // consults mutable preparation-page geometry while refining boundaries.
        auto topology = std::make_shared<const indexed_triangle_set>(native_mesh);
        const auto source = artifact.local_path;
        const auto config_path = fs::path(data_dir()) / "local_semantic_runtime.json";
        const auto module_directory = fs::path(resources_dir()) / "tools" / "ai";
        const auto temporary_parent = version_directory() / "semantic_requests";
        const auto semantic_cache = version_directory() / "semantic_cache";
        const auto expected_source = input.identity.source_sha256, expected_geometry = input.identity.geometry_id;
        wxWeakRef<LocalPrintColorPanel> weak(owner);
        worker = std::thread([this, weak, task = std::move(task), recipe_input = std::move(recipe_input), topology, source, config_path, module_directory,
                             temporary_parent, semantic_cache, recognize_details, revision, expected_source, expected_geometry]() mutable {
            Matching::Computation result;
            std::shared_ptr<const LocalSemanticEvidence::Evidence> evidence;
            wxString notice;
            fs::path owned_request;
            try {
                if (recognize_details) {
                    try {
                        LocalSemanticWorker::Configuration config; std::string reason;
                        if (!fs::exists(config_path)) notice = _L("未启用本地细节识别，按原色匹配。\n");
                        else if (!LocalSemanticWorker::read_configuration(config_path, config, reason))
                            notice = _L("本地识别配置不可用，按原色匹配。\n");
                        else if (!config.enabled) notice = _L("本地细节识别已关闭，按原色匹配。\n");
                        else if (AI::model_artifact_format(source) != "glb")
                            notice = _L("此文件暂不支持本地细节识别，按原色匹配。\n");
                        else {
                            fs::create_directories(temporary_parent);
                            const auto request = temporary_parent / fs::unique_path("page-%%%%-%%%%-%%%%");
                            if (!fs::create_directory(request)) throw std::runtime_error("Unable to create an owned local request.");
                            owned_request = request;
                            auto analyzed = LocalSemanticWorker::analyze(config, module_directory, owned_request, source, *topology, cancel, semantic_cache);
                            if (analyzed.process.status == LocalSemanticWorker::Status::Cancelled) result.cancelled = true;
                            else if (analyzed.process.status == LocalSemanticWorker::Status::Ready) {
                                std::vector<AI::PrintColorRegion> merged;
                                if (LocalSemanticDraft::merge_regions(task.identity, analyzed.evidence, merged, reason)) {
                                    task.identity.regions = std::move(merged);
                                    evidence = std::make_shared<const LocalSemanticEvidence::Evidence>(std::move(analyzed.evidence));
                                    if (analyzed.cache_hit) notice = _L("已复用通过校验的本地识别结果，本次未运行识别模型。\n");
                                } else notice = _L("识别结果与当前局部设置不兼容，保留现有设置匹配。\n");
                            } else if (analyzed.process.status == LocalSemanticWorker::Status::TimedOut)
                                notice = _L("本地细节识别超时，按现有原色和局部设置匹配。\n");
                            else notice = _L("本地细节识别未完成，按现有原色和局部设置匹配。\n");
                        }
                    } catch (const std::exception&) {
                        notice = _L("本地细节识别不可用，按现有原色和局部设置匹配。\n");
                    }
                }
                if (cancel.load()) result.cancelled = true;
                if (!result.cancelled && AI::print_color_mode(task.identity.requested_color_count) == AI::PrintColorMode::Layered) {
                    auto catalog = LocalPrintColorRecipes::enumerate(recipe_input);
                    if (catalog.cancelled) result.cancelled = true;
                    task.recipe_catalog = std::make_shared<const LocalPrintColorRecipes::Catalog>(std::move(catalog));
                }
                if (!result.cancelled)
                    result = LocalPrintColorBoundaryRefinement::compute_guarded(task, *topology);
                if (result.ok() && AI::model_artifact_sha256(source) != expected_source)
                    result.error = "Original model changed during color matching. Reopen the model.";
            } catch (const std::exception& error) { result.error = error.what(); }
            // These are disposable files from this one owned invocation. Original
            // assets and version records never live beneath this request root.
            wxString cleanup_notice;
            if (!owned_request.empty()) {
                std::string reason;
                if (!LocalSemanticWorker::cleanup_request(owned_request, temporary_parent, reason))
                    cleanup_notice = _L("本次识别的临时文件未能完全清理。\n");
            }
            wxGetApp().CallAfter([this, weak, result = std::move(result), evidence, notice, cleanup_notice, recognize_details, revision,
                                 expected_source, expected_geometry]() mutable {
                if (!weak || stopped || revision != operation_revision) return;
                if (worker.joinable()) worker.join(); busy = false;
                if (!result.ok() || cancel) { message((cancel || result.cancelled ? _L("已取消，准备页未修改。\n") : u8(result.error)) + cleanup_notice); refresh(); return; }
                if (input.identity.source_sha256 != expected_source || input.identity.geometry_id != expected_geometry) {
                    message(_L("模型在计算期间已改变，请重新打开。")); refresh(); return;
                }
                if (recognize_details) {
                    semantic_evidence = evidence; semantic_notice = notice;
                    input.identity.regions = result.result.regions;
                    initial = input.identity;
                }
                computed = std::move(result.result);
                if (!cleanup_notice.empty()) computed.notices.push_back("Local semantic request cleanup incomplete.");
                has_result = true; display(); refresh();
            });
        });
    }

    void display()
    {
        if (!has_result || busy) return;
        std::vector<PreviewPalette::Color> colors;
        if (!original->GetValue()) for (size_t target : computed.face_targets) colors.push_back(computed.targets[target].output);
        std::string error;
        if (!preview->display_surface_colors(colors, computed.geometry_id, error)) { message(u8(error)); return; }
        const auto quality = LocalPrintColorQuality::evaluate(input.faces, computed);
        wxString text = wxString::Format(_L("目标 n=%d；实际分组 k=%d；物理耗材 p=%d\n"), int(computed.requested_color_count),
            int(computed.targets.size()), int(computed.physical_channels.size()));
        if (restored_confirmation) text += _L("已恢复确认配色，面分组保持不变。\n");
        text += computed.mode == AI::PrintColorMode::Direct ? _L("直接匹配\n") : _L("叠色扩展：比较实体色与合法配方\n");
        if (computed.mode == AI::PrintColorMode::Layered) {
            std::set<uint32_t> outputs;
            size_t recipes = 0;
            for (const auto& target : computed.targets) if (target.executable) {
                outputs.insert(Matching::packed_rgb(target.output));
                if (target.recipe) ++recipes;
            }
            text += wxString::Format(_L("当前方案输出 %d 种颜色，叠色目标 %d 个。\n"), int(outputs.size()), int(recipes));
            if (outputs.size() < computed.targets.size())
                text += _L("部分目标共用近似输出；保留全部原目标，未增加实体耗材。\n");
            if (recipes) text += _L("叠色配方已参与预览；准备页应用尚未接通，暂不能确认。\n");
        }
        if (computed.mode == AI::PrintColorMode::Layered && !restored_confirmation) {
            const auto& process = snapshot.sublayer_process;
            text += process.sublayers_enabled ? _L("当前配置已开启子层叠色。\n") : _L("当前配置未开启子层叠色。\n");
            if (!process.layer_heights_mm.empty())
                text += wxString::Format(_L("配置层高 %.3f mm；配方仍需按着色区域逐层核验。\n"), process.layer_heights_mm.front());
            size_t known_limits = 0;
            for (const auto& material : snapshot.sublayer_materials)
                if (material.min_layer_mm > 0 && material.max_layer_mm >= material.min_layer_mm && material.line_width_mm > 0)
                    ++known_limits;
            text += wxString::Format(_L("喷嘴配置限制可用：%d/%d 种耗材（自动映射使用共同范围）。\n"),
                int(known_limits), int(snapshot.sublayer_materials.size()));
            if (process.z_resolution_mm <= 0) text += _L("待补充设备 Z 步距。\n");
            if (process.region_width_mm <= 0 || process.region_height_mm <= 0)
                text += _L("待核验着色区域最小宽度和高度。\n");
            if (process.surface_condition.empty() || process.measurement_condition.empty())
                text += _L("尚未绑定表面方向和颜色观察条件。\n");
        }
        text = layer_notice + text;
        text += _L("以下是屏幕颜色预测，未经实物校准。\n");
        if (quality.has_covered_samples) text += wxString::Format(_L("平均 ΔE00 %.2f；P95 %.2f；最大 %.2f\n"), quality.mean_delta_e, quality.p95_delta_e, quality.worst_delta_e);
        else text += _L("无可执行覆盖，色差指标暂不可用。\n");
        text += wxString::Format(_L("未覆盖面积 %.1f%%；已分配但超差面积 %.1f%%\n"), quality.unresolved_area_fraction * 100, quality.over_tolerance_area_fraction * 100);
        if (!snapshot.material_metadata_complete) text += _L("耗材温度或类型资料不完整，请先完善工程耗材配置。\n");
        text += _L("当前使用面角颜色采样。\n");
        if (restored_confirmation) text += _L("本次未重新识别细节。\n");
        else if (semantic_evidence) {
            size_t active_faces = 0;
            for (const auto& region : computed.regions)
                if (region.id.find("auto-semantic:") == 0 && !region.user_protected && !region.locked_physical_slot)
                    active_faces += region.faces.size();
            text += wxString::Format(_L("本地识别可靠标注 %d 面，未知 %d 面；当前采用 %d 面。\n"),
                int(semantic_evidence->known_faces), int(semantic_evidence->unknown_faces), int(active_faces));
            text += _L("未知区域按原色匹配；未单独识别牙齿、瞳孔或眼白。\n");
        }
        if (!restored_confirmation) text += semantic_notice;
        for (const auto& notice : computed.notices) {
            if (notice.find("Local material locks reused existing physical groups") == 0)
                text += _L("局部锁色已复用现有耗材分组；与同样锁色约束的原计算方案相比，整体和保护区域的预测色差未退步。\n");
            else if (notice.find("Final semantic quality gate retained") == 0)
                text += _L("细节偏好未通过最终质量对照，已保留基线配色。\n");
            else if (notice.find("Final semantic quality gate accepted") == 0)
                text += quality.unresolved_area_fraction <= 1e-12 && quality.has_covered_samples ?
                    _L("细节偏好已通过整体与关键区域的最终质量对照。\n") :
                    _L("细节候选已完成对照；仍有未分配区域，完整配色尚未通过。\n");
            else if (notice == "Local semantic request cleanup incomplete.")
                text += _L("本次识别的临时文件未能完全清理。\n");
            else if (notice.find("Workspace recipe evaluation: ") == 0)
                text += _L("已检查当前工艺；条件不完整，未产生可执行叠色配方。\n");
            else if (notice.find("Workspace recipe candidates: ") == 0)
                text += _L("当前工艺候选已参与目标比较；颜色为预测，仍需试样确认。\n");
            else if (notice.find("Layered bounded search found no assignment") == 0)
                text += _L("本次有界搜索未找到同时满足锁色与反差要求的方案。\n");
            else if (notice.find("Layered search uses all direct colors") == 0)
                text += _L("已比较全部目标的实体色及配方候选；这是有界搜索结果。\n");
        }
        for (size_t t = 0; t < computed.targets.size(); ++t) {
            const auto& target = computed.targets[t];
            text += wxString::Format(_L("分组 %d："), int(t + 1));
            text += target.physical_slot ? wxString::Format(_L("耗材 %d，ΔE %.2f"), int(*target.physical_slot + 1), target.delta_e00) :
                target.recipe ? wxString::Format(_L("%d 料叠色，ΔE %.2f"), int(target.recipe->components.size()), target.delta_e00) : _L("未分配");
            text += target.within_tolerance ? "\n" : _L("，需处理\n");
        }
        for (const auto& region : quality.regions)
            text += u8(region.label) + wxString::Format(_L("：ΔE %.2f，最大 %.2f\n"), region.mean_delta_e, region.worst_delta_e);
        message(text);
    }

    void apply(bool deferred)
    {
        if (busy || input.faces.empty() || plater == nullptr || (!deferred && !has_result)) return;
        if (!deferred && computed.unresolved_count() != 0 && !accept_error->GetValue()) return;
        bool applied = false;
        try {
            const auto current = adapter.printable_palette();
            if (!deferred && (current.material_fingerprint != computed.material_fingerprint || current.process_fingerprint != computed.process_fingerprint))
                throw std::runtime_error("Materials or process changed. Recompute before applying.");
            if (AI::model_artifact_sha256(artifact.local_path) != input.identity.source_sha256)
                throw std::runtime_error("Original model changed. Reopen it before applying.");
            ModelVolume* existing = nullptr; int object_index = -1;
            if (target_volume.valid()) {
                for (size_t i = 0; i < plater->model().objects.size(); ++i)
                    for (auto* volume : plater->model().objects[i]->volumes) if (volume->id() == target_volume) { existing = volume; object_index = int(i); }
                if (!existing || !LocalPrintColorApplication::same_surface_partition(native_mesh, existing->mesh().its))
                    throw std::runtime_error("Prepared model changed or was removed. Reopen the current model.");
                if (existing->source.input_file != target_source || existing->mmu_segmentation_facets.timestamp() != target_paint_stamp ||
                    static_cast<const ObjectBase&>(existing->config).timestamp() != target_config_stamp)
                    throw std::runtime_error("Prepared model colors or settings changed. Reopen it before applying this draft.");
            }
            // Defer on an existing model is navigation only, never an implicit
            // repaint or loss of its last confirmed version.
            if (deferred && existing) { if (navigate) navigate(); return; }
            auto result = computed;
            result.parent_version = input.identity.parent_version;
            if (!deferred) { result.confirmed = true; std::string reason; if (!result.valid(reason)) throw std::runtime_error(reason); }
            Model staging; auto* object = staging.add_object(); object->name = model_name;
            auto* volume = object->add_volume(TriangleMesh(native_mesh), ModelVolumeType::MODEL_PART, false);
            LocalPrintColorApplication::PreparedPainting painting; std::string error;
            LocalPrintRecipeTransition::Prepared recipe;
            if (!deferred) {
                const auto& mesh = existing ? existing->mesh() : volume->mesh();
                const bool has_recipes = std::any_of(result.targets.begin(), result.targets.end(), [](const auto& target) { return bool(target.recipe); });
                if (has_recipes) {
                    // Both adoption and existing-volume edits use one native
                    // project transaction. Current surface/process evidence is
                    // still mandatory; the UI does not invent it for imports.
                    const auto* bundle = wxGetApp().preset_bundle;
                    if (!bundle) throw std::runtime_error("The current material configuration is unavailable.");
                    PrintColorNozzleRouting routing;
                    if (const auto* plate = plater->get_partplate_list().get_curr_plate()) {
                        routing.mode = plate->get_real_filament_map_mode(bundle->project_config);
                        routing.filament_maps = plate->get_real_filament_maps(bundle->project_config);
                    }
                    if (!LocalPrintRecipeTransition::prepare(mesh, native_mesh, result, current, *bundle, routing, recipe, error))
                        throw std::runtime_error(error);
                    result = recipe.result;
                    painting = std::move(recipe.native.painting);
                } else if (!LocalPrintColorApplication::prepare_from_source(mesh, native_mesh, result, current.material_fingerprint, current.process_fingerprint, painting, error))
                    throw std::runtime_error(error);
                if (!existing && !LocalPrintRecipeApplication::apply_painting(*volume, std::move(painting), error)) throw std::runtime_error(error);
            }
            const auto directory = version_directory(); fs::create_directories(directory);
            const auto id = fs::unique_path("orca-color-%%%%%%%%-%%%%%%%%").string();
            const auto version_asset = directory / (id + ".glb");
            LocalPrintColorCommit::Prepared mutation;
            if (existing && !LocalPrintColorCommit::prepare(*existing, std::move(painting), version_asset.string(), mutation, error))
                throw std::runtime_error(error);
            auto colors = source_colors;
            if (!deferred) for (size_t f = 0; f < result.face_count; ++f)
                for (size_t c = 0; c < 3; ++c) {
                    const auto& rgb = result.targets[result.face_targets[f]].output;
                    colors[f*3+c] = {rgb[0], rgb[1], rgb[2], 1.f};
                }
            if (!AI::write_model_artifact(version_asset, source_mesh, colors, error)) throw std::runtime_error(error);
            nlohmann::json record = {{"schema", "orcaslicer.local-color-version.v1"}, {"version_id", id},
                {"parent_version", input.identity.parent_version}, {"source_path", artifact.local_path.string()},
                {"source_name", model_name},
                {"source_sha256", input.identity.source_sha256}, {"binding_geometry_id", input.identity.geometry_id},
                {"derived_sha256", AI::model_artifact_sha256(version_asset)},
                {"result", deferred ? nlohmann::json(nullptr) : LocalPrintColorState::encode(result)}};
            const auto temporary = directory / (id + ".json.tmp");
            { fs::ofstream stream(temporary); stream << record.dump(2); stream.close(); if (!stream) throw std::runtime_error("Unable to save the color version."); }
            fs::rename(temporary, directory / (id + ".json"));
            auto refresh_materials = [&] {
                if (!recipe.native.bundle) return;
                auto& bundle = *wxGetApp().preset_bundle;
                plater->get_partplate_list().set_filament_count(int(bundle.filament_presets.size()));
                plater->on_config_change(bundle.full_config());
                plater->on_filament_count_change(bundle.filament_presets.size());
                plater->get_partplate_list().invalid_all_slice_result();
                bundle.export_selections(*wxGetApp().app_config);
            };
            // All parsing, identity checks and version writes precede mutation.
            if (existing) {
                if (!plater->apply_local_print_colors(*existing, mutation, recipe.native.bundle.get(), error)) throw std::runtime_error(error);
                applied = true;
                target_source = existing->source.input_file; target_paint_stamp = existing->mmu_segmentation_facets.timestamp();
                target_config_stamp = static_cast<const ObjectBase&>(existing->config).timestamp();
                refresh_materials();
                // changed_object() also drops instances onto the bed. A color
                // transaction only refreshes paint and preserves placement.
                plater->update(); wxGetApp().obj_list()->update_info_items(size_t(object_index));
            } else {
                volume->source.input_file = version_asset.string(); object->input_file = version_asset.string();
                size_t index = 0;
                if (!plater->adopt_local_print_model(*object, recipe.native.bundle.get(), index, error))
                    throw std::runtime_error(error);
                applied = true;
                target_volume = plater->model().objects[index]->volumes.front()->id();
                const auto* imported = plater->model().objects[index]->volumes.front();
                target_source = imported->source.input_file; target_paint_stamp = imported->mmu_segmentation_facets.timestamp();
                target_config_stamp = static_cast<const ObjectBase&>(imported->config).timestamp();
                refresh_materials();
                plater->finish_local_print_model_import(index);
            }
            input.identity.parent_version = id; initial = input.identity; undo.clear(); redo.clear();
            message(deferred ? _L("原色资产已保留。可从准备页选中模型后继续匹配。") : _L("已按确认的面分组和耗材槽应用，未再次聚类。准备页支持撤销。"));
            if (navigate) navigate();
        } catch (const std::exception& error) {
            message(applied ? _L("配色已应用，但界面刷新未完成。可在准备页撤销。\n") + u8(error.what()) : u8(error.what()));
        }
    }

    LocalPrintColorPanel* owner;
    wxWindow* controls_parent {nullptr};
    Plater* plater;
    OrcaWorkspaceAdapter adapter;
    std::function<void()> navigate;
    ModelPreview3D* preview {nullptr}; wxSpinCtrl* count {nullptr}; wxChoice* slots {nullptr};
    wxCheckBox* original {nullptr}; wxCheckBox* select {nullptr}; wxCheckBox* accept_error {nullptr}; wxTextCtrl* report {nullptr};
    wxButton* apply_button {nullptr}; wxButton* defer_button {nullptr};
    std::thread worker; std::atomic<bool> cancel {false}; bool busy {false}; bool stopped {false}; bool has_result {false};
    bool restored_confirmation {false};
    uint64_t operation_revision {0};
    std::shared_ptr<const LocalSemanticEvidence::Evidence> semantic_evidence;
    wxString semantic_notice, layer_notice;
    wxButton* layer_button {nullptr}; wxButton* layer_cancel_button {nullptr}; bool layer_busy {false};
    AI::GeneratedModelArtifact artifact; ObjectID target_volume; std::string model_name;
    fs::path workbench_source_path;
    std::string target_source; ObjectBase::Timestamp target_paint_stamp {0}, target_config_stamp {0};
    AI::PrintablePaletteSnapshot snapshot;
    Matching::Input input; AI::LocalPrintColorResult initial, computed;
    indexed_triangle_set source_mesh, native_mesh; std::vector<RGBA> source_colors;
    std::vector<AI::LocalPrintColorResult> undo, redo;
};

LocalPrintColorPanel::LocalPrintColorPanel(wxWindow* parent, Plater* plater, std::function<void()> navigation,
                                         std::function<void()> back_to_workbench)
    : wxPanel(parent), m_impl(std::make_unique<Impl>(this, plater, std::move(navigation), std::move(back_to_workbench))) {}
LocalPrintColorPanel::~LocalPrintColorPanel() { shutdown(); }
bool LocalPrintColorPanel::open_artifact(const AI::GeneratedModelArtifact& artifact)
{
    if (m_impl->busy || m_impl->stopped) return false;
    // Returning to the same workbench model must retain its edits/undo history.
    if (m_impl->workbench_source_path == artifact.local_path && !m_impl->input.faces.empty() &&
        AI::model_artifact_sha256(artifact.local_path) == m_impl->input.identity.source_sha256)
        return true;
    m_impl->open(artifact);
    m_impl->workbench_source_path = artifact.local_path;
    return true;
}
void LocalPrintColorPanel::shutdown() { if (m_impl) m_impl->shutdown(); }
} // namespace Slic3r::GUI
