#pragma once
#include "RedesignTheme.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace Slic3r::GUI::RedesignArtwork {
inline wxColour flow_background_colour() { return RedesignTheme::flow_background_colour(); }
inline wxString text(const char* value) { return wxString::FromUTF8(value); }
inline wxImage resource_image(const char* name) {
    return wxImage(wxString::FromUTF8(Slic3r::resources_dir() + "/images/" + name));
}
// Visual component migrated from 047af2b; no generation or model state is owned here.
class FlowGuideCanvas final : public wxPanel {
public:
    explicit FlowGuideCanvas(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(259)))
        , m_design(resource_image("redesign_flow_design.png"))
        , m_model_rear(resource_image("redesign_flow_model_mono.png"))
        , m_model_front(resource_image("redesign_flow_model_blue.png"))
        , m_arrow(resource_image("redesign_flow_arrow_right.png"))
    {
        SetMinSize(wxSize(-1, FromDIP(259)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(flow_background_colour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    }

private:
    static wxFont flow_font(int point_size, bool medium, double scale = 1.0)
    {
        const wxString family = wxFontEnumerator::IsValidFacename("HONOR Sans Design") ? "HONOR Sans Design" :
                                wxFontEnumerator::IsValidFacename("HarmonyOS Sans SC") ? "HarmonyOS Sans SC" :
                                wxFontEnumerator::IsValidFacename("Microsoft YaHei UI") ? "Microsoft YaHei UI" :
                                wxString();
        wxFontInfo info(std::max(9, static_cast<int>(std::round(point_size * scale))));
        info.Family(wxFONTFAMILY_SWISS);
        info.Weight(medium ? wxFONTWEIGHT_MEDIUM : wxFONTWEIGHT_NORMAL);
        if (!family.empty())
            info.FaceName(family);
        return wxFont(info);
    }

    static void draw_centered_text(wxDC& dc, const wxString& value, const wxFont& font,
                                   const wxColour& colour, int centre_x, int top_y)
    {
        dc.SetFont(font);
        dc.SetTextForeground(colour);
        wxCoord text_width = 0, text_height = 0;
        dc.GetTextExtent(value, &text_width, &text_height);
        const int x = centre_x - static_cast<int>(text_width) / 2;
        dc.DrawText(value, x, top_y);
    }

    static wxBitmap scaled_bitmap(const wxImage& image, int width, int height)
    {
        if (!image.IsOk() || width <= 0 || height <= 0)
            return wxNullBitmap;
        return wxBitmap(image.Scale(width, height, wxIMAGE_QUALITY_HIGH));
    }

    void draw_card(wxDC& dc, int x, int y, int width, int height, double scale)
    {
        dc.SetPen(wxPen(wxColour(98, 98, 101), std::max(1, FromDIP(scale)), wxPENSTYLE_SHORT_DASH));
        dc.SetBrush(wxBrush(wxColour(32, 32, 35)));
        dc.DrawRoundedRectangle(x, y, width, height, FromDIP(12 * scale));
        dc.SetFont(flow_font(11, false, scale));
        dc.SetTextForeground(wxColour(166, 166, 167));
        const auto prompt = wxControl::Ellipsize(text("生成一只可爱的小怪兽手办。"), dc, wxELLIPSIZE_END,
            std::max(1, width - FromDIP(24 * scale)));
        dc.DrawText(prompt, x + FromDIP(12 * scale),
                    y + FromDIP(12 * scale));
        wxCoord count_width = 0, count_height = 0;
        dc.GetTextExtent(text("13/2000"), &count_width, &count_height);
        dc.DrawText(text("13/2000"), x + width - FromDIP(12 * scale) - count_width,
                    y + height - FromDIP(12 * scale) - count_height);
    }

    void draw_rotated_design(wxAutoBufferedPaintDC& dc, int x, int y, int width, int height)
    {
        // The exported PNG contains a 264x344 opaque card inside transparent
        // shadow padding. Match that card to Figma's rotated 132x172 inner
        // frame so its 2.3-degree bounds fit the existing 139x177 slot.
        constexpr double source_width = 328.0;
        constexpr double source_height = 392.0;
        constexpr double card_x = 0.0;
        constexpr double card_y = 20.0;
        constexpr double card_width = 264.0;
        constexpr double card_height = 344.0;
        constexpr double figma_slot_width = 138.736;
        constexpr double figma_slot_height = 176.896;
        constexpr double figma_card_width = 131.95;
        constexpr double figma_card_height = 171.739;
        const double target_card_width = width * figma_card_width / figma_slot_width;
        const double target_card_height = height * figma_card_height / figma_slot_height;
        const int image_width = std::max(1, static_cast<int>(std::floor(
            target_card_width * source_width / card_width)));
        const int image_height = std::max(1, static_cast<int>(std::floor(
            target_card_height * source_height / card_height)));
        const wxBitmap bitmap = scaled_bitmap(m_design, image_width, image_height);
        if (!bitmap.IsOk())
            return;
        const double card_centre_x = (card_x + card_width / 2.0) * image_width / source_width;
        const double card_centre_y = (card_y + card_height / 2.0) * image_height / source_height;
        dc.SetClippingRegion(x, y, width, height);
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->Translate(x + width / 2.0, y + height / 2.0);
            gc->Rotate(2.3 * M_PI / 180.0);
            gc->DrawBitmap(bitmap, -card_centre_x, -card_centre_y, image_width, image_height);
        }
        dc.DestroyClippingRegion();
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(flow_background_colour()));
        dc.Clear();
        const wxSize client = GetClientSize();
        if (client.x <= 0 || client.y <= 0)
            return;

        // The Figma pane containing this surface is 1427px wide.  Scale the
        // 1067px guide composition against that pane, then center it locally.
        const double scale = std::min(1.0, double(client.x) / double(FromDIP(1427)));
        const int flow_width = FromDIP(1067 * scale);
        const int flow_height = FromDIP(259 * scale);
        const int left = (client.x - flow_width) / 2;
        const int top = std::max(0, (client.y - flow_height) / 2);
        const auto dip = [this, scale](double value) { return FromDIP(value * scale); };

        const int prompt_x = left + dip(11), prompt_y = top + dip(7);
        const int prompt_w = dip(276), prompt_h = dip(160);
        draw_card(dc, prompt_x, prompt_y, prompt_w, prompt_h, scale);

        const int arrow_size = dip(93);
        const wxBitmap arrow = scaled_bitmap(m_arrow, arrow_size, arrow_size);
        if (arrow.IsOk()) {
            dc.DrawBitmap(arrow, left + dip(328), top + dip(56), true);
            dc.DrawBitmap(arrow, left + dip(697), top + dip(56), true);
        }

        const int design_x = left + dip(493), design_y = top;
        draw_rotated_design(dc, design_x, design_y, dip(139), dip(177));

        const int model_x = left + dip(840), model_y = top;
        const wxBitmap rear = scaled_bitmap(m_model_rear, dip(126), dip(145));
        const wxBitmap front = scaled_bitmap(m_model_front, dip(146), dip(167));
        if (rear.IsOk()) dc.DrawBitmap(rear, model_x, model_y + dip(11), true);
        if (front.IsOk()) dc.DrawBitmap(front, model_x + dip(62), model_y, true);

        const wxColour heading_colour(226, 226, 227);
        const wxColour subtitle_colour(162, 162, 164);
        const wxFont heading_font = flow_font(14, true, scale);
        const wxFont subtitle_font = flow_font(12, false, scale);
        const bool compact = client.x < FromDIP(700);
        draw_centered_text(dc, compact ? text("图片或描述") : text("上传图片或输入提示词生成图片"), heading_font, heading_colour,
                           left + dip(150), top + dip(207));
        draw_centered_text(dc, text("生成图片"), heading_font, heading_colour,
                           left + dip(563), top + dip(207));
        draw_centered_text(dc, text("转为3D"), heading_font, heading_colour,
                           left + dip(939), top + dip(207));
        if (compact) return;
        constexpr double subtitle_top = 234.0;
        draw_centered_text(dc, text("描述想创作的图片"), subtitle_font, subtitle_colour,
                           left + dip(150), top + dip(subtitle_top));
        draw_centered_text(dc, text("生成图片并完善"), subtitle_font, subtitle_colour,
                           left + dip(563), top + dip(subtitle_top));
        draw_centered_text(dc, text("获得可打印的专属 3D模型"), subtitle_font, subtitle_colour,
                           left + dip(939), top + dip(subtitle_top));
    }

    wxImage m_design;
    wxImage m_model_rear;
    wxImage m_model_front;
    wxImage m_arrow;
};
}
