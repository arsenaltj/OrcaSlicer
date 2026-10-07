#pragma once

#include "ModelGenerationInputStyle.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/dcbuffer.h>
#include <wx/sizer.h>
#include <array>
#include <functional>
#include <algorithm>

namespace Slic3r::GUI {
// Five original Figma slots, projected from the existing viewport's state.
// Button retains the project's focus, keyboard activation and click semantics.
class ModelViewportToolButton final : public Button {
public:
    ModelViewportToolButton(wxWindow* parent, const wxString& label)
        : Button(parent,label), m_icon(this,"figma-ux/viewport-tool-warning",32) {
        SetPaddingSize(wxSize(0,0));
        SetMinSize(FromDIP(wxSize(56,55)));
        SetMaxSize(FromDIP(wxSize(56,55)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        wxFont font=GetFont(); font.SetFaceName(ModelGenerationInputStyle::font_face);
        font.SetPixelSize(FromDIP(wxSize(0,14))); SetFont(font);
        Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(wxColour(32,32,35))); dc.Clear();
            if (m_checked || HasFocus()) {
                dc.SetPen(HasFocus()?wxPen(ModelGenerationInputStyle::yellow):*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(m_checked ? ModelGenerationInputStyle::selected : wxColour(32,32,35)));
                dc.DrawRoundedRectangle(GetClientRect(),FromDIP(8));
            }
            const wxSize icon=m_icon.GetBmpSize();
            dc.DrawBitmap(m_icon.bmp(),(GetClientSize().x-icon.x)/2,0,true);
            dc.SetFont(GetFont());
            dc.SetTextForeground(IsEnabled()?ModelGenerationInputStyle::text:ModelGenerationInputStyle::secondary);
            const wxSize text=dc.GetTextExtent(GetLabel());
            dc.DrawText(GetLabel(),(GetClientSize().x-text.x)/2,icon.y+FromDIP(6));
        });
    }
    void set_checked(bool checked) {if(m_checked!=checked){m_checked=checked;Refresh(false);}}
private:
    ScalableBitmap m_icon;
    bool m_checked=false;
};

class ModelViewportToolbar final : public wxPanel, public AIThemeOwner {
public:
    enum Tool { Remesh, Texture, Mesh, Info, Rotate };
    explicit ModelViewportToolbar(wxWindow* parent) : wxPanel(parent) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(ModelGenerationInputStyle::background)); dc.Clear();
            dc.SetPen(wxPen(wxColour(42,42,45)));
            dc.SetBrush(wxBrush(wxColour(32,32,35)));
            dc.DrawRoundedRectangle(GetClientRect(),FromDIP(12));
        });
        const std::array<wxString,5> labels={_L("重拓扑"),_L("贴图"),_L("网格"),_L("信息"),_L("自动旋转")};
        for (size_t i=0;i<labels.size();++i) {
            m_tools[i]=new ModelViewportToolButton(this,labels[i]);
            m_tools[i]->Bind(wxEVT_BUTTON,[this,i](wxCommandEvent&){if(on_tool)on_tool(Tool(i));});
        }
        m_divider=new wxPanel(this);
        m_divider->SetBackgroundColour(wxColour(57,57,60));
        Bind(wxEVT_SIZE,[this](wxSizeEvent& event){arrange(GetClientSize().x);event.Skip();});
        m_tools[Remesh]->Enable(false);
        m_tools[Remesh]->SetToolTip(_L("当前本地工作台没有重拓扑能力。整体美颜仅修整表面，不会自动修补拓扑。"));
        m_tools[Remesh]->EnableTooltipEvenDisabled();
        m_tools[Texture]->SetToolTip(_L("显示原始贴图与颜色，关闭后以灰模观察；只改变显示。"));
        m_tools[Mesh]->SetToolTip(_L("显示或隐藏三角网格；模型几何和选区不变。"));
        m_tools[Info]->SetToolTip(_L("显示或隐藏当前模型信息；视角和编辑不变。"));
        m_tools[Rotate]->SetToolTip(_L("自动旋转当前视角；再次点击或手动拖动即停止。"));
    }
    int height_for_width(int width) const {
        const int columns=columns_for_width(width);
        const int rows=(int(m_tools.size())+columns-1)/columns;
        return rows*FromDIP(55)+(rows-1)*FromDIP(8)+FromDIP(12);
    }
    // Keep original asset/button geometry. Gaps and row arrangement adapt
    // when a compact viewport cannot fit the original five-slot strip.
    void arrange(int width) {
        const int columns=columns_for_width(width);
        const int item=FromDIP(56),gap=FromDIP(columns==5 && width<FromDIP(363)?6:13);
        const bool single_row=columns==5;
        m_divider->Show(single_row);
        for(size_t i=0;i<m_tools.size();++i) {
            const int row=int(i)/columns, col=int(i)%columns;
            const int count=std::min(columns,int(m_tools.size())-row*columns);
            const int span=count*item+(count-1)*gap+(single_row?FromDIP(14):0);
            const int x=(width-span)/2+col*(item+gap)+(single_row && col>=2?FromDIP(14):0);
            const wxRect bounds(x,FromDIP(6)+row*FromDIP(63),item,FromDIP(55));
            if(m_tools[i]->GetRect()!=bounds)m_tools[i]->SetSize(bounds);
            if(single_row && i==2)
                m_divider->SetSize(wxRect(x-FromDIP(14),FromDIP(18),FromDIP(1),FromDIP(30)));
        }
    }
    void apply_ai_theme(bool) override {SetBackgroundColour(ModelGenerationInputStyle::background);Refresh(false);}
    void set_state(Tool tool, bool enabled, bool checked) {
        m_tools[tool]->Enable(enabled);m_tools[tool]->set_checked(checked);
    }
    std::function<void(Tool)> on_tool;
private:
    int columns_for_width(int width) const {
        return width>=FromDIP(330)?5:width>=FromDIP(214)?3:width>=FromDIP(145)?2:1;
    }
    std::array<ModelViewportToolButton*,5> m_tools;
    wxPanel* m_divider=nullptr;
};
} // namespace Slic3r::GUI
