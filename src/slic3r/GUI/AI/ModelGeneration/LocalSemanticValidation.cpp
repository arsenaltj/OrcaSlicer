#include "LocalSemanticValidation.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "slic3r/GUI/Plater.hpp"
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <wx/choicdlg.h>
#include <wx/control.h>
#include <wx/eventfilter.h>
#include <openssl/evp.h>
#include <future>
#include <fstream>
#include <set>

namespace Slic3r::GUI {
namespace {
namespace SC = AI::SemanticColoring;
std::string file_hash(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!in || !ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return {};
    std::array<char, 65536> buffer {};
    while (in) { in.read(buffer.data(), buffer.size()); EVP_DigestUpdate(ctx.get(), buffer.data(), size_t(in.gcount())); }
    unsigned char digest[32]; unsigned length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &length) != 1) return {};
    const char* hex = "0123456789abcdef"; std::string result;
    for (unsigned i = 0; i < length; ++i) { result += hex[digest[i] >> 4]; result += hex[digest[i] & 15]; }
    return result;
}

class LocalSemanticValidation final : public wxPanel, private wxEventFilter {
public:
    LocalSemanticValidation(wxWindow* parent, Plater* plater, std::function<void()> imported)
        : wxPanel(parent), m_adapter(plater, std::move(imported)), m_timer(this)
    {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* files = new wxBoxSizer(wxHORIZONTAL);
        button(files, _L("打开本地 OBJ / GLB"), [this] {
            wxFileDialog dialog(this, _L("打开原始彩色模型"), {}, {}, "OBJ / GLB|*.obj;*.glb", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() == wxID_OK) load(std::filesystem::path(dialog.GetPath().ToStdWstring()));
        });
        const std::array<const char*, 4> fixture_files {{"left.obj", "right.obj", "couple.obj", "animal.obj"}};
        const std::array<wxString, 4> names {{_L("方飞"), _L("刘亦菲"), _L("双人"), _L("动物")}};
        for (size_t i = 0; i < fixture_files.size(); ++i) button(files, names[i], [this, file = std::string(fixture_files[i])] {
            wxString directory;
            if (!wxGetEnv("ORCA_SEMANTIC_FIXTURES", &directory)) {
                m_status->SetLabel(_L("未配置样本目录，请使用“打开本地 OBJ / GLB”。")); return;
            }
            load(std::filesystem::path(directory.ToStdWstring()) / file);
        });
        root->Add(files, 0, wxEXPAND | wxALL, FromDIP(6));
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        m_mode = new wxChoice(this, wxID_ANY);
        m_mode->Append(_L("原色")); m_mode->Append(_L("基线：仅语义")); m_mode->Append(_L("候选：MobileSAM 边界"));
        m_mode->SetSelection(2); actions->Add(m_mode, 1, wxRIGHT, FromDIP(6));
        m_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { apply_mode(); });
        button(actions, _L("重新运行"), [this] { m_preview->set_validation_provider("none"); apply_mode(); });
        m_cancel_button = button(actions, _L("取消"), [this] {
            ++m_load_generation; m_restore.clear();
            if (m_export_stage >= 0) {
                finish_export();
                m_status->SetLabel(_L("已取消六面图导出，恢复导出前的试色和视角。")); return;
            }
            auto state = m_preview->color_trial_state(); state.enabled = false; m_preview->restore_color_trial(state);
            m_status->SetLabel(_L("已取消当前结果应用，原始模型保留。"));
        });
        button(actions, _L("导出三列六面图"), [this] { start_export(); });
        button(actions, _L("保存验证会话"), [this] { save_session(); });
        button(actions, _L("打开验证会话"), [this] { open_session(); });
        root->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        auto* editing = new wxBoxSizer(wxHORIZONTAL);
        button(editing, _L("局部圈选"), [this] {
            m_preview->set_selection_enabled(true);
            m_preview->set_selection_gesture(ModelPreview3D::SelectionGesture::Lasso);
        });
        button(editing, _L("旋转"), [this] { m_preview->set_selection_enabled(false); });
        button(editing, _L("局部改色"), [this] {
            auto slots = m_preview->active_slot_palette();
            slots.erase(std::remove_if(slots.begin(),slots.end(),[](const auto& slot){return !slot.enabled;}),slots.end());
            wxArrayString labels;
            for (const auto& slot : slots) {
                wxColour color(int(std::lround(slot.color[0]*255)),int(std::lround(slot.color[1]*255)),int(std::lround(slot.color[2]*255)));
                labels.Add(wxString::FromUTF8(slot.id)+" · "+color.GetAsString(wxC2S_HTML_SYNTAX));
            }
            if (labels.empty()) { m_status->SetLabel(_L("请先启用至少一个耗材。")); return; }
            wxSingleChoiceDialog dialog(this,_L("选择局部使用的耗材槽位"),_L("局部改色"),labels);
            if (dialog.ShowModal()!=wxID_OK) return;
            auto before = m_preview->manual_color_state();
            if (m_preview->paint_selected_slot(slots[size_t(dialog.GetSelection())])) {
                m_undo.push_back(std::move(before)); m_redo.clear();
            } else m_status->SetLabel(_L("请先圈选需要修改的模型区域。"));
        });
        button(editing, _L("撤销改色"), [this] {
            if (m_undo.empty()) return;
            m_redo.push_back(m_preview->manual_color_state());
            m_preview->set_manual_state(std::move(m_undo.back())); m_undo.pop_back();
        });
        button(editing, _L("重做改色"), [this] {
            if (m_redo.empty()) return;
            m_undo.push_back(m_preview->manual_color_state());
            m_preview->set_manual_state(std::move(m_redo.back())); m_redo.pop_back();
        });
        button(editing, _L("试色导入准备页"), [this] { import_model(true); });
        button(editing, _L("原色导入准备页"), [this] { import_model(false); });
        root->Add(editing, 0, wxEXPAND | wxALL, FromDIP(6));
        m_status = new wxStaticText(this, wxID_ANY, _L("本地验证：直接打开原始模型，无需历史任务或生成服务。"));
        root->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        m_preview = new ModelPreview3D(this); root->Add(m_preview, 1, wxEXPAND);
        SetSizer(root);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); }, m_timer.GetId());
        m_timer.Start(150);
        wxEvtHandler::AddFilter(this);
    }
    ~LocalSemanticValidation() override {
        wxEvtHandler::RemoveFilter(this);
        m_timer.Stop(); if (m_loading.valid()) m_loading.wait();
    }

private:
    template<class F> wxButton* button(wxBoxSizer* row, const wxString& label, F action) {
        auto* item = new wxButton(this, wxID_ANY, label);
        item->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
        row->Add(item, 0, wxRIGHT, FromDIP(4));
        return item;
    }
    int FilterEvent(wxEvent& event) override {
        if (m_export_stage < 0) return Event_Skip;
        auto* window = dynamic_cast<wxWindow*>(event.GetEventObject());
        if (!window || (window != this && !IsDescendant(window)) ||
            window == m_cancel_button || (m_cancel_button && m_cancel_button->IsDescendant(window)))
            return Event_Skip;
        // Block user changes, including already queued commands and canvas
        // gestures. Painting, sizing, timers and inference polling still run;
        // neither the preview panel nor its GL canvas is disabled.
        if (event.IsCommandEvent() || dynamic_cast<wxMouseEvent*>(&event) || dynamic_cast<wxKeyEvent*>(&event))
            return Event_Processed;
        // A project palette must not refresh from external preset edits while
        // the three export columns are using the frozen input selection.
        if (event.GetEventType() == wxEVT_IDLE && dynamic_cast<ModelPreviewColorControls*>(window))
            return Event_Processed;
        return Event_Skip;
    }
    void freeze_export_controls() {
        m_export_controls.clear();
        const std::function<void(wxWindow*)> collect = [&](wxWindow* parent) {
            for (auto node = parent->GetChildren().GetFirst(); node; node = node->GetNext()) {
                wxWindow* child = node->GetData();
                if (child == m_cancel_button) continue;
                if (dynamic_cast<wxControl*>(child))
                    m_export_controls.emplace_back(child, child->IsThisEnabled());
                collect(child);
            }
        };
        collect(this);
        for (const auto& saved : m_export_controls) saved.first->Disable();
    }
    struct Loaded {
        ModelPreview3D::PreparedModel model;
        std::filesystem::path path;
        std::string sha256, error;
        size_t generation;
    };
    void load(const std::filesystem::path& path, nlohmann::json restoration = {}) {
        if (m_loading.valid()) { m_status->SetLabel(_L("正在读取模型，请取消后稍等读取结束。")); return; }
        if (!std::filesystem::is_regular_file(path)) { m_status->SetLabel(_L("模型文件不存在。")); return; }
        // The restoration document belongs to this accepted load only. A
        // normal model load clears any prior session, while a rejected busy
        // load cannot replace the document belonging to the in-flight model.
        m_restore = std::move(restoration);
        if (m_export_stage >= 0) finish_export();
        const size_t generation = ++m_load_generation;
        m_loading = std::async(std::launch::async, [path, generation] {
            Loaded result; result.path = path; result.generation = generation; result.sha256 = file_hash(path);
            try { ModelPreview3D::prepare_model(boost::filesystem::path(path.wstring()), result.model, result.error); }
            catch (const std::exception& e) { result.error = e.what(); }
            return result;
        });
        m_status->SetLabel(_L("正在后台读取本地模型……"));
    }
    void poll() {
        if (m_loading.valid() && m_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto result = m_loading.get();
            if (result.generation != m_load_generation) { m_restore.clear(); return; }
            if (!result.error.empty()) {
                m_restore.clear(); m_status->SetLabel(wxString::FromUTF8(result.error)); return;
            }
            const bool restoring = !m_restore.empty();
            std::string error;
            AI::ColorTrialPersistence::State restored_trial;
            ModelPreview3D::ManualColorState restored_manual;
            bool restore_manual = false;
            int restored_mode = 2;
            std::string restored_signature;
            // Validate the complete session against the prepared source before
            // replacing the currently visible model or any of its user edits.
            try { if (restoring) {
                if (m_restore.at("source_sha256").get<std::string>() != result.sha256)
                    throw std::runtime_error("Original model does not match the saved session hash.");
                const size_t faces = result.model.triangles;
                if (!AI::ColorTrialPersistence::decode(m_restore.at("trial"), faces,
                        result.model.geometry_id, restored_trial, error) || restored_trial.colors.empty())
                    throw std::runtime_error(error.empty() ? "Saved trial palette is empty." : error);
                if (m_restore.contains("mode")) {
                    if (!m_restore["mode"].is_number_integer())
                        throw std::runtime_error("Invalid saved validation mode.");
                    restored_mode = m_restore["mode"].get<int>();
                    if (restored_mode < 0 || restored_mode > 2)
                        throw std::runtime_error("Invalid saved validation mode.");
                }
                if (m_restore.contains("manual")) {
                    if (!AI::SurfaceSelectionPersistence::decode_colors(m_restore["manual"], faces,
                            result.model.geometry_id, restored_manual.colors, error))
                        throw std::runtime_error(error);
                    restore_manual = true;
                }
                if (m_restore.contains("manual_slots")) {
                    const auto& entries = m_restore["manual_slots"];
                    if (!entries.is_array() || entries.size() > restored_manual.colors.size())
                        throw std::runtime_error("Invalid saved manual slot count.");
                    std::set<size_t> colored, seen;
                    for (const auto& item : restored_manual.colors) colored.insert(item.first);
                    for (const auto& entry : entries) {
                        if (!entry.is_object() || !entry.at("face_id").is_number_integer() ||
                            !entry.at("slot_id").is_string() || !entry.at("color").is_array() ||
                            entry["color"].size() != 3)
                            throw std::runtime_error("Invalid saved manual slot record.");
                        const size_t face = entry["face_id"].get<size_t>();
                        const auto id = entry["slot_id"].get<std::string>();
                        const auto color = entry["color"].get<SC::Color>();
                        if (face >= faces || colored.count(face) == 0 || !seen.insert(face).second ||
                            id.empty() || id.size() > 256 || std::any_of(color.begin(), color.end(),
                                [](float c) { return !std::isfinite(c) || c < 0.f || c > 1.f; }))
                            throw std::runtime_error("Invalid saved manual slot identity or color.");
                        restored_manual.slots.push_back({face,id,id,color});
                    }
                }
                if (m_restore.contains("mapping")) {
                    const auto& palette = restored_trial.semantic_palette.empty() ?
                        restored_trial.colors : restored_trial.semantic_palette;
                    std::vector<SC::PaletteSlot> validation_slots;
                    for (size_t i = 0; i < palette.size(); ++i) {
                        const std::string id = restored_trial.slot_ids.empty() ?
                            (restored_trial.source == 0 ? "auto-fallback-" : "manual-slot-") + std::to_string(i + 1) :
                            restored_trial.slot_ids[i];
                        // Structural validation must also work for a saved
                        // session with every slot temporarily disabled.
                        validation_slots.push_back({id,palette[i],true});
                    }
                    SC::SlotMappingResult checked;
                    if (!SC::decode_slot_mapping(m_restore["mapping"], faces, validation_slots, checked, error))
                        throw std::runtime_error(error);
                    if (m_restore.contains("diagnostics")) {
                        if (!m_restore["diagnostics"].is_object())
                            throw std::runtime_error("Invalid saved model identity record.");
                        restored_signature = m_restore["diagnostics"].value("analysis_signature", "");
                    }
                }
                if (m_restore.contains("camera")) {
                    const auto& camera = m_restore["camera"];
                    if (!camera.is_object()) throw std::runtime_error("Invalid saved camera.");
                    for (const char* field : {"yaw","pitch","zoom","pan_x","pan_y"})
                        if (camera.contains(field) && (!camera[field].is_number() ||
                                !std::isfinite(camera[field].get<double>())))
                            throw std::runtime_error("Invalid saved camera coordinate.");
                    if (camera.contains("zoom") && camera["zoom"].get<double>() <= 0.)
                        throw std::runtime_error("Invalid saved camera zoom.");
                }
            } } catch (const std::exception& e) {
                m_restore.clear(); m_status->SetLabel(wxString::FromUTF8(e.what())); return;
            }
            size_t triangles = 0, colors = 0; Vec3d dimensions;
            if (!m_preview->load_prepared_model(std::move(result.model), {}, triangles, dimensions, colors, error)) {
                m_restore.clear(); m_status->SetLabel(wxString::FromUTF8(error)); return;
            }
            m_path = result.path; m_source_hash = result.sha256; m_undo.clear(); m_redo.clear();
            auto state = m_preview->color_trial_state();
            if (restoring) {
                static_cast<AI::ColorTrialPersistence::State&>(state) = std::move(restored_trial);
                if (restore_manual) m_preview->set_manual_state(std::move(restored_manual));
                m_mode->SetSelection(restored_mode);
                // Choosing the provider must not turn saved OFF controls ON.
                m_preview->set_validation_provider(restored_mode == 2 ? "mobilesam.cpu.v1" : "none");
            } else {
                const auto card = young_portrait_color_pack(); state.colors.clear();
                for (const auto& hex : card.colors) {
                    const wxColour color(wxString::FromUTF8(hex));
                    state.colors.push_back({color.Red()/255.f,color.Green()/255.f,color.Blue()/255.f});
                }
                state.mapping_colors = state.semantic_palette = state.semantic_mapping_palette = state.semantic_portrait_card = state.colors;
                state.source = 2; state.count = 6; state.enabled = true; state.lighting = true;
                state.slot_ids.clear(); state.slot_enabled.clear();
                state.legacy_slot_ids.clear(); state.dormant_slots.clear();
            }
            m_preview->restore_color_trial(state); m_preview->front_view();
            if (restoring && m_restore.contains("mapping"))
                m_preview->restore_semantic_snapshot(m_restore["mapping"],restored_signature);
            if (!restoring) apply_mode();
            if (restoring && m_restore.contains("camera")) {
                auto camera = m_preview->view_state(); const auto& c = m_restore["camera"];
                camera.yaw=c.value("yaw",camera.yaw); camera.pitch=c.value("pitch",camera.pitch);
                camera.zoom=c.value("zoom",camera.zoom); camera.pan_x=c.value("pan_x",0.); camera.pan_y=c.value("pan_y",0.);
                m_preview->restore_view(camera);
            }
            m_restore.clear();
            m_status->SetLabel(wxString::Format(_L("已载入 %s · %u 个原面；请选择原色、基线或候选。"),
                wxString(m_path.filename().wstring()), unsigned(triangles)));
        }
        if (m_export_stage >= 0) {
            // Programmatic progress updates may have enabled controls again.
            for (const auto& saved : m_export_controls) saved.first->Disable();
            if (!m_preview->semantic_busy()) export_stage();
        }
    }
    void apply_mode() {
        auto state = m_preview->color_trial_state();
        state.enabled = m_mode->GetSelection() != 0; state.semantic_optimization = true;
        m_preview->set_validation_provider(m_mode->GetSelection() == 2 ? "mobilesam.cpu.v1" : "none");
        m_preview->restore_color_trial(state);
    }
    void save_session() {
        if (m_path.empty() || m_preview->semantic_busy()) { m_status->SetLabel(_L("请等待当前模型配色结束后保存。")); return; }
        wxFileDialog dialog(this, _L("保存验证会话"), {}, "validation-session.json", "JSON|*.json", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto camera = m_preview->view_state();
        nlohmann::json session {{"schema","orca.local-semantic-session/v1"},{"source",m_path.u8string()},
            {"source_sha256",m_source_hash},{"trial",m_preview->color_trial_metadata()},
            {"manual",m_preview->face_color_metadata()},{"mode",m_mode->GetSelection()},
            {"diagnostics",m_preview->validation_diagnostics()},
            {"mapping",SC::encode_slot_mapping(m_preview->semantic_slots_result())},
            {"camera",{{"yaw",camera.yaw},{"pitch",camera.pitch},{"zoom",camera.zoom},{"pan_x",camera.pan_x},{"pan_y",camera.pan_y}}}};
        session["manual_slots"]=nlohmann::json::array();
        for (const auto& intent : m_preview->manual_color_state().slots)
            session["manual_slots"].push_back({{"face_id",intent.face_id},{"slot_id",intent.intended_slot_id},{"color",intent.intended_color}});
        std::ofstream out(std::filesystem::path(dialog.GetPath().ToStdWstring()),std::ios::binary); out << session.dump(2);
        m_status->SetLabel(out ? _L("验证会话已保存；打印分区请在准备页另存 3MF。") : _L("会话保存失败。"));
    }
    void open_session() {
        if (m_loading.valid()) { m_status->SetLabel(_L("正在读取模型，请取消后稍等读取结束。")); return; }
        wxFileDialog dialog(this, _L("打开验证会话"), {}, {}, "JSON|*.json", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        try {
            std::ifstream input(std::filesystem::path(dialog.GetPath().ToStdWstring())); nlohmann::json doc; input >> doc;
            if (doc.value("schema", "") != "orca.local-semantic-session/v1") throw std::runtime_error("Unsupported validation session.");
            const auto source = std::filesystem::u8path(doc.at("source").get<std::string>());
            const auto hash = doc.at("source_sha256").get<std::string>();
            if (hash.size() != 64 || std::any_of(hash.begin(),hash.end(),[](char c) {
                    return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
                })) throw std::runtime_error("Invalid saved source hash.");
            load(source,std::move(doc));
        } catch (const std::exception& e) { m_restore.clear(); m_status->SetLabel(wxString::FromUTF8(e.what())); }
    }
    void start_export() {
        if (m_export_stage >= 0) { m_status->SetLabel(_L("六面图正在导出，请完成或取消后再开始。")); return; }
        if (m_loading.valid()) { m_status->SetLabel(_L("请等待当前模型读取结束后再导出。")); return; }
        if (m_path.empty() || m_preview->semantic_busy()) { m_status->SetLabel(_L("请等待当前配色结束后导出。")); return; }
        wxDirDialog dialog(this, _L("选择六面图输出目录")); if (dialog.ShowModal()!=wxID_OK) return;
        // Keep every run, including incomplete or rejected candidates.
        try {
            const auto base=std::filesystem::path(dialog.GetPath().ToStdWstring()) / m_path.stem();
            m_output=base;
            for (size_t run=2;std::filesystem::exists(m_output);++run)
                m_output=base.parent_path()/(base.filename().wstring()+L"-"+std::to_wstring(run));
            std::filesystem::create_directories(m_output);
        } catch (const std::exception& e) { m_status->SetLabel(wxString::FromUTF8(e.what())); return; }
        m_export_saved=m_preview->color_trial_state(); m_export_camera=m_preview->view_state(); m_export_mode=m_mode->GetSelection();
        m_export_stage=0; freeze_export_controls(); m_mode->SetSelection(0); apply_mode();
    }
    void export_stage() {
        try { export_stage_images(); }
        catch (const std::exception& e) {
            finish_export(); m_status->SetLabel(wxString::FromUTF8(e.what()));
        }
    }
    void export_stage_images() {
        const std::array<const char*,3> modes {{"original","baseline","candidate"}};
        const std::array<const char*,6> views {{"front","back","left","right","top","bottom"}};
        constexpr double pi=3.14159265358979323846;
        const std::array<std::pair<double,double>,6> angles {{{-pi/2,-pi/2},{pi/2,-pi/2},{0,-pi/2},{pi,-pi/2},{0,0},{0,pi}}};
        nlohmann::json captures=nlohmann::json::array();
        std::string error;
        for (bool lighting : {true,false}) {
            auto state=m_preview->color_trial_state(); state.lighting=lighting; m_preview->restore_color_trial(state);
            for (size_t i=0;i<views.size();++i) {
                auto camera=m_preview->view_state(); camera.yaw=angles[i].first; camera.pitch=angles[i].second;
                camera.zoom=1.; camera.pan_x=camera.pan_y=0.; m_preview->restore_view(camera);
                const std::string name=std::string(modes[m_export_stage])+"-"+views[i]+(lighting?"-lit.png":"-flat.png");
                int samples=0;
                if (!m_preview->capture_validation_image(m_output/name,1024,1024,samples,error)) {
                    m_status->SetLabel(wxString::FromUTF8(error)); finish_export(); return;
                }
                captures.push_back({{"image",name},{"yaw",camera.yaw},{"pitch",camera.pitch},{"samples",samples},{"lighting",lighting}});
            }
        }
        std::ofstream record(m_output/(std::string(modes[m_export_stage])+".json"));
        record << nlohmann::json({{"source_sha256",m_source_hash},{"captures",captures},
            {"diagnostics",m_preview->validation_diagnostics()},{"trial",m_preview->color_trial_metadata()}}).dump(2);
        record.close();
        if (!record) throw std::runtime_error("Cannot save six-view export metadata.");
        if (++m_export_stage < 3) { m_mode->SetSelection(m_export_stage); apply_mode(); return; }
        for (bool lighting : {true,false}) for (const char* view : views) {
            wxImage combined(3072,1024); combined.SetRGB(wxRect(0,0,3072,1024),255,255,255);
            for (size_t column=0;column<3;++column) {
                wxImage part(wxString((m_output/(std::string(modes[column])+"-"+view+(lighting?"-lit.png":"-flat.png"))).wstring()),wxBITMAP_TYPE_PNG);
                if (!part.IsOk()) throw std::runtime_error("A required six-view capture could not be read.");
                for(int row=0;row<1024;++row) std::copy_n(part.GetData()+row*1024*3,1024*3,
                    combined.GetData()+(row*3072+int(column)*1024)*3);
            }
            if (!combined.SaveFile(wxString((m_output/(std::string("compare-")+view+(lighting?"-lit.png":"-flat.png"))).wstring()),wxBITMAP_TYPE_PNG))
                throw std::runtime_error("Cannot save the six-view comparison image.");
        }
        finish_export();
        m_status->SetLabel(_L("六面图已导出：左原色／中基线／右候选。有光照与无光照结果均已保存。"));
    }
    void finish_export() {
        if (m_export_stage < 0) return;
        m_export_stage=-1; m_mode->SetSelection(m_export_mode);
        // Restore the full user state, including independently disabled trial
        // and semantic controls. apply_mode() intentionally enables those
        // controls for captures and must not run while restoring the user.
        m_preview->set_validation_provider(m_export_mode == 2 ? "mobilesam.cpu.v1" : "none");
        m_preview->restore_color_trial(m_export_saved); m_preview->restore_view(m_export_camera);
        for (const auto& saved : m_export_controls) saved.first->Enable(saved.second);
        m_export_controls.clear();
    }
    void import_model(bool trial);
    OrcaWorkspaceAdapter m_adapter;
    ModelPreview3D* m_preview {nullptr}; wxChoice* m_mode {nullptr}; wxStaticText* m_status {nullptr}; wxTimer m_timer;
    std::future<Loaded> m_loading; size_t m_load_generation {0};
    std::filesystem::path m_path,m_output; std::string m_source_hash;
    nlohmann::json m_restore;
    std::vector<ModelPreview3D::ManualColorState> m_undo,m_redo;
    int m_export_stage {-1},m_export_mode {2};
    wxButton* m_cancel_button {nullptr};
    std::vector<std::pair<wxWindow*,bool>> m_export_controls;
    ModelPreviewColorControls::State m_export_saved;
    ModelPreview3D::ViewState m_export_camera;
};

void LocalSemanticValidation::import_model(bool trial)
{
    if (m_path.empty() || m_preview->semantic_busy()) { m_status->SetLabel(_L("请等待当前配色结束后导入。")); return; }
    trial = trial && m_preview->color_trial_mapping().enabled;
    AI::ModelImportRequest request;
    request.artifact.local_path=boost::filesystem::path(m_path.wstring());
    request.artifact.format=m_path.extension()==".glb"?"glb":"obj";
    request.artifact.color_encoding="vertex";
    request.color_mode=AI::ImportColorMode::NativeMatch;
    request.face_color_geometry_id=m_preview->geometry_id();
    if (!trial) request.face_color_overrides=m_preview->face_color_overrides();
    if (trial) {
        const auto source=m_preview->semantic_source();
        auto slots=m_preview->active_slot_palette();
        slots.erase(std::remove_if(slots.begin(),slots.end(),[](const auto& s){return !s.enabled;}),slots.end());
        if (!source || slots.empty()) { m_status->SetLabel(_L("请至少启用一个耗材。")); return; }
        const auto* configured=wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        std::set<size_t> used;
        for (const auto& slot:slots) {
            size_t matched=configured?configured->values.size():0;
            const std::string project_prefix="project-filament-";
            std::optional<size_t> bound;
            if(slot.id.rfind(project_prefix,0)==0) try {bound=std::stoull(slot.id.substr(project_prefix.size()));} catch (...) {}
            if (configured) for(size_t i=0;i<configured->values.size();++i) {
                if(bound && i!=*bound) continue;
                wxColour c(wxString::FromUTF8(configured->values[i]));
                if (used.count(i)==0 && c.IsOk() && std::abs(c.Red()/255.f-slot.color[0])<.5f/255.f &&
                    std::abs(c.Green()/255.f-slot.color[1])<.5f/255.f && std::abs(c.Blue()/255.f-slot.color[2])<.5f/255.f) {matched=i;break;}
            }
            if (!configured || matched==configured->values.size()) {
                m_status->SetLabel(_L("试色与工程耗材不一致。请先用“应用 / 保存耗材包”配置相同槽位，再导入。")); return;
            }
            used.insert(matched); request.material_slots.push_back({slot.id,slot.color,matched});
        }
        std::vector<SC::Color> labs; for(const auto& slot:slots) labs.push_back(PreviewPalette::to_lab(slot.color));
        request.face_slot_overrides.reserve(source->mesh.indices.size());
        for(size_t id=0;id<source->mesh.indices.size();++id) {
            SC::Color color {};
            if(source->face_colors.size()==source->mesh.indices.size()) for(size_t c=0;c<3;++c) color[c]=source->face_colors[id][c];
            else if(source->vertex_colors.size()==source->mesh.vertices.size())
                for(int vertex:source->mesh.indices[id]) for(size_t c=0;c<3;++c) color[c]+=source->vertex_colors[vertex][c]/3.f;
            request.face_slot_overrides.push_back({id,slots[PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(color),labs)].id});
        }
        const auto& mapped=m_preview->semantic_slots_result();
        if(m_preview->material_mapping_ready()) for(const auto& face:mapped.face_slots)
            request.face_slot_overrides[face.face_id].slot_id=face.slot_id;
        for(const auto& manual:m_preview->face_color_overrides())
            request.face_slot_overrides[manual.first].slot_id=slots[PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(manual.second),labs)].id;
        if(m_preview->material_mapping_ready()) for(const auto& manual:m_preview->effective_manual_slots())
            request.face_slot_overrides[manual.face_id].slot_id=manual.slot_id;
        std::set<size_t> manual_faces; for(const auto& item:m_preview->face_color_overrides()) manual_faces.insert(item.first);
        if(m_preview->semantic_ready()) for(const auto& leaf:mapped.subface_slots) {
            if(manual_faces.count(leaf.face_id)) continue;
            auto slot=std::find_if(slots.begin(),slots.end(),[&](const auto& s){return s.id==leaf.slot_id;});
            if(slot!=slots.end()) request.subface_color_overrides.push_back({leaf.face_id,leaf.path.depth,leaf.path.value,slot->color,slot->id});
        }
    }
    const auto result=m_adapter.import_artifact(request);
    m_status->SetLabel(result.imported()?_L("模型已导入准备页，请另存 3MF 检查恢复和切片材料。"):
        wxString::FromUTF8(result.error));
}
}

bool local_semantic_validation_requested() {
    wxString value; return wxGetEnv("ORCA_SEMANTIC_VALIDATION",&value) && value=="1";
}
wxPanel* create_local_semantic_validation(wxWindow* parent,Plater* plater,std::function<void()> imported) {
    return new LocalSemanticValidation(parent,plater,std::move(imported));
}
}
