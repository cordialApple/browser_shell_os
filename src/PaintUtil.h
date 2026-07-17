#pragma once
#include <windows.h>
#include <string>

namespace Paint
{
    constexpr COLORREF kCardBg       = RGB(44,  44,  48);
    constexpr COLORREF kRowHover     = RGB(86,  86,  96);  // lift under the cursor's fan row (≥3:1 vs kCardBg)
    constexpr COLORREF kChipActiveBg = RGB(0xF8, 0x7E, 0x73);
    constexpr COLORREF kTextPrimary  = RGB(220, 220, 220);
    constexpr COLORREF kTextActive   = RGB(38,  20,  16);  // dark warm — legible on coral active chip
    constexpr COLORREF kTextOnBg     = RGB(20,  24,  32);   // dark — legible on the light button pill
    constexpr COLORREF kButtonBg     = RGB(228, 231, 238);  // light pill — pops in the taskbar gap
    constexpr COLORREF kButtonBorder = RGB(120, 124, 132);

    struct Theme
    {
        COLORREF pillTop, pillBottom, pillBorder, pillText;
        COLORREF chipTop, chipBottom, chipBorder, chipText;
        COLORREF activeTop, activeBottom, activeText;
        COLORREF hoverFill;
        bool     gradient;
    };

    const Theme& ActiveTheme();
    void         SetActiveTheme(const std::wstring& name);

    inline void FillVGradient(HDC hdc, const RECT& rc, COLORREF top, COLORREF bottom)
    {
        TRIVERTEX v[2];
        v[0].x     = rc.left;
        v[0].y     = rc.top;
        v[0].Red   = static_cast<COLOR16>(GetRValue(top) << 8);
        v[0].Green = static_cast<COLOR16>(GetGValue(top) << 8);
        v[0].Blue  = static_cast<COLOR16>(GetBValue(top) << 8);
        v[0].Alpha = 0;
        v[1].x     = rc.right;
        v[1].y     = rc.bottom;
        v[1].Red   = static_cast<COLOR16>(GetRValue(bottom) << 8);
        v[1].Green = static_cast<COLOR16>(GetGValue(bottom) << 8);
        v[1].Blue  = static_cast<COLOR16>(GetBValue(bottom) << 8);
        v[1].Alpha = 0;
        GRADIENT_RECT gr = { 0, 1 };
        GradientFill(hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    }

    inline COLORREF Lighten(COLORREF c, int d)
    {
        auto up = [d](int v) { v += d; return v > 255 ? 255 : v; };
        return RGB(up(GetRValue(c)), up(GetGValue(c)), up(GetBValue(c)));
    }

    inline void FillMetallic(HDC hdc, const RECT& rc, COLORREF base, COLORREF shadow)
    {
        const int h = rc.bottom - rc.top;
        const int mid = rc.top + (h * 2) / 5;
        RECT hiR = { rc.left, rc.top, rc.right, mid };
        RECT loR = { rc.left, mid, rc.right, rc.bottom };
        FillVGradient(hdc, hiR, Lighten(base, 26), base);
        FillVGradient(hdc, loR, base, shadow);
    }

    inline int ScalePx(int px, int dpi) { return MulDiv(px, dpi, 96); }

    inline HFONT MakeFont(int ptSize, int weight, int dpi)
    {
        LOGFONTW lf  = {};
        lf.lfHeight  = -MulDiv(ptSize, dpi, 72);
        lf.lfWeight  = weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        return CreateFontIndirectW(&lf);
    }
}
