#pragma once
#include <windows.h>

namespace Paint
{
    constexpr COLORREF kCardBg       = RGB(44,  44,  48);
    constexpr COLORREF kRowHover     = RGB(86,  86,  96);  // lift under the cursor's fan row (≥3:1 vs kCardBg)
    constexpr COLORREF kChipActiveBg = RGB(0xF8, 0x7E, 0x73);
    constexpr COLORREF kTextPrimary  = RGB(220, 220, 220);
    constexpr COLORREF kTextActive   = RGB(38,  20,  16);  // dark warm — legible on coral active chip
    constexpr COLORREF kTextSecond   = RGB(170, 170, 176);
    constexpr COLORREF kTextOnBg     = RGB(20,  24,  32);   // dark — legible on the light button pill
    constexpr COLORREF kButtonBg     = RGB(228, 231, 238);  // light pill — pops in the taskbar gap
    constexpr COLORREF kButtonBorder = RGB(120, 124, 132);

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
