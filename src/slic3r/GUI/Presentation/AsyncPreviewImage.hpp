#pragma once
#include "UiRuntime.hpp"
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/window.h>

namespace Slic3r::GUI::Presentation {

struct PreviewImageRequest {
    std::string path, revision;
    wxSize bounds;
    int dpi {96}, radius {0};
    bool cover {false};
    bool operator==(const PreviewImageRequest& other) const {
        return path == other.path && revision == other.revision && bounds == other.bounds &&
            dpi == other.dpi && radius == other.radius && cover == other.cover;
    }
};

// No native window, bitmap or GL calls on a worker. This is also the CPU test seam.
wxImage prepare_preview_image(const PreviewImageRequest&, const std::function<bool()>& canceled);

// One replaceable channel, backed by the existing bounded task runner. The
// previous complete bitmap remains available until a matching result installs.
class AsyncPreviewImage {
public:
    explicit AsyncPreviewImage(wxWindow* owner, TaskRunner& tasks = background_tasks());
    ~AsyncPreviewImage();
    void request(PreviewImageRequest request);
    void set_active(bool active);
    const wxBitmap& bitmap() const;
private:
    struct State;
    std::shared_ptr<State> m_state;
    static void launch(const std::shared_ptr<State>&);
};
}
