#pragma once
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Model/BeautyEditRegions.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticEvidence.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include <wx/panel.h>
#include <wx/timer.h>
#include <thread>
#include <atomic>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <deque>

class wxChoice; class wxCheckBox; class wxSlider; class wxStaticText; class wxButton;
namespace Slic3r::GUI {
class ModelPreview3D;
class BeautyWorkbenchControls : public wxPanel {
public:
    BeautyWorkbenchControls(wxWindow*, ModelPreview3D*, AI::IPrintablePaletteProvider&, std::function<void()> layout_changed);
    ~BeautyWorkbenchControls();
    void synchronize(const boost::filesystem::path&, bool editable, bool visible);
    void update_gesture();
    bool has_changes() const {return dirty;}
    bool ready() const {return bool(surface) && !failed && !task;}
    void mark_saved();
    void prepare_options(AI::ModelFinishingOptions&, const AI::SurfaceSelectionPersistence::SelectionState&);
    static nlohmann::json accepted_document(const AI::ModelFinishingOptions&, const std::string& output_geometry);
    static void prepare_import(const boost::filesystem::path&, AI::ModelImportRequest&);
    std::function<void(const std::string&)> add_native_mixed_filament;
private:
    static constexpr uint32_t none=UINT32_MAX;
    struct Preparation {
        std::atomic<bool> canceled {false}, done {false};
        std::atomic<int> stage {0};
        std::shared_ptr<const AI::BeautySurface> surface;
        AI::BeautyDocument document;
        AI::BeautyPuzzle puzzle;
        AI::BeautyPuzzle saved_puzzle;
        std::optional<AI::BeautyEditRegions> edit_regions;
        std::optional<AI::BeautyEditRegions> saved_edit_regions;
        std::vector<std::array<float,4>> base_colors;
        std::vector<int32_t> semantic_labels;
        std::vector<std::string> semantic_names;
        std::vector<std::vector<size_t>> eye_details;
        std::vector<LocalSemanticEvidence::EyeDetail> eye_shapes;
        std::string base_file,base_hash,error,notice;
        nlohmann::json recognition_attempt;
        AI::BeautyPuzzle before_upgrade;
        bool upgraded=false;
        bool draft=false,regroup=false,guidance_only=false,guidance_ready=false;
    };
    struct Snapshot {AI::BeautyPuzzle puzzle;std::optional<AI::BeautyEditRegions> edit_regions;uint32_t selected=none;};
    ModelPreview3D* preview;
    AI::IPrintablePaletteProvider& palette_provider;
    std::function<void()> changed;
    wxChoice* mode;
    wxCheckBox* borders;
    wxCheckBox* original_view;
    wxStaticText* status;
    wxStaticText* topology_status;
    wxStaticText* workflow_status;
    wxSlider* radius;
    wxButton *color_button,*restore_button,*undo_button,*redo_button,*reset_button;
    wxButton* more_button;
    wxButton* match_button;
    wxButton* focus_button;
    wxTimer timer;
    std::thread worker;
    struct DraftRequest {
        boost::filesystem::path model;
        nlohmann::json record;
        uint64_t generation=0;
        bool remove=false;
        bool clear_legacy=false;
        bool durable_cleanup=false;
    };
    std::thread draft_worker;
    std::mutex draft_mutex;
    std::condition_variable draft_cv;
    std::optional<DraftRequest> draft_pending;
    std::deque<DraftRequest> draft_cleanup_pending;
    std::atomic<bool> draft_cancel{false};
    std::atomic<uint64_t> draft_generation{0};
    std::shared_ptr<Preparation> task;
    std::shared_ptr<const AI::BeautySurface> surface;
    std::shared_ptr<const AI::BeautySurface> cached_surface;
    std::vector<std::array<float,4>> cached_base_colors;
    std::string cached_base_hash;
    AI::BeautyDocument document;
    AI::BeautyPuzzle puzzle;
    std::optional<AI::BeautyEditRegions> edit_regions;
    std::vector<std::array<float,4>> base_colors;
    std::vector<int32_t> semantic_labels;
    std::vector<std::string> semantic_names;
    std::vector<std::vector<size_t>> eye_details;
    std::vector<LocalSemanticEvidence::EyeDetail> eye_shapes;
    std::vector<Snapshot> undo,redo;
    boost::filesystem::path source;
    std::string identity,base_file,base_hash;
    uint32_t selected=none,stroke_selected=none;
    int stroke_mode=0;
    int shown_stage=-1;
    bool editable=false,failed=false,dirty=false,regroup_requested=false;
    bool guidance_requested=false,guidance_ready=false;
    std::string preparation_notice;
    AI::BeautyPuzzle saved_puzzle;
    std::optional<AI::BeautyEditRegions> saved_edit_regions;
    std::vector<size_t> selected_faces() const;
    std::array<float,4> selected_color() const;
    void restore_selected_color();
    void tick();
    void pick(size_t,bool merge=false);
    void change_color();
    void match_colors();
    void more_actions();
    void stroke(const std::vector<size_t>&);
    void commit(AI::BeautyPuzzle,uint32_t);
    void commit_layers(AI::BeautyPuzzle,std::optional<AI::BeautyEditRegions>,uint32_t);
    void restore(bool forward);
    void render(bool repaint);
    void save_draft(const AI::BeautyPuzzle&,const std::optional<AI::BeautyEditRegions>&);
    void invalidate_draft_queue();
    nlohmann::json record(const AI::BeautyPuzzle&,const std::optional<AI::BeautyEditRegions>&) const;
    bool layers_equal(const AI::BeautyPuzzle&,const std::optional<AI::BeautyEditRegions>&,
                      const AI::BeautyPuzzle&,const std::optional<AI::BeautyEditRegions>&) const;
    void message(const wxString&);
    void trim(std::vector<Snapshot>&);
};
}
