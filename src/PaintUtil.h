#pragma once
#include <windows.h>

namespace Paint
{
    constexpr COLORREF kBgColor      = RGB(28,  28,  30);
    constexpr COLORREF kCardBg       = RGB(44,  44,  48);
    constexpr COLORREF kChipBg       = RGB(60,  60,  66);
    constexpr COLORREF kChipActiveBg = RGB(38,  79,  120);
    constexpr COLORREF kTextPrimary  = RGB(220, 220, 220);
    constexpr COLORREF kTextActive   = RGB(240, 244, 250);
    constexpr COLORREF kTextSecond   = RGB(170, 170, 176);

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
