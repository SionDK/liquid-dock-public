#include "settings_window.h"
#include "settings_window_internal.h"
#include "dock.h"
#include "theme.h"
#include "oled.h"
#include "buttons.h"
#include "sensors.h"
#include "admin_helper.h"
#include "version.h"
#include <dwmapi.h>
#include <map>
#include "system.h"

namespace settingswindow {

// ---------------------------------------------------------------- button customizer (buttons tab)
// A preview of the dock's tray drawn with the dock's own pairing (PlanButtons) and icons
// (DrawButtonIcon), and the buttons not on the dock. Drag a button from the list onto the preview to
// add it, along the preview to move it, off the preview to take it away. A click on a listed button
// adds it at the end; right-click offers the same as menu items.

bool IsNav(int control) { return control <= -10 && control > -20; }

ButtonDrag g_bd;

namespace {

D2D1_RECT_F g_preview{};                 // the preview panel (drop target)
std::vector<float> g_dropCentres;        // centres of the chosen buttons without the dragged one

IDWriteTextFormat* GlyphFont(float size) {
    int key = (int)std::round(size * 10);
    auto& f = g_glyphFonts[key];
    if (!f) {
        wchar_t family[64] = L"Segoe Fluent Icons";
        if (g_gfx.fmtGlyph) g_gfx.fmtGlyph->GetFontFamilyName(family, ARRAYSIZE(family));
        f = Font(key / 10.f, DWRITE_FONT_WEIGHT_NORMAL, family);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    return f.Get();
}

// Where the dragged button lands: how many of the other chosen buttons lie left of the pointer.
int DropIndex(float x) {
    int i = 0;
    for (float c : g_dropCentres) if (c < x) ++i;
    return i;
}

bool OverPreview(float x, float y) {
    return x >= g_preview.left - 12 && x <= g_preview.right + 12 && y >= g_preview.top - 16 && y <= g_preview.bottom + 16;
}

}   // namespace

// The chosen list as it would be after dropping here (the list itself while nothing is dragged).
std::vector<ButtonId> ShownList() {
    auto list = ChosenButtons();
    if (!g_bd.active) return list;
    list.erase(std::remove(list.begin(), list.end(), g_bd.id), list.end());
    if (OverPreview(g_bd.x, g_bd.y)) list.insert(list.begin() + std::min<size_t>(DropIndex(g_bd.x), list.size()), g_bd.id);
    return list;
}

void CommitButtons(const std::vector<ButtonId>& list) {
    g_settings.trayButtons = FormatButtons(list);
    g_settings.Save();
    PostMessageW(g_mainWnd, WM_APP_RELAYOUT, 0, 0);
}

float DrawCustomizer(float x0, float y, float x1) {
    auto* dc = g_gfx.dc.Get();
    const float top = y;
    const IconState st = CurrentIconState(NowSeconds());
    Draw(L"독에 표시할 버튼", g_fLabel.Get(), { x0, y, x1, y + 22 }, Text());
    y += 24;
    Draw(L"아래 목록에서 독으로 끌어다 놓으세요. 독 밖으로 끌어내면 빠집니다.", g_fHint.Get(), { x0, y, x1, y + 18 }, Muted());
    y += 28;

    // ---- preview: the tray end of the dock, at the dock's proportions
    g_preview = { x0, y, x1, y + 112 };
    Fill(g_preview, Wash(g_bd.active && OverPreview(g_bd.x, g_bd.y) ? 0.09f : 0.05f), 8);
    std::vector<ButtonId> chosen = ShownList(), group1 = chosen;
    group1.push_back(ButtonId::Settings);
    const std::vector<ButtonId> group0 = { ButtonId::Network, ButtonId::Volume };
    const float pct = std::clamp(g_settings.dockThickness, 75, 150) / 100.f;
    constexpr float spacing[] = { 0.22f, 0.32f, 0.44f };
    const float gapRatio = g_settings.iconGap > 0 ? std::clamp(g_settings.iconGap, 10, 60) / 100.f : spacing[std::clamp(g_settings.iconSpacing, 0, 2)];
    float k = 0.62f, icon = 0, thick = 0, pad = 0, gap = 0, width = 0;
    bool stack = false;
    const battery::State bat = g_sys.Battery();   // the battery cell, on a PC with a battery
    auto batteryWidth = [&] { return bat.present ? std::max(BatteryIconWidth(icon, k), 30 * k) + 10 * k : 0.0f; };
    for (int pass = 0; pass < 2; ++pass) {   // shrink once if the bar does not fit the panel
        icon = std::round(g_settings.iconSize * 0.78f * k);
        thick = icon / 0.6f * pct; pad = (thick - icon) / 2; gap = icon * gapRatio;
        stack = thick * 0.5f >= 22 * k;
        width = 2 * pad + 3 * (icon + gap) + 1 + gap + icon * 0.45f + 46 * k + 58 * k + batteryWidth()
              + PlanButtons(group0, icon, k, stack, 0, nullptr) + PlanButtons(group1, icon, k, stack, 0, nullptr);
        float avail = g_preview.right - g_preview.left - 24;
        if (width <= avail) break;
        k *= avail / width;
    }
    float bx = std::round((g_preview.left + g_preview.right - width) / 2), by = std::round((g_preview.top + g_preview.bottom - thick) / 2);
    D2D1_RECT_F bar = { bx, by, bx + width, by + thick };
    Fill(bar, Wash(0.10f), thick * 0.4f);
    g_brush->SetColor(Wash(0.16f));
    dc->DrawRoundedRectangle({ bar, thick * 0.4f, thick * 0.4f }, g_brush.Get(), 1);
    float x = bx + pad;
    const D2D1_COLOR_F apps[3] = { ColorF(0xF2A541), ColorF(0x4CC38A), ColorF(0x5B8DEF) };
    for (auto col : apps) {
        col.a = 0.85f;
        Fill({ x, by + pad, x + icon, by + pad + icon }, col, icon * 0.22f);
        x += icon + gap;
    }
    Fill({ x + gap / 2, by + pad * 1.2f, x + gap / 2 + 1, by + thick - pad * 1.2f }, Wash(0.35f));
    x += 1 + gap;
    IDWriteTextFormat* glyphs = GlyphFont(17 * k);
    Draw(L"", glyphs, { x, by, x + icon * 0.45f, by + thick }, Text(0.85f));
    x += icon * 0.45f;
    auto cellRect = [&](const PlannedCell& c) {
        D2D1_RECT_F r = { c.u0, by, c.u1, by + thick };
        if (c.half == 0) r.bottom = by + thick / 2; else if (c.half == 1) r.top = by + thick / 2;
        return r;
    };
    std::vector<PlannedCell> cells;
    x = PlanButtons(group0, icon, k, stack, x, &cells);
    for (auto& c : cells) DrawButtonIcon(c.id, dc, g_brush.Get(), glyphs, cellRect(c), k, stack ? 0.8f : 1.0f, Text(), 0, st);
    Draw(g_sys.timeText, g_fTiny.Get(), { x, by + thick / 2 - 15, x + 46 * k, by + thick / 2 }, Text());
    Draw(g_sys.dateText.size() > 5 ? g_sys.dateText.substr(g_sys.dateText.size() - 5) : g_sys.dateText, g_fTiny.Get(),
         { x, by + thick / 2, x + 46 * k, by + thick / 2 + 15 }, Muted());
    x += 46 * k;
    Draw(L"CPU", g_fTiny.Get(), { x, by, x + 58 * k, by + thick }, Muted());
    x += 58 * k;
    if (bat.present) {   // as on the dock: the icon over the percentage
        const float bw = batteryWidth(), iw = BatteryIconWidth(icon, k), ih = BatteryIconHeight(icon, k);
        const float left = std::round(x + (bw - iw) / 2), top = std::round(by + thick / 2 - ih - 1);
        DrawBatteryIcon(dc, g_brush.Get(), { left, top, left + iw, top + ih }, k, Text(), 0, bat);
        Draw(battery::PercentText(bat), g_fTiny.Get(), { x, by + thick / 2, x + bw, by + thick / 2 + 15 }, Text());
        x += bw;
    }
    PlanButtons(group1, icon, k, stack, x, &cells);
    // where a drop would land: centres of the chosen buttons without the dragged one
    g_dropCentres.clear();
    auto chosenNow = ChosenButtons();
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& c = cells[i];
        auto r = cellRect(c);
        bool dragged = g_bd.active && c.id == g_bd.id;
        if (c.id != ButtonId::Settings && !dragged) g_dropCentres.push_back((r.left + r.right) / 2 + (c.half == 1 ? 0.01f : 0));
        if (dragged) {   // where it will go
            g_brush->SetColor(g_accent);
            dc->DrawRoundedRectangle({ { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, 6, 6 }, g_brush.Get(), 2);
        } else if (c.id != ButtonId::Settings && g_hover == kHotDock - (int)i) {
            Fill({ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, Wash(0.10f), 6);
        }
        DrawButtonIcon(c.id, dc, g_brush.Get(), glyphs, r, k, stack ? 0.8f : 1.0f, Text(c.id == ButtonId::Settings ? 0.55f : 1), 0, st);
        if (c.id != ButtonId::Settings && !g_bd.active) {
            auto at = std::find(chosenNow.begin(), chosenNow.end(), c.id);
            if (at != chosenNow.end()) g_hots.push_back({ r, kHotDock - (int)(at - chosenNow.begin()), 0 });
        }
    }
    y = g_preview.bottom + 18;

    // ---- the buttons not on the dock
    Draw(L"사용 가능한 버튼", g_fLabel.Get(), { x0, y, x1, y + 22 }, Text());
    y += 30;
    std::vector<ButtonId> rest;
    for (int i = 0; i < kButtonCount; ++i) {
        auto id = (ButtonId)i;
        if (!Info(id).fixed && std::find(chosen.begin(), chosen.end(), id) == chosen.end()) rest.push_back(id);
    }
    if (rest.empty()) { Draw(L"모든 버튼이 독에 있습니다", g_fHint.Get(), { x0, y, x1, y + 20 }, Muted()); y += 30; }
    const int cols = 3;
    const float cw = (x1 - x0 - (cols - 1) * 8) / cols, ch = 44;
    for (size_t i = 0; i < rest.size(); ++i) {
        float cx = x0 + (i % cols) * (cw + 8), cy = y + (i / cols) * (ch + 8);
        D2D1_RECT_F r = { cx, cy, cx + cw, cy + ch };
        bool hot = g_hover == kHotPalette - (int)rest[i];
        Fill(r, Wash(hot ? 0.10f : 0.06f), 6);
        DrawButtonIcon(rest[i], dc, g_brush.Get(), GlyphFont(15), { cx + 6, cy + 6, cx + 38, cy + ch - 6 }, 0.85f, 1, Text(), 0, st);
        Draw(Info(rest[i]).name, g_fChip.Get(), { cx + 42, cy, r.right - 6, r.bottom }, Text());
        if (!g_bd.active) g_hots.push_back({ r, kHotPalette - (int)rest[i], 0 });
    }
    y += ((rest.size() + cols - 1) / cols) * (ch + 8) + 8;

    // ---- the button being dragged follows the pointer
    if (g_bd.active) {
        D2D1_RECT_F g = { g_bd.x - 20, g_bd.y - 20, g_bd.x + 20, g_bd.y + 20 };
        Fill(g, g_dark ? ColorF(0.25f, 0.25f, 0.28f, 0.92f) : ColorF(1, 1, 1, 0.95f), 8);
        g_brush->SetColor(g_accent);
        dc->DrawRoundedRectangle({ g, 8, 8 }, g_brush.Get(), 1.5f);
        DrawButtonIcon(g_bd.id, dc, g_brush.Get(), GlyphFont(15), g, 0.85f, 1, Text(), 0, st);
    }
    return y - top;
}

void ButtonMenu(int hot, POINT screen) {
    auto list = ChosenButtons();
    HMENU m = CreatePopupMenu();
    if (hot <= kHotPalette) {
        AppendMenuW(m, MF_STRING, 1, L"독에 추가");
    } else {
        int i = kHotDock - hot;
        AppendMenuW(m, MF_STRING | (i > 0 ? 0 : MF_GRAYED), 2, L"왼쪽으로");
        AppendMenuW(m, MF_STRING | (i + 1 < (int)list.size() ? 0 : MF_GRAYED), 3, L"오른쪽으로");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 4, L"독에서 빼기");
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) list.push_back((ButtonId)(kHotPalette - hot));
    else if (cmd >= 2) {
        int i = kHotDock - hot;
        if (cmd == 2) std::swap(list[i], list[i - 1]);
        else if (cmd == 3) std::swap(list[i], list[i + 1]);
        else list.erase(list.begin() + i);
    }
    if (cmd) CommitButtons(list);
}

}   // namespace settingswindow
