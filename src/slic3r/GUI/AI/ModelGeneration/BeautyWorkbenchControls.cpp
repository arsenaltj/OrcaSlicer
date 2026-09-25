#include "BeautyWorkbenchControls.hpp"
#include "ModelPreview3D.hpp"
#include "LocalSemanticWorkerClient.hpp"
#include "slic3r/GUI/NativeMixedFilamentSuggestion.hpp"
#include "slic3r/GUI/AI/Model/BeautyEyeDetail.hpp"
#include "slic3r/GUI/AI/Model/BeautyGuidance.hpp"
#include "slic3r/GUI/AI/Model/BeautyRecognition.hpp"
#include <ctime>
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/colordlg.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/menu.h>
#include <wx/dialog.h>
#include <algorithm>

namespace Slic3r::GUI {
namespace {
boost::filesystem::path draft_metadata_path(const boost::filesystem::path& metadata)
{
    auto result = metadata;
    result += ".draft";
    return result;
}

nlohmann::json read_metadata(const boost::filesystem::path& model) {
    const auto root=boost::filesystem::path(data_dir())/"generated_models";
    const auto path=AI::beauty_metadata_path(model,root);
    nlohmann::json json=nlohmann::json::object();
    if(boost::filesystem::exists(path)) {
        if(boost::filesystem::file_size(path)>128*1024*1024)throw std::runtime_error("The beauty record is too large to load safely.");
        boost::filesystem::ifstream input(path);
        if(!input)throw std::runtime_error("Cannot read the model record.");
        json=nlohmann::json::parse(input);
        if(!json.is_object())throw std::runtime_error("Invalid model record; the existing file has been preserved.");
        auto adjacent=model;adjacent.replace_extension(".json");
        if(path!=adjacent && (!json.contains("model_path") || !json["model_path"].is_string() ||
            boost::filesystem::canonical(root/json["model_path"].get<std::string>())!=boost::filesystem::canonical(model)))
            throw std::runtime_error("The saved beauty record belongs to another model.");
    }
    // Draft edits are kept separately so a large accepted workbench document
    // is not serialized on every click. The wrapper keeps deletion preflight
    // and older readers compatible with the original embedded field.
    const auto sidecar=draft_metadata_path(path);
    if(boost::filesystem::is_regular_file(sidecar)) {
        if(boost::filesystem::file_size(sidecar)>128*1024*1024)throw std::runtime_error("The beauty draft is too large to load safely.");
        boost::filesystem::ifstream input(sidecar);
        if(!input)throw std::runtime_error("Cannot read the beauty draft.");
        const auto draft=nlohmann::json::parse(input);
        if(!draft.is_object() || !draft.value("beauty_puzzle_draft", nlohmann::json()).is_object())
            throw std::runtime_error("Invalid beauty draft; the existing file has been preserved.");
        json["beauty_puzzle_draft"]=draft.at("beauty_puzzle_draft");
    }
    return json;
}
void write_metadata(const boost::filesystem::path& model,const nlohmann::json& metadata) {
    const auto path=AI::beauty_metadata_path(model,boost::filesystem::path(data_dir())/"generated_models");
    const auto temporary=path.parent_path()/boost::filesystem::unique_path("beauty-record-%%%%-%%%%.tmp");
    try {
        boost::filesystem::ofstream output(temporary,std::ios::binary);output<<metadata.dump();output.close();
        if(!output)throw std::runtime_error("Cannot save the puzzle draft. Check disk space and permissions.");
        boost::filesystem::rename(temporary,path);
    } catch(...) {boost::system::error_code ignored;boost::filesystem::remove(temporary,ignored);throw;}
}

void write_draft_metadata(const boost::filesystem::path& model, const nlohmann::json& value,
                          const std::atomic<bool>* canceled = nullptr,
                          const std::atomic<uint64_t>* generation = nullptr,
                          uint64_t expected_generation = 0)
{
    const auto metadata=AI::beauty_metadata_path(model,boost::filesystem::path(data_dir())/"generated_models");
    const auto path=draft_metadata_path(metadata);
    const auto temporary=path.parent_path()/boost::filesystem::unique_path("beauty-draft-%%%%-%%%%.tmp");
    try {
        const auto current = [&] {
            return (!canceled || !canceled->load()) &&
                (!generation || generation->load() == expected_generation);
        };
        if (!current()) throw std::runtime_error("Beauty draft write cancelled.");
        boost::filesystem::ofstream output(temporary,std::ios::binary);
        const auto bytes=nlohmann::json{{"beauty_puzzle_draft", value}}.dump();
        constexpr size_t chunk=64*1024;
        for(size_t offset=0;offset<bytes.size();offset+=chunk) {
            if(!current())throw std::runtime_error("Beauty draft write cancelled.");
            output.write(bytes.data()+offset,std::streamsize(std::min(chunk,bytes.size()-offset)));
        }
        output.close();
        if(!output)throw std::runtime_error("Cannot save the puzzle draft. Check disk space and permissions.");
        // A reset/model switch can invalidate this request while the file is
        // being serialized. Never publish an obsolete snapshot after that
        // point; the next queued request owns the final state.
        if(!current())throw std::runtime_error("Beauty draft write cancelled.");
        boost::filesystem::rename(temporary,path);
    } catch(...) {boost::system::error_code ignored;boost::filesystem::remove(temporary,ignored);throw;}
}

void remove_draft_metadata(const boost::filesystem::path& model)
{
    const auto metadata=AI::beauty_metadata_path(model,boost::filesystem::path(data_dir())/"generated_models");
    boost::system::error_code error;
    boost::filesystem::remove(draft_metadata_path(metadata),error);
    if (error) throw std::runtime_error("Cannot remove the beauty draft: " + error.message());
}
}
BeautyWorkbenchControls::BeautyWorkbenchControls(wxWindow* parent,ModelPreview3D* model,AI::IPrintablePaletteProvider& palette,std::function<void()> layout_changed)
    :wxPanel(parent),preview(model),palette_provider(palette),changed(std::move(layout_changed)),timer(this)
{
    auto* root=new wxBoxSizer(wxVERTICAL);
    workflow_status=new wxStaticText(this,wxID_ANY,_L("流程：检查网格 → 原色美颜 → 导入时确认耗材配色"));
    workflow_status->Wrap(FromDIP(250));root->Add(workflow_status,0,wxEXPAND|wxBOTTOM,FromDIP(6));
    topology_status=new wxStaticText(this,wxID_ANY,_L("网格预检：正在读取模型……"));
    topology_status->Wrap(FromDIP(250));root->Add(topology_status,0,wxEXPAND|wxBOTTOM,FromDIP(8));
    status=new wxStaticText(this,wxID_ANY,_L("正在准备拼图……"));
    status->SetMinSize(wxSize(1,FromDIP(55)));root->Add(status,0,wxEXPAND|wxBOTTOM,FromDIP(8));
    original_view=new wxCheckBox(this,wxID_ANY,_L("对照原始全彩（仅查看）"));root->Add(original_view,0,wxBOTTOM,FromDIP(10));
    original_view->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){render(true);});
    match_button=new wxButton(this,wxID_ANY,_L("匹配整模型耗材颜色（可撤销）"));
    match_button->SetToolTip(_L("将原始全彩近似为当前耗材。先对照预览，检查肤色、衣服和底座，再保存导入。"));
    match_button->Disable();root->Add(match_button,0,wxEXPAND|wxBOTTOM,FromDIP(6));
    match_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){match_colors();});
    color_button=new wxButton(this,wxID_ANY,_L("更改这块颜色"));root->Add(color_button,0,wxEXPAND|wxBOTTOM,FromDIP(6));
    color_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){change_color();});
    focus_button=new wxButton(this,wxID_ANY,_L("放大编辑这块"));root->Add(focus_button,0,wxEXPAND|wxBOTTOM,FromDIP(6));
    focus_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){
        if(selected==none || task || preview->selection_busy())return;
        preview->select_beauty_faces(selected_faces());preview->focus_selection();
    });
    restore_button=new wxButton(this,wxID_ANY,_L("这块恢复原纹理"));root->Add(restore_button,0,wxEXPAND|wxBOTTOM,FromDIP(12));
    restore_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){restore_selected_color();});
    mode=new wxChoice(this,wxID_ANY);
    for(const auto& name:{_L("点选拼图"),_L("扩入：把旁边涂进这块"),_L("擦出：把边缘还给旁边"),_L("合并：点击相邻拼图"),_L("拆分：圈出新的一块")})mode->Append(name);
    mode->SetSelection(0);root->Add(mode,0,wxEXPAND|wxBOTTOM,FromDIP(6));
    mode->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){update_gesture();render(false);});
    radius=new wxSlider(this,wxID_ANY,18,4,60);radius->SetToolTip(_L("笔刷大小"));root->Add(radius,0,wxEXPAND|wxBOTTOM,FromDIP(8));
    radius->Bind(wxEVT_SLIDER,[this](wxCommandEvent&){update_gesture();});
    borders=new wxCheckBox(this,wxID_ANY,_L("显示拼图边线"));borders->SetValue(true);root->Add(borders,0,wxBOTTOM,FromDIP(12));
    borders->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){render(false);});
    auto* history=new wxBoxSizer(wxHORIZONTAL);
    undo_button=new wxButton(this,wxID_ANY,_L("撤销"));redo_button=new wxButton(this,wxID_ANY,_L("重做"));
    history->Add(undo_button,1,wxRIGHT,FromDIP(6));history->Add(redo_button,1);root->Add(history,0,wxEXPAND|wxBOTTOM,FromDIP(6));
    undo_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){restore(false);});redo_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){restore(true);});
    reset_button=new wxButton(this,wxID_ANY,_L("放弃未保存修改"));root->Add(reset_button,0,wxEXPAND);
    reset_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){
        if(!editable || !dirty)return;
        try {invalidate_draft_queue();remove_draft_metadata(source);auto metadata=read_metadata(source);metadata.erase("beauty_puzzle_draft");write_metadata(source,metadata);
            identity.clear();dirty=false;synchronize(source,true,true);
        }catch(const std::exception& e){message(wxString::FromUTF8(e.what()));}
    });
    more_button=new wxButton(this,wxID_ANY,_L("更多操作…"));root->Add(more_button,0,wxEXPAND|wxTOP,FromDIP(8));
    more_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){more_actions();});
    mode->Hide();restore_button->Hide();borders->Hide();reset_button->Hide();radius->Hide();
    SetSizer(root);
    preview->m_puzzle_focus=[this]{return original_view->GetValue()?std::vector<size_t>{}:selected_faces();};
    preview->m_puzzle_aperture=[this] {
        const auto faces=selected_faces();
        if(faces.empty())return std::vector<size_t>{};
        for(const auto& eye:eye_shapes) {
            size_t overlap=0,inside=0;
            for(size_t f:faces) {
                overlap+=std::binary_search(eye.iris_faces.begin(),eye.iris_faces.end(),f);
                inside+=std::binary_search(eye.aperture_faces.begin(),eye.aperture_faces.end(),f);
            }
            if(overlap*2>=std::min(faces.size(),eye.iris_faces.size()) && inside>=faces.size()*.8 &&
                faces.size()<=eye.aperture_faces.size())return eye.aperture_faces;
        }
        return std::vector<size_t>{};
    };
    preview->m_puzzle_stroke_start=[this](int action){stroke_selected=selected;stroke_mode=action?action:mode->GetSelection();};
    preview->m_puzzle_pick=[this](size_t f,bool merge){pick(f,merge);};
    preview->m_puzzle_is_selected=[this](size_t f){return selected!=none && f<puzzle.face_piece.size() &&
        (edit_regions?edit_regions->face_region[f]:puzzle.face_piece[f])==selected;};
    preview->m_puzzle_change_color=[this]{change_color();};
    preview->m_puzzle_stroke=[this](const auto& f){stroke(f);};
    preview->m_puzzle_reshape=[this](const auto& added,const auto& removed,bool preserve_shape) {
        if(!editable || !surface || task || stroke_selected==none || original_view->GetValue())return;
        if(edit_regions) {
            try {
                auto next=puzzle;auto regions=*edit_regions;
                if(regions.reshape_and_paint(next,*surface,base_hash,stroke_selected,added,removed))
                    commit_layers(std::move(next),std::move(regions),stroke_selected);
            }catch(const std::exception& e){message(_L("边界未修改：")+wxString::FromUTF8(e.what()));}
            return;
        }
        try {auto next=puzzle;next.reshape_region(stroke_selected,added,removed,*surface,semantic_labels,preserve_shape);commit(std::move(next),stroke_selected);}
        catch(const std::exception& e){message(_L("边界未修改：")+wxString::FromUTF8(e.what()));}
    };
    preview->m_puzzle_undo=[this](bool forward){restore(forward);};
    Bind(wxEVT_TIMER,[this](wxTimerEvent&){tick();});timer.Start(200);
    draft_worker=std::thread([this] {
        for (;;) {
            DraftRequest request;
            {
                std::unique_lock<std::mutex> lock(draft_mutex);
                draft_cv.wait(lock,[this] {
                    return draft_cancel.load() || !draft_cleanup_pending.empty() || draft_pending.has_value();
                });
                if (!draft_cleanup_pending.empty()) {
                    request=std::move(draft_cleanup_pending.front());
                    draft_cleanup_pending.pop_front();
                } else {
                    if (draft_cancel.load()) return;
                    request=std::move(*draft_pending);
                    draft_pending.reset();
                }
            }
            if (!request.durable_cleanup &&
                (draft_cancel.load() || request.generation!=draft_generation.load())) continue;
            try {
                const auto current = [this, &request] {
                    return request.durable_cleanup ||
                        (!draft_cancel.load() && request.generation == draft_generation.load());
                };
                if (!current()) continue;
                if (request.remove) {
                    remove_draft_metadata(request.model);
                    if (request.clear_legacy && current()) {
                        auto metadata=read_metadata(request.model);
                        if (!current()) continue;
                        if (metadata.erase("beauty_puzzle_draft")) write_metadata(request.model,metadata);
                    }
                } else {
                    write_draft_metadata(request.model,request.record,&draft_cancel,
                                         &draft_generation,request.generation);
                }
            } catch (const std::exception& error) {
                if (request.durable_cleanup ||
                    (!draft_cancel.load() && request.generation == draft_generation.load()))
                    BOOST_LOG_TRIVIAL(warning)<<"Cannot persist beauty draft asynchronously: "<<error.what();
            }
        }
    });
}
std::vector<size_t> BeautyWorkbenchControls::selected_faces() const {
    if(selected==none)return {};
    return edit_regions?edit_regions->faces(selected):puzzle.faces(selected);
}
std::array<float,4> BeautyWorkbenchControls::selected_color() const {
    return edit_regions?edit_regions->representative_color(puzzle,*surface,selected):
        puzzle.representative_color(selected,*surface);
}
void BeautyWorkbenchControls::restore_selected_color() {
    if(selected==none || !surface)return;
    try {
        auto next=puzzle;
        if(edit_regions) {
            const auto editor=preview->beauty_editor();if(!editor)return;
            const auto original=AI::beauty_source_face_colors(editor->mesh(),base_colors);
            if(next.palette.empty()) {
                const auto restored=next.assign_region(selected_faces(),*surface);
                next.clear_color(restored);
            } else {
                const auto target=edit_regions->source_region_color(original,*surface,selected);
                next.paint_faces_filament(selected_faces(),*surface,next.nearest_filament(target));
            }
        } else next.restore_source_color(selected,*surface);
        commit(std::move(next),selected);
    }catch(const std::exception& e){message(wxString::FromUTF8(e.what()));}
}
void BeautyWorkbenchControls::change_color() {
    if(!editable || !surface || task || selected==none || preview->selection_busy() || original_view->GetValue())return;
    const auto c=selected_color();wxColourData data;
    data.SetColour(wxColour(int(c[0]*255),int(c[1]*255),int(c[2]*255)));
    auto palette=palette_provider.printable_palette();
    wxDialog dialog(this,wxID_ANY,_L("区域颜色"));
    auto* root=new wxBoxSizer(wxVERTICAL);
    root->Add(new wxStaticText(&dialog,wxID_ANY,_L("准备页当前耗材")),0,wxALL,FromDIP(14));
    auto* swatches=new wxGridSizer(3,FromDIP(8),FromDIP(8));
    wxColour color=data.GetColour();size_t custom_index=0,chosen_slot=SIZE_MAX;
    for(const auto& channel:palette.physical_channels) {
        const wxColour value(wxString::FromUTF8(channel.display_color));if(!value.IsOk())continue;
        if(custom_index<16)data.SetCustomColour(int(custom_index++),value);
        auto* button=new wxButton(&dialog,wxID_ANY,wxString::Format(_L("耗材 %llu"),static_cast<unsigned long long>(channel.slot+1)),wxDefaultPosition,FromDIP(wxSize(96,44)));
        button->SetBackgroundColour(value);
        button->SetForegroundColour(value.Red()*.3+value.Green()*.6+value.Blue()*.1<140?*wxWHITE:*wxBLACK);
        button->SetToolTip(wxString::FromUTF8(channel.display_color+"  "+channel.material_type));
        button->Enable(channel.compatible);
        button->Bind(wxEVT_BUTTON,[&dialog,&color,&chosen_slot,value,slot=channel.slot](wxCommandEvent&){color=value;chosen_slot=slot;dialog.EndModal(wxID_OK);});
        swatches->Add(button,0,wxEXPAND);
    }
    if(custom_index)root->Add(swatches,0,wxLEFT|wxRIGHT|wxBOTTOM|wxEXPAND,FromDIP(14));
    else {delete swatches;root->Add(new wxStaticText(&dialog,wxID_ANY,_L("准备页暂无可用耗材颜色")),0,wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(14));}
    if(!palette.mixed_recipes.empty()) {
        root->Add(new wxStaticText(&dialog,wxID_ANY,_L("准备页叠色耗材")),0,wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(14));
        auto* mixes=new wxGridSizer(3,FromDIP(8),FromDIP(8));
        for(const auto& recipe:palette.mixed_recipes) {
            if(!recipe.existing_virtual_slot)continue;
            const wxColour value(wxString::FromUTF8(recipe.target_color));if(!value.IsOk())continue;
            auto* button=new wxButton(&dialog,wxID_ANY,wxString::Format(_L("叠色 %llu"),static_cast<unsigned long long>(*recipe.existing_virtual_slot+1)));
            button->SetBackgroundColour(value);button->SetForegroundColour(value.Red()*.3+value.Green()*.6+value.Blue()*.1<140?*wxWHITE:*wxBLACK);
            button->Bind(wxEVT_BUTTON,[&dialog,&chosen_slot,&color,value,slot=*recipe.existing_virtual_slot](wxCommandEvent&){color=value;chosen_slot=slot;dialog.EndModal(wxID_OK);});mixes->Add(button,0,wxEXPAND);
        }
        root->Add(mixes,0,wxLEFT|wxRIGHT|wxBOTTOM|wxEXPAND,FromDIP(14));
    }
    auto* add_mix=new wxButton(&dialog,wxID_ANY,_L("添加叠色耗材…"));
    add_mix->SetToolTip(_L("按这块原始纹理的颜色推荐配比，确认后应用。"));
    add_mix->Enable(bool(add_native_mixed_filament) && palette.supports(AI::ColorOutputMode::ProcessMix));
    add_mix->Bind(wxEVT_BUTTON,[&dialog](wxCommandEvent&){dialog.EndModal(wxID_MORE);});
    root->Add(add_mix,0,wxLEFT|wxRIGHT|wxBOTTOM|wxEXPAND,FromDIP(14));
    auto* actions=new wxBoxSizer(wxHORIZONTAL);
    auto* custom=new wxButton(&dialog,wxID_ANY,puzzle.palette.empty()?_L("自选颜色…"):_L("自选颜色并匹配耗材…"));
    custom->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){wxColourDialog picker(&dialog,&data);if(picker.ShowModal()==wxID_OK){color=picker.GetColourData().GetColour();dialog.EndModal(wxID_OK);}});
    actions->Add(custom,1,wxRIGHT,FromDIP(8));actions->Add(new wxButton(&dialog,wxID_CANCEL,_L("取消")),1);
    root->Add(actions,0,wxALL|wxEXPAND,FromDIP(14));dialog.SetSizerAndFit(root);dialog.CentreOnParent();
    const int action=dialog.ShowModal();
    if(action==wxID_MORE) {
        const auto editor=preview->beauty_editor();if(!editor)return;
        const auto original=AI::beauty_source_face_colors(editor->mesh(),base_colors);
        std::string target_hex;
        if(!original.empty()) {
            const auto target=edit_regions?edit_regions->source_region_color(original,*surface,selected):
                puzzle.source_region_color(selected,*surface,original);
            target_hex=wxString::Format("#%02X%02X%02X",int(std::clamp(target[0],0.f,1.f)*255+.5f),
                int(std::clamp(target[1],0.f,1.f)*255+.5f),int(std::clamp(target[2],0.f,1.f)*255+.5f)).ToStdString();
            std::vector<std::string> colors,types;
            for(const auto& channel:palette.physical_channels){colors.push_back(channel.display_color);types.push_back(channel.material_type);}
            const auto current=selected_color();
            const auto current_hex=wxString::Format("#%02X%02X%02X",int(std::clamp(current[0],0.f,1.f)*255+.5f),
                int(std::clamp(current[1],0.f,1.f)*255+.5f),int(std::clamp(current[2],0.f,1.f)*255+.5f)).ToStdString();
            if(!suggest_native_mixed_filament(target_hex,colors,{},types,current_hex).valid) {
                target_hex.clear();message(_L("没有更接近原色的推荐，仍可手动调整叠色。"));
            }
        }
        add_native_mixed_filament(target_hex);
        const auto updated=palette_provider.printable_palette();
        const auto added=std::find_if(updated.mixed_recipes.begin(),updated.mixed_recipes.end(),[&](const auto& r){
            return std::none_of(palette.mixed_recipes.begin(),palette.mixed_recipes.end(),[&](const auto& old){return old.existing_virtual_slot==r.existing_virtual_slot;});
        });
        if(added==updated.mixed_recipes.end())return;
        chosen_slot=*added->existing_virtual_slot;palette=updated;
    } else if(action!=wxID_OK)return;
    auto next=puzzle;
    if(!next.palette.empty())next.match_filaments(*surface,palette.physical_channels,palette.mixed_recipes);
    const auto mixed=std::find_if(palette.mixed_recipes.begin(),palette.mixed_recipes.end(),[&](const auto& r){return r.existing_virtual_slot==chosen_slot;});
    if(edit_regions) {
        if(next.palette.empty()) {
            next.paint_faces_color(selected_faces(),*surface,{color.Red()/255.f,color.Green()/255.f,color.Blue()/255.f,1});
            commit(std::move(next),selected);return;
        }
        const auto slot=chosen_slot==SIZE_MAX?next.nearest_filament({color.Red()/255.f,color.Green()/255.f,color.Blue()/255.f,1}):chosen_slot;
        if(mixed!=palette.mixed_recipes.end() && std::none_of(next.mixed_recipes.begin(),next.mixed_recipes.end(),
            [&](const auto& recipe){return recipe.existing_virtual_slot==mixed->existing_virtual_slot;}))
            next.mixed_recipes.push_back(*mixed);
        next.paint_faces_filament(selected_faces(),*surface,slot);
    }
    else if(next.palette.empty())next.paint(selected,{color.Red()/255.f,color.Green()/255.f,color.Blue()/255.f,1});
    else if(mixed!=palette.mixed_recipes.end())next.paint_mixed(selected,*mixed);
    else if(chosen_slot!=SIZE_MAX && !next.palette.empty())next.paint_filament(selected,chosen_slot);
    else next.paint(selected,{color.Red()/255.f,color.Green()/255.f,color.Blue()/255.f,1});
    commit(std::move(next),selected);
}
void BeautyWorkbenchControls::match_colors() {
    if(!editable || !surface || task || !puzzle.palette.empty() || preview->selection_busy() || original_view->GetValue())return;
    const auto palette=palette_provider.printable_palette();
    if(!AI::is_valid_physical_channel_set(palette.physical_channels) ||
       std::none_of(palette.physical_channels.begin(),palette.physical_channels.end(),[](const auto& c){return c.compatible;})) {
        message(_L("准备页没有可用耗材。请先配置耗材，再返回工作台匹配。"));return;
    }
    try {
        const auto editor=preview->beauty_editor();if(!editor)return;
        const auto original=AI::beauty_source_face_colors(editor->mesh(),base_colors);
        auto next=puzzle;
        const AI::BeautyGuidance guidance{semantic_labels,eye_details,eye_shapes,semantic_names};
        AI::beauty_match_feature_filaments(next,*surface,guidance,original,palette.physical_channels,palette.mixed_recipes);
        commit(std::move(next),selected);
    }catch(const std::exception& e){message(_L("耗材匹配未完成：")+wxString::FromUTF8(e.what()));}
}
void BeautyWorkbenchControls::more_actions() {
    if(!editable || !surface || preview->selection_busy() || task || original_view->GetValue())return;
    if(edit_regions) {
        wxMenu menu;
        menu.Append(3,_L("这块重新自动配色"));menu.Enable(3,selected!=none);
        menu.Append(6,_L("放弃未保存修改"));menu.Enable(6,dirty);
        const int action=GetPopupMenuSelectionFromUser(menu);
        if(action==3)restore_selected_color();
        else if(action==6){wxCommandEvent event(wxEVT_BUTTON);reset_button->GetEventHandler()->ProcessEvent(event);}
        return;
    }
    wxMenu menu;
    menu.Append(1,_L("框出一个区域（Ctrl 拖动）"));
    menu.Append(2,_L("合并相邻区域（Shift 点击）"));menu.Enable(2,selected!=none);
    menu.Append(3,puzzle.palette.empty()?_L("恢复这块的原纹理"):_L("这块重新自动配色"));menu.Enable(3,selected!=none && puzzle.colors.count(selected));
    menu.AppendSeparator();menu.Append(4,_L("重新自动分区（可撤销）"));
    menu.Append(8,_L("平整现有边界（可撤销）"));
    menu.Append(10,_L("更新五官识别（保留当前区域）"));
    if(!semantic_names.empty())menu.Append(11,_L("补出五官区域并配色（可撤销）"));
    if(!eye_details.empty())menu.Append(9,_L("按识别结果修整眼珠（可撤销）"));
    menu.AppendCheckItem(5,_L("显示所有区域边线"));menu.Check(5,borders->GetValue());
    menu.Append(6,_L("放弃未保存修改"));menu.Enable(6,dirty);
    menu.AppendSeparator();menu.Append(7,_L("回到点选和拖边界"));
    const int action=GetPopupMenuSelectionFromUser(menu);
    if(action==1 || action==2 || action==7) {
        mode->SetSelection(action==1?4:action==2?3:0);update_gesture();render(false);
    } else if(action==3) restore_selected_color();
    else if(action==4) {regroup_requested=true;message(_L("正在重新整理大区域和五官，完成后可撤销……"));tick();}
    else if(action==5) {borders->SetValue(!borders->GetValue());render(false);}
    else if(action==6) {wxCommandEvent event(wxEVT_BUTTON);reset_button->GetEventHandler()->ProcessEvent(event);}
    else if(action==8) {try {auto next=puzzle;next.smooth_boundaries(*surface);commit(std::move(next),selected);}catch(const std::exception& e){message(wxString::FromUTF8(e.what()));}}
    else if(action==10) {guidance_requested=true;message(_L("正在更新五官辅助，当前区域和配色保持不变……"));tick();}
    else if(action==11) {
        try {
            const AI::BeautyGuidance guidance{semantic_labels,eye_details,eye_shapes,semantic_names};
            auto next=AI::beauty_supplement_features(puzzle,*surface,guidance);
            const auto editor=preview->beauty_editor();if(!editor)return;
            const auto palette=palette_provider.printable_palette();
            next.match_filaments(*surface,palette.physical_channels,palette.mixed_recipes,
                AI::beauty_source_face_colors(editor->mesh(),base_colors));
            commit(std::move(next),none);
        }catch(const std::exception& e){message(wxString::FromUTF8(e.what()));}
    }
    else if(action==9) {
        try {
            auto next=puzzle;uint32_t focus=none;
            for(const auto& detail:eye_details)focus=next.refine_detail_region(detail,*surface);
            commit(std::move(next),focus);
        }catch(const std::exception& e){message(wxString::FromUTF8(e.what()));}
    }
}
BeautyWorkbenchControls::~BeautyWorkbenchControls() {
    timer.Stop();
    if(task)task->canceled=true;
    if(worker.joinable())worker.join();
    draft_cancel=true;
    { std::lock_guard<std::mutex> lock(draft_mutex); draft_pending.reset(); }
    draft_cv.notify_all();
    if(draft_worker.joinable())draft_worker.join();
}
void BeautyWorkbenchControls::message(const wxString& text) {
    status->SetLabel(text);status->Wrap(FromDIP(250));Layout();changed();
}
void BeautyWorkbenchControls::synchronize(const boost::filesystem::path& path,bool enabled,bool visible) {
    editable=enabled;
    if(enabled && (path!=source || identity!=preview->geometry_id())) {
        if(surface){cached_surface=surface;cached_base_colors=std::move(base_colors);cached_base_hash=base_hash;}
        invalidate_draft_queue();
        source=path;identity=preview->geometry_id();failed=false;surface.reset();document={};puzzle={};edit_regions.reset();
        base_colors.clear();semantic_labels.clear();semantic_names.clear();eye_details.clear();eye_shapes.clear();guidance_ready=false;guidance_requested=false;original_view->SetValue(false);undo.clear();redo.clear();selected=none;dirty=false;saved_puzzle={};saved_edit_regions.reset();regroup_requested=false;
        if(task)task->canceled=true;
        preview->set_beauty_surface({});topology_status->SetLabel(_L("网格预检：正在读取模型……"));
        message(_L("正在整理大区域和识别五官，首次需要稍等。\n可先旋转查看，识别结果会保存在本地。"));
    }
    Enable(enabled && bool(surface) && !task);preview->set_puzzle_enabled(visible,enabled && bool(surface) && !task);update_gesture();
    if(enabled && visible && surface && !task) {
        const auto palette=palette_provider.printable_palette();
        const bool changed_mix=std::any_of(puzzle.mixed_recipes.begin(),puzzle.mixed_recipes.end(),[&](const auto& r){
            return std::none_of(palette.mixed_recipes.begin(),palette.mixed_recipes.end(),[&](const auto& current){return AI::same_native_mixed_recipe(r,current);});
        });
        if(!puzzle.palette.empty() && AI::is_valid_physical_channel_set(palette.physical_channels) &&
           (!puzzle.same_palette(palette.physical_channels) || changed_mix))commit(puzzle,selected);
    }
}
void BeautyWorkbenchControls::update_gesture() {
    if(!editable || !surface)return;
    const int action=mode->GetSelection();
    preview->set_selection_gesture(action==1 || action==2?ModelPreview3D::SelectionGesture::Brush:
        action==4?ModelPreview3D::SelectionGesture::Lasso:ModelPreview3D::SelectionGesture::Similar,radius->GetValue());
    preview->set_selection_overlay_visible(false);radius->Hide();
    Layout();
}
void BeautyWorkbenchControls::tick() {
    if(task && !task->done && !task->canceled && shown_stage!=task->stage.load()) {
        shown_stage=task->stage.load();
        message(shown_stage==0?_L("正在准备模型表面，可先旋转查看……"):
            shown_stage==1?_L("正在识别五官，首次可能需要几分钟。\n可先旋转查看，结果会保存在本地。"):
            _L("正在整理五官区域，保留原始纹理……"));
    }
    // Reloading an identical asset clears its GL resources without changing
    // the document identity. Restore the display once its new editor is ready.
    if(editable && surface && !task && identity==preview->geometry_id() &&
       !preview->puzzle_display_ready() && preview->beauty_editor())render(true);
    if(task && task->done) {
        if(worker.joinable())worker.join();
        auto completed=std::move(task);
        if(!completed->canceled) {
            // Persist successful and empty recognition independently of edits.
            // Failure is recorded separately and must never masquerade as saved guidance.
            if(!completed->recognition_attempt.is_null())try {
                auto metadata=read_metadata(source);
                if(completed->guidance_ready)metadata["beauty_guidance"]=AI::BeautyGuidance{completed->semantic_labels,completed->eye_details,completed->eye_shapes,completed->semantic_names}.encode(identity,completed->base_hash);
                if(!completed->recognition_attempt.is_null())metadata["beauty_recognition"]=completed->recognition_attempt;
                write_metadata(source,metadata);
            }catch(const std::exception& e){BOOST_LOG_TRIVIAL(warning)<<"Cannot persist recognition result: "<<e.what();}
            if(!completed->error.empty()) {failed=!surface;preview->set_puzzle_enabled(editable,bool(surface));Enable(editable && bool(surface));message(_L("分区未完成，已保留原设置：")+wxString::FromUTF8(completed->error));}
            else if(completed->regroup || completed->guidance_only) {
                if(completed->guidance_ready) {
                    semantic_labels=std::move(completed->semantic_labels);
                    semantic_names=std::move(completed->semantic_names);
                    eye_details=std::move(completed->eye_details);eye_shapes=std::move(completed->eye_shapes);
                    guidance_ready=true;
                }
                preparation_notice=completed->notice;Enable(editable);
                if(completed->regroup)commit(std::move(completed->puzzle),none);
                else {
                    render(false);
                }
            }
            else {
                surface=completed->surface;document=std::move(completed->document);puzzle=std::move(completed->puzzle);
                edit_regions=std::move(completed->edit_regions);
                saved_puzzle=std::move(completed->saved_puzzle);
                saved_edit_regions=std::move(completed->saved_edit_regions);preparation_notice=completed->notice;
                base_colors=std::move(completed->base_colors);base_file=completed->base_file;base_hash=completed->base_hash;
                semantic_labels=std::move(completed->semantic_labels);
                semantic_names=std::move(completed->semantic_names);
                eye_details=std::move(completed->eye_details);eye_shapes=std::move(completed->eye_shapes);
                guidance_ready=completed->guidance_ready;
                dirty=!layers_equal(puzzle,edit_regions,saved_puzzle,saved_edit_regions);
                if(completed->upgraded)undo.push_back({std::move(completed->before_upgrade),edit_regions,none});
                if(dirty)try {save_draft(puzzle,edit_regions);}catch(const std::exception& e) {BOOST_LOG_TRIVIAL(warning)<<"Cannot persist automatic palette draft: "<<e.what();}
                preview->set_beauty_surface(surface);preview->set_puzzle_enabled(editable);Enable(editable);update_gesture();render(true);
            }
        }
    }
    if(!editable || failed || (surface && !regroup_requested && !guidance_requested) || task || source.empty() || identity!=preview->geometry_id())return;
    const auto editor=preview->beauty_editor();if(!editor)return;
    task=std::make_shared<Preparation>();shown_stage=-1;preview->set_puzzle_enabled(true,false);const auto work=task;const auto path=source;const auto id=identity;
    work->regroup=regroup_requested;work->guidance_only=guidance_requested;regroup_requested=false;guidance_requested=false;Enable(false);
    const auto current_puzzle=puzzle;
    const auto palette=palette_provider.printable_palette();
    const auto reuse_surface=surface?surface:cached_surface;
    const auto reuse_colors=surface?base_colors:cached_base_colors;
    const auto reuse_hash=surface?base_hash:cached_base_hash;
    const auto config_path=boost::filesystem::path(data_dir())/"local_semantic_runtime.json";
    const auto modules=boost::filesystem::path(resources_dir())/"tools"/"ai";
    const auto installed_runtime=boost::filesystem::path(resources_dir())/"beauty-runtime";
    const auto requests=boost::filesystem::path(data_dir())/"beauty_semantic_requests";
    const auto cache=boost::filesystem::path(data_dir())/"beauty_semantic_cache";
    try {
        worker=std::thread([work,editor,path,id,current_puzzle,palette,reuse_surface,reuse_colors,reuse_hash,config_path,modules,installed_runtime,requests,cache]{
            try {
                auto metadata=read_metadata(path);auto record=metadata.value("beauty_workbench",nlohmann::json::object());
                const auto saved_record=record;
                if(metadata.contains("beauty_puzzle_draft")){record=metadata["beauty_puzzle_draft"];work->draft=true;}
                if(!record.empty())work->document=AI::BeautyDocument::decode(record,id,editor->mesh().indices.size());
                else {work->document.geometry_id=id;work->document.face_count=editor->mesh().indices.size();}
                work->base_file=record.value("puzzle_base_file",path.filename().string());
                const boost::filesystem::path name(work->base_file);
                if(work->base_file.find_first_of("/\\:")!=std::string::npos || name.has_parent_path() || name.filename()!=name || name.extension()!=".glb")throw std::runtime_error("Invalid puzzle base model reference.");
                const auto base=path.parent_path()/name;
                if(boost::filesystem::is_symlink(base) || boost::filesystem::canonical(base).parent_path()!=boost::filesystem::canonical(path.parent_path()))throw std::runtime_error("Invalid puzzle base model location.");
                work->base_hash=AI::model_artifact_sha256(base);
                if(work->base_hash.empty() || work->base_hash!=record.value("puzzle_base_sha256",work->base_hash))throw std::runtime_error("The original puzzle texture is missing or changed.");
                work->base_colors=editor->vertex_colors();
                const bool reusable=reuse_surface && reuse_surface->geometry_id==id && reuse_hash==work->base_hash &&
                    (work->document.face_patch.empty() || work->document.face_patch==reuse_surface->face_patch);
                if(reusable)work->base_colors=reuse_colors;
                else if(base!=path) {
                    TriangleMesh mesh;ObjInfo info;std::string error;
                    if(!AI::load_model_artifact(base,mesh,info,error))throw std::runtime_error(error);
                    if(AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its)!=id)throw std::runtime_error("The original puzzle surface no longer matches.");
                    work->base_colors=std::move(info.vertex_colors);
                }
                work->surface=reusable?reuse_surface:AI::BeautySurface::build(editor->mesh(),work->base_colors,work->document.face_patch,[work]{return work->canceled.load();});
                work->document.face_patch=work->surface->face_patch;
                bool fresh_partition=!record.contains("puzzle");
                {
                    auto& labels=work->semantic_labels;
                    boost::filesystem::path owned;
                    const bool restore=record.contains("puzzle") && !work->regroup && !work->guidance_only;
                    if(!work->guidance_only && !work->regroup) {
                        try {
                            const auto hints=metadata.contains("beauty_guidance")?metadata.at("beauty_guidance"):record.value("guidance",nlohmann::json{});
                            if(!hints.is_null()) {
                                auto saved=AI::BeautyGuidance::decode(hints,id,work->base_hash,editor->mesh().indices.size());
                                if(saved.completed(editor->mesh().indices.size())) {
                                    work->notice=saved.has_features()?"restored":"no_details";
                                    labels=std::move(saved.labels);work->semantic_names=std::move(saved.names);work->eye_details=std::move(saved.details);work->eye_shapes=std::move(saved.eyes);work->guidance_ready=true;
                                }
                            }
                        }catch(const std::exception& e){BOOST_LOG_TRIVIAL(info)<<"Beauty recognition cache rejected: "<<e.what();}
                    }
                    const auto now=int64_t(std::time(nullptr));
                    const bool due=work->guidance_only || work->regroup || AI::beauty_recognition_due(metadata.value("beauty_recognition",nlohmann::json{}),id,work->base_hash,now);
                    if(!work->guidance_ready && !due)work->notice="retry_later";
                    if(!work->guidance_ready && due)try {
                        work->stage=1;
                        work->recognition_attempt={{"geometry",id},{"source",work->base_hash},{"time",now},{"state","failed"},{"reason","unavailable"}};
                        LocalSemanticWorker::Configuration config;std::string reason;
                        if(LocalSemanticWorker::read_runtime_configuration(config_path,installed_runtime,config,reason) && config.enabled) {
                            boost::filesystem::create_directories(requests);owned=requests/boost::filesystem::unique_path("beauty-%%%%-%%%%-%%%%");
                            if(!boost::filesystem::create_directory(owned))throw std::runtime_error("Cannot create local recognition request.");
                            auto analyzed=LocalSemanticWorker::analyze(config,modules,owned,base,editor->mesh(),work->canceled,cache);
                            if(analyzed.process.status==LocalSemanticWorker::Status::Ready) {
                                auto guidance=AI::beauty_feature_guidance(analyzed.evidence,work->surface.get());
                                labels=std::move(guidance.labels);work->semantic_names=std::move(guidance.names);
                                work->eye_shapes=std::move(guidance.eyes);
                                int32_t detail_id=int32_t(work->semantic_names.size());
                                for(auto& eye:work->eye_shapes) {
                                    auto detail=eye.iris_faces.empty()?
                                        AI::BeautyEyeDetail::dark_region(editor->mesh(),work->base_colors,*work->surface,eye.aperture_faces):eye.iris_faces;
                                    if(!detail.empty()) {
                                        eye.iris_faces=detail;
                                        for(size_t f:detail)labels[f]=detail_id;
                                        ++detail_id;work->eye_details.push_back(std::move(detail));
                                        work->semantic_names.push_back("iris");
                                    }
                                }
                                work->notice=analyzed.evidence.known_faces==0?"no_details":"recognized";
                                work->guidance_ready=true;
                                work->recognition_attempt["state"]=analyzed.evidence.known_faces==0?"no_face":"ready";
                                work->recognition_attempt["reason"]="";
                            } else {
                                work->notice="unavailable";
                                work->recognition_attempt["reason"]=analyzed.process.reason;
                                BOOST_LOG_TRIVIAL(info)<<"Beauty local recognition unavailable: "<<analyzed.process.reason;
                            }
                        } else {work->notice="unavailable";work->recognition_attempt["reason"]=reason.empty()?"not_configured":reason;}
                    }catch(const std::exception& e){work->notice="unavailable";BOOST_LOG_TRIVIAL(warning)<<"Beauty local recognition failed: "<<e.what();}
                    if(!owned.empty()) {std::string reason;LocalSemanticWorker::cleanup_request(owned,requests,reason);}
                    if(work->canceled)throw std::runtime_error("Puzzle preparation cancelled.");
                    if(!restore && !work->regroup && !work->guidance_only) {
                        AI::BeautyGuidance refined{labels,work->eye_details,work->eye_shapes,work->semantic_names};
                        AI::beauty_refine_mouth_guidance(*work->surface,AI::beauty_source_face_colors(editor->mesh(),work->base_colors),refined);
                        labels=std::move(refined.labels);work->semantic_names=std::move(refined.names);
                    }
                    work->stage=2;
                    if(work->guidance_only)work->puzzle=current_puzzle;
                    else if(restore) {
                        work->puzzle=AI::BeautyPuzzle::decode(record["puzzle"],id,editor->mesh().indices.size());
                        if(work->guidance_ready && !work->recognition_attempt.is_null() && std::any_of(labels.begin(),labels.end(),[](int32_t n){return n>=0;})) {
                            auto automatic=AI::BeautyPuzzle::create_regions(*work->surface,{},[work]{return work->canceled.load();});
                            automatic.match_filaments(*work->surface,work->puzzle.palette,work->puzzle.mixed_recipes);
                            if(AI::same_automatic_puzzle(work->puzzle,automatic)) {
                                work->before_upgrade=work->puzzle;work->upgraded=true;fresh_partition=true;
                                work->puzzle=AI::BeautyPuzzle::create_regions(*work->surface,labels,[work]{return work->canceled.load();},work->semantic_names);
                            } else work->notice="recognized_preserved";
                        }
                    } else {work->puzzle=AI::BeautyPuzzle::create_regions(*work->surface,labels,[work]{return work->canceled.load();},work->semantic_names);fresh_partition=true;}
                    if(work->regroup) {
                        // Repartitioning may move borders, never erase prior explicit paint.
                        work->puzzle.palette=current_puzzle.palette;
                        work->puzzle.mixed_recipes=current_puzzle.mixed_recipes;
                        for(const auto& entry:current_puzzle.colors) {
                            const auto faces=current_puzzle.faces(entry.first);
                            work->puzzle.assign_region(faces,*work->surface);
                            std::set<uint32_t> ids;for(size_t f:faces)ids.insert(work->puzzle.face_piece[f]);
                            for(uint32_t piece:ids) {
                                if(current_puzzle.filament_slots.count(entry.first))work->puzzle.paint_filament(piece,current_puzzle.filament_slots.at(entry.first));
                                else work->puzzle.paint(piece,entry.second);
                            }
                        }
                    }
                }
                if(saved_record.contains("puzzle"))work->saved_puzzle=AI::BeautyPuzzle::decode(saved_record["puzzle"],id,editor->mesh().indices.size());
                else work->saved_puzzle=work->draft?AI::BeautyPuzzle{}:work->puzzle;
                if(saved_record.contains("edit_regions"))
                    work->saved_edit_regions=AI::BeautyEditRegions::decode(saved_record.at("edit_regions"),id,
                        work->base_hash,editor->mesh().indices.size());
                // A new model keeps its original texture until the user chooses
                // whole-model filament matching. Existing matches follow project changes.
                if(!work->puzzle.palette.empty())
                    work->puzzle.match_filaments(*work->surface,palette.physical_channels,palette.mixed_recipes);
                work->puzzle.validate(*work->surface);
                if(record.contains("edit_regions"))
                    work->edit_regions=AI::BeautyEditRegions::decode(record.at("edit_regions"),id,
                        work->base_hash,editor->mesh().indices.size());
            } catch(const std::exception& e) {work->error=e.what();}
            work->done=true;
        });
    } catch(const std::exception& e) {task.reset();failed=true;message(wxString::FromUTF8(e.what()));}
}
void BeautyWorkbenchControls::pick(size_t face,bool merge) {
    if(!editable || !surface || task || original_view->GetValue() || face>=puzzle.face_piece.size())return;
    const auto id=edit_regions?edit_regions->face_region[face]:puzzle.face_piece[face];
    if(edit_regions){selected=id;render(false);return;}
    if((merge || mode->GetSelection()==3) && selected!=none && selected!=id) {
        try {auto next=puzzle;next.merge(selected,id,*surface);mode->SetSelection(0);update_gesture();commit(std::move(next),selected);}
        catch(const std::exception& e){message(_L("只能合并相邻拼图：")+wxString::FromUTF8(e.what()));}
    } else {selected=id;render(false);}
}
void BeautyWorkbenchControls::stroke(const std::vector<size_t>& faces) {
    if(original_view->GetValue())return;
    if(!editable || !surface || task)return;
    if(edit_regions){message(_L("此测试大区暂不支持画笔调整。"));return;}
    if(stroke_selected==none && stroke_mode!=4){message(_L("先点击一块，再拖动它的边界。"));return;}
    try {
        auto next=puzzle;auto id=stroke_selected;
        if(stroke_mode==1)next.resize_boundary(id,faces,true,*surface);
        else if(stroke_mode==2)next.resize_boundary(id,faces,false,*surface);
        else if(stroke_mode==4)id=next.assign_region(faces,*surface);
        else return;
        mode->SetSelection(0);update_gesture();
        commit(std::move(next),id);
    } catch(const std::exception& e){message(_L("这笔未修改：")+wxString::FromUTF8(e.what()));}
}
nlohmann::json BeautyWorkbenchControls::record(const AI::BeautyPuzzle& value,
                                                const std::optional<AI::BeautyEditRegions>& regions) const {
    auto result=document.encode();result["puzzle"]=value.encode();result["puzzle_base_file"]=base_file;result["puzzle_base_sha256"]=base_hash;
    if(regions)result["edit_regions"]=regions->encode();
    if(guidance_ready)result["guidance"]=AI::BeautyGuidance{semantic_labels,eye_details,eye_shapes,semantic_names}.encode(identity,base_hash);return result;
}
bool BeautyWorkbenchControls::layers_equal(const AI::BeautyPuzzle& left,
                                            const std::optional<AI::BeautyEditRegions>& left_regions,
                                            const AI::BeautyPuzzle& right,
                                            const std::optional<AI::BeautyEditRegions>& right_regions) const {
    if(!left.same_edit(right) || left_regions.has_value()!=right_regions.has_value())return false;
    return !left_regions || (left_regions->geometry_id==right_regions->geometry_id &&
        left_regions->source_sha256==right_regions->source_sha256 &&
        left_regions->face_region==right_regions->face_region);
}
void BeautyWorkbenchControls::save_draft(const AI::BeautyPuzzle& value,
                                          const std::optional<AI::BeautyEditRegions>& regions) {
    if(identity!=preview->geometry_id())throw std::runtime_error("The model changed; reload before editing.");
    DraftRequest request;
    request.model=source;
    request.generation=draft_generation.load();
    if(layers_equal(value,regions,saved_puzzle,saved_edit_regions)) {
        request.remove=true;
        // Migrate the pre-sidecar format lazily when the draft returns to the
        // accepted state. This is rare compared with ordinary edit commits.
        request.clear_legacy=true;
    } else {
        request.record=record(value,regions);
    }
    { std::lock_guard<std::mutex> lock(draft_mutex); draft_pending=std::move(request); }
    draft_cv.notify_one();
}
void BeautyWorkbenchControls::invalidate_draft_queue() {
    draft_generation.fetch_add(1);
    { std::lock_guard<std::mutex> lock(draft_mutex); draft_pending.reset(); }
    draft_cv.notify_one();
}
void BeautyWorkbenchControls::trim(std::vector<Snapshot>& stack) {
    const size_t bytes_per_face=edit_regions?sizeof(uint32_t)*2:sizeof(uint32_t);
    const size_t limit=std::min(size_t(16),std::max(size_t(1),size_t(24*1024*1024)/std::max(size_t(1),puzzle.face_piece.size()*bytes_per_face)));
    if(stack.size()>limit)stack.erase(stack.begin(),stack.end()-limit);
}
void BeautyWorkbenchControls::commit(AI::BeautyPuzzle next,uint32_t id) {
    commit_layers(std::move(next),edit_regions,id);
}
void BeautyWorkbenchControls::commit_layers(AI::BeautyPuzzle next,
                                             std::optional<AI::BeautyEditRegions> next_regions,uint32_t id) {
    if(task)return;
    if(preview->selection_busy()){message(_L("请等待这一笔处理完成。"));return;}
    try {if(!next.palette.empty()) {
            const auto palette=palette_provider.printable_palette();
            next.match_filaments(*surface,palette.physical_channels,palette.mixed_recipes);
        }
        if(layers_equal(next,next_regions,puzzle,edit_regions))return;
        save_draft(next,next_regions);undo.push_back({puzzle,edit_regions,selected});trim(undo);redo.clear();
        puzzle=std::move(next);edit_regions=std::move(next_regions);selected=id;
        dirty=!layers_equal(puzzle,edit_regions,saved_puzzle,saved_edit_regions);render(true);}
    catch(const std::exception& e){message(_L("修改未保存：")+wxString::FromUTF8(e.what()));}
}
void BeautyWorkbenchControls::restore(bool forward) {
    if(!editable || !surface || task || preview->selection_busy())return;
    auto& from=forward?redo:undo;auto& to=forward?undo:redo;if(from.empty())return;
    try {save_draft(from.back().puzzle,from.back().edit_regions);to.push_back({puzzle,edit_regions,selected});trim(to);
        puzzle=std::move(from.back().puzzle);edit_regions=std::move(from.back().edit_regions);
        selected=from.back().selected;from.pop_back();dirty=!layers_equal(puzzle,edit_regions,saved_puzzle,saved_edit_regions);render(true);}
    catch(const std::exception& e){message(wxString::FromUTF8(e.what()));}
}
void BeautyWorkbenchControls::render(bool repaint) {
    if(!surface)return;
    if(surface->boundary_edges || surface->nonmanifold_edges) {
        topology_status->SetLabel(wxString::Format(_L("网格预检：%llu 条开放边，%llu 条非流形边。先处理网格；配色保存不等于可打印。"),
            static_cast<unsigned long long>(surface->boundary_edges),
            static_cast<unsigned long long>(surface->nonmanifold_edges)));
        topology_status->SetForegroundColour(wxColour(188,62,54));
    } else {
        topology_status->SetLabel(_L("网格预检：未发现开放边或非流形边，导入后仍需切片检查。"));
        topology_status->SetForegroundColour(wxColour(31,122,90));
    }
    topology_status->Wrap(FromDIP(250));
    workflow_status->SetLabel(puzzle.palette.empty()
        ? _L("流程：检查网格 → 原色美颜 → 导入时确认耗材配色")
        : _L("流程：检查网格 → 已匹配耗材 → 对照原色再导入"));
    workflow_status->Wrap(FromDIP(250));
    preview->set_puzzle_enabled(editable,!task);
    if(selected!=none && selected_faces().empty())selected=none;
    if(original_view->GetValue()) {auto original=puzzle;original.colors.clear();preview->show_puzzle(*surface,original,base_colors,none,repaint,false);}
    else preview->show_puzzle(*surface,puzzle,base_colors,selected,repaint,borders->GetValue(),
        edit_regions?&edit_regions->face_region:nullptr);
    match_button->Enable(editable && puzzle.palette.empty() && !original_view->GetValue() && !preview->selection_busy());
    focus_button->Enable(selected!=none && !original_view->GetValue() && !preview->selection_busy());
    color_button->Enable(selected!=none && !original_view->GetValue());restore_button->Enable(selected!=none && (edit_regions || puzzle.colors.count(selected)));
    if(selected!=none){const auto c=selected_color();color_button->SetBackgroundColour(wxColour(int(c[0]*255),int(c[1]*255),int(c[2]*255)));color_button->SetForegroundColour(c[0]*.3f+c[1]*.6f+c[2]*.1f<.5f?*wxWHITE:*wxBLACK);}
    undo_button->Enable(!undo.empty());redo_button->Enable(!redo.empty());reset_button->Enable(dirty);
    wxString hint=edit_regions && selected==none?_L("点击大区，双击改色；选中后拖边界。"):
        mode->GetSelection()==4?_L("拖出方框或圈住衣服、鼻子等，建立独立区域。"):
        selected==none?_L("点击选区，双击改色。\n选中后拖边界；Ctrl 拖框可自建区域。"):
        mode->GetSelection()==1?_L("从选中块的边缘往外涂，把旁边扩进来。"):
        mode->GetSelection()==2?_L("沿选中块的边缘涂，把范围还给相邻块。"):
        mode->GetSelection()==3?_L("点击相邻的一块，合并后使用当前块颜色。"):
        mode->GetSelection()==4?_L("拖出方框或圈住衣服、鼻子等，建立独立区域。"):_L("双击改色；拖边界局部修形。\n小块：中心方点移动，四周方点调宽高。");
    hint+=puzzle.palette.empty()?_L("\n未改区域保留原纹理；可选上方整模型预览。"):
        _L("\n已匹配整模型耗材。对照原色，检查肤色、衣服和底座；逐块调整后保存。\n按住空格暂时隐藏分区线。");
    if(selected==none) {
        if(preparation_notice=="no_details")hint=_L("本次未检出可靠五官，已按颜色分区。\n可在更多操作中重新识别。");
        else if(preparation_notice=="unavailable" || preparation_notice=="retry_later")hint=_L("五官识别未完成，当前为颜色分区。\n可在更多操作中重试识别。");
        else if(preparation_notice=="recognized_preserved")hint=_L("五官辅助已补充，保留了现有修改。\n更多操作可补出五官区域，可撤销。");
    }
    const auto palette_hint=puzzle.palette.empty()?_L("原始全彩 · 尚未匹配整模型耗材\n"):
        !puzzle.mixed_recipes.empty()?_L("已匹配当前耗材（含叠色） · "):
        wxString::Format(_L("已匹配 %llu 种耗材 · "),static_cast<unsigned long long>(puzzle.palette.size()));
    if(original_view->GetValue())hint=_L("正在对照原始全彩。\n取消勾选后继续改色和拖边界。");
    message(palette_hint+wxString::Format(_L("%llu 个区域\n"),static_cast<unsigned long long>(edit_regions?edit_regions->count():puzzle.piece_count()))+(dirty?_L("修改待保存 · "):_L("已保存 · "))+hint);
}
void BeautyWorkbenchControls::mark_saved() {
    try {invalidate_draft_queue();
        DraftRequest request;request.model=source;request.remove=true;request.clear_legacy=true;request.durable_cleanup=true;
        { std::lock_guard<std::mutex> lock(draft_mutex); draft_cleanup_pending.push_back(std::move(request)); }
        draft_cv.notify_one();
        dirty=false;saved_puzzle=puzzle;saved_edit_regions=edit_regions;}
    catch(const std::exception& e){message(_L("新版本已保存，但旧草稿清理失败：")+wxString::FromUTF8(e.what()));}
}
void BeautyWorkbenchControls::prepare_options(AI::ModelFinishingOptions& options,const AI::SurfaceSelectionPersistence::SelectionState&) {
    if(!surface || source.empty() || !dirty)throw std::runtime_error("请先修改拼图，再保存新版本。");
    options.beauty_puzzle=true;options.beauty_appearance=false;options.beauty_deform=false;
    options.beauty_document=record(puzzle,edit_regions);options.beauty_surface=surface;
    options.appearance.face_weights.assign(puzzle.face_piece.size(),0);options.appearance.face_target_colors.resize(puzzle.face_piece.size());
    for(size_t f=0;f<puzzle.face_piece.size();++f) {
        const auto c=puzzle.colors.find(puzzle.face_piece[f]);if(c==puzzle.colors.end())continue;
        options.selected_faces.push_back(f);options.appearance.face_weights[f]=1;
        options.appearance.face_target_colors[f]={c->second[0],c->second[1],c->second[2]};
    }
}
nlohmann::json BeautyWorkbenchControls::accepted_document(const AI::ModelFinishingOptions& options,const std::string& output_geometry) {
    auto result=options.beauty_document;result["geometry_id"]=output_geometry;
    result["edits"]=nlohmann::json::array({{{"kind","puzzle"},{"source_geometry",output_geometry}}});return result;
}
void BeautyWorkbenchControls::prepare_import(const boost::filesystem::path& path,AI::ModelImportRequest& request) {
    const auto metadata=read_metadata(path);
    if(!metadata.contains("beauty_workbench"))return;
    const auto& record=metadata.at("beauty_workbench");
    if(!record.contains("puzzle"))return;
    const auto schema=record.at("puzzle").value("schema",std::string{});
    if(schema!="orca.beauty-puzzle/v2" && schema!="orca.beauty-puzzle/v3")return;
    const auto& saved=record.at("puzzle");
    const auto puzzle=AI::BeautyPuzzle::decode(saved,record.at("geometry_id").get<std::string>(),saved.at("face_count").get<size_t>());
    AI::ModelMatchedColors matched;
    matched.source_sha256=metadata.at("model_sha256").get<std::string>();matched.geometry_id=puzzle.geometry_id;matched.palette=puzzle.palette;
    matched.mixed_recipes=puzzle.mixed_recipes;
    for(uint32_t id:puzzle.face_piece) {
        const auto slot=puzzle.filament_slots.find(id);
        if(slot==puzzle.filament_slots.end())throw std::runtime_error("区域配色不完整，请回工作台重新匹配并保存。");
        matched.face_slots.push_back(slot->second);
    }
    if(!matched.valid())throw std::runtime_error("已保存配色无效，请回工作台重新保存。");
    request.matched_colors=std::move(matched);
    request.color_trial.reset();request.face_color_overrides.clear();
}
}
