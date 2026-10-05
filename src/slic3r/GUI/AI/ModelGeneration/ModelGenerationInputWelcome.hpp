#pragma once
#include "ModelGenerationInputStyle.hpp"

namespace Slic3r::GUI::ModelGenerationInputStyle {
class Welcome final : public wxPanel, public AIThemeOwner {
public:
    void apply_ai_theme(bool) override { Refresh(false); }
    explicit Welcome(wxWindow* parent) : wxPanel(parent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(1, 1));
        const auto root = Slic3r::resources_dir() + "/images/figma-ux/";
        for (size_t i = 0; i < m_images.size(); ++i)
            m_images[i].LoadFile(wxString::FromUTF8(root + std::array<const char*, 4>{
                "design.png", "arrow.png", "model_gray.png", "model_color.png"}[i]));
        // Source image 262 points up. The frame rotates it clockwise by 90°.
        if (m_images[1].IsOk()) m_images[1] = m_images[1].Rotate90();
        if (m_images[0].IsOk()) {
            const int w = m_images[0].GetWidth(), h = m_images[0].GetHeight();
            wxImage crop = m_images[0].GetSubImage(wxRect(int(w * .18), int(h * .05), int(w * .70), int(h * .92)));
            m_images[0] = crop;
        }
        Bind(wxEVT_PAINT, &Welcome::paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Refresh(false); event.Skip(); });
    }
private:
    void paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(background)); dc.Clear();
        const auto size = GetClientSize();
        const bool compact = size.x < FromDIP(700);
        const double scale = std::min(1.0, double(size.x) / FromDIP(1050));
        const auto dip = [this, scale](double v) { return std::max(1, int(FromDIP(int(v)) * scale)); };
        const int left = (size.x - dip(1000)) / 2;
        const int top = std::max(dip(40), (size.y - dip(390)) / 2);
        auto label = [&](const wxString& value, int center, int y, int points, const wxColour& color) {
            wxFont font = wxGetApp().normal_font();
            font.SetFaceName("HONOR SANS Design");
            font.SetPointSize(std::max(9, int(points * scale)));
            dc.SetFont(font); dc.SetTextForeground(color);
            dc.DrawText(value, center - dc.GetTextExtent(value).x / 2, y);
        };
        auto image = [&](size_t i, int x, int y, int w, int h) {
            if (!m_images[i].IsOk()) return;
            if (!m_scaled[i].IsOk() || m_scaled[i].GetWidth() != w || m_scaled[i].GetHeight() != h)
                m_scaled[i] = wxBitmap(m_images[i].Scale(w, h, wxIMAGE_QUALITY_HIGH));
            dc.DrawBitmap(m_scaled[i], x, y, true);
        };
        label(compact ? _L("图片或文字 → 2D 设计图 → 3D 模型") :
            _L("上传图片或描述想法，创建专属模型"), size.x / 2, top, 25, text);
        const int row = top + dip(120);
        dc.SetPen(wxPen(wxColour(90, 90, 95), 1, wxPENSTYLE_SHORT_DASH));
        dc.SetBrush(wxBrush(panel));
        dc.DrawRoundedRectangle(left + dip(15), row + dip(8), dip(260), dip(145), dip(12));
        wxFont font = wxGetApp().normal_font(); font.SetFaceName("HONOR SANS Design");
        font.SetPointSize(std::max(9, int(11 * scale)));
        dc.SetFont(font); dc.SetTextForeground(secondary);
        dc.DrawText(compact ? _L("一只小怪兽") : _L("生成一只可爱的小怪兽手办。"),
            left + dip(27), row + dip(20));
        if (!compact) dc.DrawText(_L("描述示例"), left + dip(27), row + dip(128));
        image(1, left + dip(310), row + dip(48), dip(86), dip(86));
        // Figma uses a portrait crop of the square source photograph.
        image(0, left + dip(456), row, dip(130), dip(170));
        image(1, left + dip(660), row + dip(48), dip(86), dip(86));
        image(2, left + dip(790), row + dip(12), dip(117), dip(135));
        image(3, left + dip(849), row, dip(136), dip(155));
        const int captions = row + dip(195);
        label(compact ? _L("输入想法") : _L("上传图片或输入提示词生成图片"),
            left + dip(145), captions, 12, text);
        label(_L("生成图片"), left + dip(521), captions, 12, text);
        label(_L("转为3D"), left + dip(900), captions, 12, text);
        // Minimum readable fonts cannot shrink with the artwork. At narrow
        // widths keep one caption per step instead of overlapping two rows.
        if (!compact) {
            const int detail_y = captions + std::max(dip(27), dc.GetCharHeight() + FromDIP(6));
            label(_L("描述想创作的图片"), left + dip(145), detail_y, 11, secondary);
            label(_L("生成图片并完善"), left + dip(521), detail_y, 11, secondary);
            label(_L("获得可打印的专属 3D 模型"), left + dip(900), detail_y, 11, secondary);
        }
    }
    std::array<wxImage, 4> m_images;
    std::array<wxBitmap, 4> m_scaled;
};
} // namespace Slic3r::GUI::ModelGenerationInputStyle
