#pragma once

#include "RedesignTheme.hpp"
#include "../I18N.hpp"
#include "slic3r/Utils/PrinterFilamentComparison.hpp"

#include <vector>
#include <wx/dcbuffer.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r::GUI {

class PrinterTransferFilamentPanel final : public wxPanel
{
    class Swatch final : public wxPanel {
    public:
        explicit Swatch(wxWindow* parent) : wxPanel(parent)
        {
            SetBackgroundStyle(wxBG_STYLE_PAINT);
            SetMinSize(FromDIP(wxSize(18, 18)));
            Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(this);
                dc.SetBackground(wxBrush(RedesignTheme::panel_colour()));
                dc.Clear();
                if (m_color.IsOk()) {
                    dc.SetPen(wxPen(wxColour(110, 110, 115)));
                    dc.SetBrush(wxBrush(m_color));
                    dc.DrawRoundedRectangle(GetClientRect(), FromDIP(3));
                }
            });
        }

        void set_color(const std::string& value)
        {
            const auto color = transfer_filament_color(value);
            m_color = color.empty() ? wxColour{} : wxColour(wxString::FromUTF8(color));
            Refresh(false);
        }

    private:
        wxColour m_color;
    };

public:
    explicit PrinterTransferFilamentPanel(wxWindow* parent) : wxPanel(parent)
    {
        SetBackgroundColour(RedesignTheme::panel_colour());
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* headers = new wxBoxSizer(wxHORIZONTAL);
        headers->Add(label(_L("切片用料"), true), 1, wxEXPAND);
        headers->AddSpacer(FromDIP(38));
        headers->Add(label(_L("设备实际用料"), true), 1, wxEXPAND);
        layout->Add(headers, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
        m_rows_layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(m_rows_layout, 0, wxEXPAND);
        SetSizer(layout);
    }

    void set_filaments(const std::vector<SlicedTransferFilament>& filaments)
    {
        m_rows.clear();
        m_rows_layout->Clear(true);
        if (filaments.empty()) {
            m_rows_layout->Add(label(_L("未核实")), 0, wxBOTTOM, FromDIP(8));
            m_advice = nullptr;
        }
        for (const auto& source : filaments) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* sliced_swatch = new Swatch(this);
            sliced_swatch->set_color(source.filament.color);
            row->Add(sliced_swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            wxString description = wxString::Format(_L("耗材 %d · %s"), source.id + 1,
                source.filament.material.empty() ? _L("未核实") : wxString::FromUTF8(source.filament.material));
            if (source.filament.color.empty() && !source.filament.material.empty())
                description += _L(" · 颜色未核实");
            row->Add(label(description), 1, wxALIGN_CENTER_VERTICAL);
            auto* arrow = label(L"→");
            arrow->SetMinSize(FromDIP(wxSize(16, 20)));
            row->Add(arrow, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(12));
            auto* device_swatch = new Swatch(this);
            row->Add(device_swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            auto* device_label = label(_L("未核实"));
            row->Add(device_label, 1, wxALIGN_CENTER_VERTICAL);
            m_rows.push_back({source, device_swatch, device_label});
            m_rows_layout->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        }
        if (!filaments.empty()) {
            m_advice = label(wxEmptyString);
            m_advice->SetMinSize(FromDIP(wxSize(0, 38)));
            RedesignTheme::style_text(m_advice, RedesignTheme::accent_colour(), 10);
            m_rows_layout->Add(m_advice, 0, wxEXPAND);
        }
        Layout();
    }

    void show_device_filaments(const DeviceTransferFilaments& filaments, bool loading = false)
    {
        bool difference = false;
        for (const auto& row : m_rows) {
            const auto device = row.source.tool ? filaments.find(*row.source.tool) : filaments.end();
            wxString description = loading && row.source.tool ? _L("正在读取…") : _L("未核实");
            std::string color;
            if (device != filaments.end()) {
                const auto& actual = device->second;
                const auto match = compare_transfer_filaments(row.source.filament, actual.filament);
                difference |= match == TransferFilamentMatch::Different;
                color = actual.filament.color;
                description = wxString::Format(_L("料槽 %d · %s"), actual.slot + 1,
                    actual.filament.material.empty() ? _L("未核实") : wxString::FromUTF8(actual.filament.material));
                if (color.empty() && !actual.filament.material.empty())
                    description += _L(" · 颜色未核实");
            }
            row.swatch->set_color(color);
            row.label->SetLabelText(description);
            row.label->SetToolTip(description);
        }
        if (m_advice) {
            m_advice->SetLabelText(difference ?
                _L("继续将使用设备当前耗材。如需尽量符合切片预期，请先更换对应耗材。") : wxString{});
            if (difference)
                m_advice->Wrap(GetClientSize().x > 0 ? GetClientSize().x : FromDIP(600));
        }
    }

private:
    wxStaticText* label(const wxString& text, bool heading = false)
    {
        auto* result = new wxStaticText(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize,
                                         wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
        RedesignTheme::style_text(result, heading ? RedesignTheme::secondary_text_colour() : RedesignTheme::primary_text_colour(),
                                  10, heading);
        result->SetMinSize(FromDIP(wxSize(0, 20)));
        return result;
    }

    struct Row {
        SlicedTransferFilament source;
        Swatch* swatch;
        wxStaticText* label;
    };
    std::vector<Row> m_rows;
    wxBoxSizer* m_rows_layout;
    wxStaticText* m_advice = nullptr;
};

} // namespace Slic3r::GUI
