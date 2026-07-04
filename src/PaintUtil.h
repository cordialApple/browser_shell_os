#pragma once
#include <windows.h>

namespace Paint
{
    constexpr COLORREF kBgColor      = RGB(0x00, 0xA2, 0xED);
    constexpr COLORREF kCardBg       = RGB(44,  44,  48);
    constexpr COLORREF kChipBg       = RGB(60,  60,  66);
    constexpr COLORREF kChipActiveBg = RGB(0xF8, 0x7E, 0x73);
    constexpr COLORREF kTextPrimary  = RGB(220, 220, 220);
    constexpr COLORREF kTextActive   = RGB(38,  20,  16);  // dark warm — legible on coral active chip
    constexpr COLORREF kTextSecond   = RGB(170, 170, 176);
    constexpr COLORREF kTextOnBg     = RGB(20,  24,  32);   // dark — legible on kBgColor blue
    constexpr COLORREF kButtonBg     = RGB(228, 231, 238);  // light pill — pops over dark cards + blue
    constexpr COLORREF kButtonBorder = RGB(120, 124, 132);

    // Vertical window stack: one legible band per minimized window; the dock's
    // reserved height grows with the window count up to kMaxBands (extras beyond
    // that are not laid out — deferred, matches the many-window overflow debt).
    constexpr int kBandHeightDip = 34;
    constexpr int kBandPadDip    = 4;   // must match CardLayout's pad
    constexpr int kMaxBands      = 4;

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
