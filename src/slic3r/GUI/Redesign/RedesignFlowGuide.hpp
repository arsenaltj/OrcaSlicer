#pragma once
#include "RedesignTheme.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/fontenum.h>
#include <wx/control.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace Slic3r::GUI::RedesignArtwork {
inline wxColour flow_background_colour() { return RedesignTheme::flow_background_colour(); }
inline wxString text(const char* value) { return wxString::FromUTF8(value); }
inline wxImage resource_image(const char* name) {
    return wxImage(wxString::FromUTF8(Slic3r::resources_dir() + "/images/" + name));
}
// Break CJK text even when there are no spaces for wxStaticText::Wrap.
inline wxString wrap_text(wxDC& dc, const wxString& value, int width)
{
    wxString wrapped, line;
    for (const auto character : value) {
        const wxString next = line + character;
        if (!line.empty() && dc.GetTextExtent(next).x > width) {
            wrapped += line + '\n';
            line.clear();
        }
        line += character;
    }
    return wrapped + line;
}
// Visual component migrated from 047af2b; no generation or model state is owned here.
class FlowGuideCanvas final : public wxPanel {
public:
    explicit FlowGuideCanvas(wxWindow* parent, std::function<int()> reference_width = {},
                             wxString prompt_count = "13/2000")
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(259)),
                  wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE)
        , m_reference_width(std::move(reference_width))
        , m_prompt_count(std::move(prompt_count))
        , m_design(resource_image("redesign_flow_design.png"))
        , m_model_rear(resource_image("redesign_flow_model_mono.png"))
        , m_model_front(resource_image("redesign_flow_model_blue.png"))
        , m_arrow(resource_image("redesign_flow_arrow_right.png"))
    {
        SetMinSize(wxSize(-1, FromDIP(259)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(flow_background_colour());
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            Refresh(false);
            event.Skip();
        });
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
        const wxSize size = dc.GetMultiLineTextExtent(value);
        dc.DrawLabel(value, wxRect(centre_x - size.x / 2, top_y, size.x, size.y),
                     wxALIGN_CENTER_HORIZONTAL | wxALIGN_TOP);
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
        dc.GetTextExtent(m_prompt_count, &count_width, &count_height);
        dc.DrawText(m_prompt_count, x + width - FromDIP(12 * scale) - count_width,
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

        // Use the drawer-closed width for artwork and fonts. Opening the
        // drawer changes only the column spacing, not the element scale.
        const int reference_width = m_reference_width ? m_reference_width() : client.x;
        const double scale = std::min({1.0, double(std::max(1, reference_width)) / FromDIP(1427),
                                      double(client.y) / FromDIP(259)});
        const auto dip = [this, scale](double value) { return FromDIP(value * scale); };
        const wxFont heading_font = flow_font(14, true, scale);
        const wxFont subtitle_font = flow_font(12, false, scale);
        std::array<wxString, 3> headings {text("上传图片或输入提示词生成图片"), text("生成图片"), text("转为3D")};
        std::array<wxString, 3> subtitles {text("描述想创作的图片"), text("生成图片并完善"), text("获得可打印的专属 3D模型")};
        std::array<int, 3> image_widths {dip(276), dip(139), dip(208)};
        auto widths = image_widths;
        for (size_t i = 0; i < widths.size(); ++i) {
            dc.SetFont(heading_font);
            widths[i] = std::max(widths[i], dc.GetTextExtent(headings[i]).x);
            dc.SetFont(subtitle_font);
            widths[i] = std::max(widths[i], dc.GetTextExtent(subtitles[i]).x);
        }
        const int available = std::max(1, client.x - 2 * FromDIP(12));
        const int minimum_gap = dip(40);
        auto total_width = [&widths] { return widths[0] + widths[1] + widths[2]; };
        // At narrow widths keep every instruction, wrapping within its own
        // column rather than hiding subtitles or shrinking the font again.
        if (total_width() + 2 * minimum_gap > available) {
            widths = image_widths;
            const int spare = std::max(0, available - total_width() - 2 * minimum_gap);
            for (auto& width : widths) width += spare / 3;
            for (size_t i = 0; i < widths.size(); ++i) {
                dc.SetFont(heading_font);
                headings[i] = wrap_text(dc, headings[i], widths[i]);
                dc.SetFont(subtitle_font);
                subtitles[i] = wrap_text(dc, subtitles[i], widths[i]);
            }
        }
        // Extremely narrow windows cannot hold all three images at the
        // reference scale. Fit the artwork as a last resort; retain readable
        // text, its full wording, and separate column bounds.
        const double fit = std::min(1.0, double(std::max(3, available - 2 * minimum_gap)) /
                                         std::max(1, total_width()));
        if (fit < 1.0) {
            for (auto& width : widths) width = std::max(1, int(std::floor(width * fit)));
            for (auto& width : image_widths) width = std::max(1, int(std::floor(width * fit)));
            for (size_t i = 0; i < widths.size(); ++i) {
                headings[i].Replace("\n", ""); subtitles[i].Replace("\n", "");
                dc.SetFont(heading_font); headings[i] = wrap_text(dc, headings[i], widths[i]);
                dc.SetFont(subtitle_font); subtitles[i] = wrap_text(dc, subtitles[i], widths[i]);
            }
        }
        const int gap = std::max(0, std::min((available - total_width()) / 2,
                                            (dip(1067) - total_width()) / 2));
        const int flow_width = total_width() + 2 * gap;
        const int left = (client.x - flow_width) / 2;
        std::array<int, 3> centres {left + widths[0] / 2,
            left + widths[0] + gap + widths[1] / 2,
            left + widths[0] + gap + widths[1] + gap + widths[2] / 2};
        int heading_height = 0, subtitle_height = 0;
        for (size_t i = 0; i < widths.size(); ++i) {
            dc.SetFont(heading_font);
            heading_height = std::max(heading_height, dc.GetMultiLineTextExtent(headings[i]).y);
            dc.SetFont(subtitle_font);
            subtitle_height = std::max(subtitle_height, dc.GetMultiLineTextExtent(subtitles[i]).y);
        }
        const int subtitle_y = std::max(dip(234), dip(207) + heading_height + dip(5));
        const int flow_height = std::max(dip(259), subtitle_y + subtitle_height);
        const int top = std::max(0, (client.y - flow_height) / 2);
        const auto artwork_dip = [this, scale, fit](double value) { return FromDIP(value * scale * fit); };

        draw_card(dc, centres[0] - image_widths[0] / 2, top + artwork_dip(7),
                  image_widths[0], artwork_dip(160), scale * fit);

        const int arrow_size = std::max(1, std::min(dip(93), gap - 2 * dip(6)));
        const wxBitmap arrow = scaled_bitmap(m_arrow, arrow_size, arrow_size);
        if (arrow.IsOk()) {
            dc.DrawBitmap(arrow, left + widths[0] + (gap - arrow_size) / 2,
                          top + (artwork_dip(177) - arrow_size) / 2, true);
            dc.DrawBitmap(arrow, left + widths[0] + gap + widths[1] + (gap - arrow_size) / 2,
                          top + (artwork_dip(177) - arrow_size) / 2, true);
        }

        draw_rotated_design(dc, centres[1] - image_widths[1] / 2, top, image_widths[1], artwork_dip(177));

        const int model_x = centres[2] - image_widths[2] / 2;
        const wxBitmap rear = scaled_bitmap(m_model_rear, artwork_dip(126), artwork_dip(145));
        const wxBitmap front = scaled_bitmap(m_model_front, artwork_dip(146), artwork_dip(167));
        if (rear.IsOk()) dc.DrawBitmap(rear, model_x, top + artwork_dip(11), true);
        if (front.IsOk()) dc.DrawBitmap(front, model_x + artwork_dip(62), top, true);

        const wxColour heading_colour(226, 226, 227);
        const wxColour subtitle_colour(162, 162, 164);
        for (size_t i = 0; i < widths.size(); ++i) {
            draw_centered_text(dc, headings[i], heading_font, heading_colour, centres[i], top + dip(207));
            draw_centered_text(dc, subtitles[i], subtitle_font, subtitle_colour, centres[i], top + subtitle_y);
        }
    }

    std::function<int()> m_reference_width;
    wxString m_prompt_count;
    wxImage m_design;
    wxImage m_model_rear;
    wxImage m_model_front;
    wxImage m_arrow;
};
}
