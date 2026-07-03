#include "Renderer.h"
#include <string>

namespace
{
    constexpr COLORREF kBgColor      = RGB(28,  28,  30);
    constexpr COLORREF kCardBg       = RGB(44,  44,  48);
    constexpr COLORREF kTextPrimary  = RGB(220, 220, 220);
    constexpr COLORREF kTextSecond   = RGB(160, 160, 165);

    int ScalePx(int px, int dpiI) { return MulDiv(px, dpiI, 96); }

    HFONT MakeFont(int ptSize, int weight, int dpiI)
    {
        LOGFONTW lf      = {};
        lf.lfHeight      = -MulDiv(ptSize, dpiI, 72);
        lf.lfWeight      = weight;
        lf.lfCharSet     = DEFAULT_CHARSET;
        lf.lfQuality     = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        return CreateFontIndirectW(&lf);
    }

    void DrawCard(HDC hdc, const RECT& cardRc, const TrackedWindow& win, int dpiI)
    {
        HBRUSH cardBrush = CreateSolidBrush(kCardBg);
        FillRect(hdc, &cardRc, cardBrush);
        DeleteObject(cardBrush);

        const int pad = ScalePx(6, dpiI);
        const int cardH = cardRc.bottom - cardRc.top;

        HFONT titleFont = MakeFont(10, FW_SEMIBOLD, dpiI);
        HFONT tabFont   = MakeFont(9,  FW_NORMAL,   dpiI);

        HFONT old = static_cast<HFONT>(SelectObject(hdc, titleFont));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kTextPrimary);

        RECT titleRc = { cardRc.left + pad, cardRc.top + pad,
                         cardRc.right - pad, cardRc.top + cardH / 2 };
        DrawTextW(hdc, win.title.c_str(), -1, &titleRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(hdc, tabFont);
        SetTextColor(hdc, kTextSecond);

        std::wstring tabLine;
        for (size_t i = 0; i < win.tabs.size(); ++i)
        {
            if (i > 0) tabLine += L"  ·  ";
            tabLine += win.tabs[i].title;
        }

        RECT tabRc = { cardRc.left + pad, cardRc.top + cardH / 2,
                       cardRc.right - pad, cardRc.bottom - pad };
        DrawTextW(hdc, tabLine.c_str(), -1, &tabRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(hdc, old);
        DeleteObject(titleFont);
        DeleteObject(tabFont);
    }
}

namespace Renderer
{
    void Paint(HDC hdc, const RECT& rc, UINT dpi, const Store& store)
    {
        HBRUSH bg = CreateSolidBrush(kBgColor);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        const int dpiI = dpi ? static_cast<int>(dpi) : 96;
        const auto& all = store.All();

        if (all.empty())
        {
            HFONT font    = MakeFont(12, FW_NORMAL, dpiI);
            HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, kTextPrimary);
            RECT textRc = rc;
            DrawTextW(hdc, L"browser: none", -1, &textRc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFont);
            DeleteObject(font);
            return;
        }

        const int pad = ScalePx(4, dpiI);
        const int rcW = rc.right - rc.left;

        int nonMinCount = 0;
        for (const auto& [hwnd, w] : all)
            if (!w.minimized) ++nonMinCount;

        if (nonMinCount > 0)
        {
            const TrackedWindow* first = nullptr;
            for (const auto& [hwnd, w] : all)
                if (!w.minimized) { first = &w; break; }

            std::wstring label = L"browser: " + first->title;
            if (all.size() > 1)
            {
                wchar_t extra[32];
                swprintf_s(extra, L" (+%zu)", all.size() - 1);
                label += extra;
            }

            HFONT font    = MakeFont(12, FW_NORMAL, dpiI);
            HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, kTextPrimary);
            RECT textRc = rc;
            DrawTextW(hdc, label.c_str(), -1, &textRc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, oldFont);
            DeleteObject(font);
            return;
        }

        int minCount = static_cast<int>(all.size());
        int cardW    = (rcW - pad * (minCount + 1)) / minCount;
        int x        = rc.left + pad;

        for (const auto& [hwnd, win] : all)
        {
            RECT cardRc = { x, rc.top + pad, x + cardW, rc.bottom - pad };
            DrawCard(hdc, cardRc, win, dpiI);
            x += cardW + pad;
        }
    }
}
