#include "Renderer.h"
#include "PaintUtil.h"
#include <string>
#include <vector>

using namespace Paint;

namespace
{
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

        const int hdrTop  = cardRc.top + pad;
        const int hdrBot  = cardRc.top + cardH / 2;
        const int hdrLeft = cardRc.left + pad;
        const int hdrRight = cardRc.right - pad;

        // "N tabs" badge, right-aligned; title centered in a symmetric remainder.
        // Only show the badge if it fits symmetrically beside a minimum title width,
        // otherwise the title rect would invert and the title would vanish.
        wchar_t badge[24];
        swprintf_s(badge, L"%d tab%s",
                   static_cast<int>(win.tabs.size()),
                   win.tabs.size() == 1 ? L"" : L"s");
        SIZE badgeSz = {};
        GetTextExtentPoint32W(hdc, badge, static_cast<int>(wcslen(badge)), &badgeSz);
        const int badgeCol = badgeSz.cx + ScalePx(4, dpiI);  // pad so badge never touches title
        const int minTitle = ScalePx(30, dpiI);

        RECT titleRc = { hdrLeft, hdrTop, hdrRight, hdrBot };
        if (2 * badgeCol + minTitle <= hdrRight - hdrLeft)
        {
            RECT badgeRc = { hdrRight - badgeSz.cx, hdrTop, hdrRight, hdrBot };
            SetTextColor(hdc, kTextSecond);
            DrawTextW(hdc, badge, -1, &badgeRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            titleRc.left  = hdrLeft + badgeCol;
            titleRc.right = hdrRight - badgeCol;
        }
        if (titleRc.right > titleRc.left)
        {
            SetTextColor(hdc, kTextPrimary);
            DrawTextW(hdc, win.title.c_str(), -1, &titleRc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        SelectObject(hdc, tabFont);

        // Tab row: one truncated chip per tab, like the browser tab strip.
        const RECT rowRc = { cardRc.left + pad, cardRc.top + cardH / 2 + ScalePx(2, dpiI),
                             cardRc.right - pad, cardRc.bottom - pad };
        const int rowW = rowRc.right - rowRc.left;
        const int n    = static_cast<int>(win.tabs.size());

        const int gap      = ScalePx(4, dpiI);
        const int minChipW = ScalePx(110, dpiI);  // legible width; +N absorbs the rest
        const int chipPad  = ScalePx(6, dpiI);

        // Active tab first so it's never buried in the "+N" overflow.
        std::vector<int> order;
        order.reserve(n);
        int activeIdx = -1;
        for (int i = 0; i < n; ++i)
            if (win.tabs[i].active) { activeIdx = i; break; }
        if (activeIdx >= 0) order.push_back(activeIdx);
        for (int i = 0; i < n; ++i)
            if (i != activeIdx) order.push_back(i);

        if (n > 0 && rowW > gap)
        {
            // Fit as many chips as possible at min width; reserve a "+N" slot only
            // if some overflow AND a chip still fits beside it. Then stretch the
            // visible chips to fill the full row.
            int trailerW = 0;
            int fit = (rowW + gap) / (minChipW + gap);
            if (fit < 1) fit = 1;
            int visible = n <= fit ? n : fit;

            if (visible < n)
            {
                const int tw = ScalePx(44, dpiI) + gap;
                const int fitWithTrailer = (rowW - tw + gap) / (minChipW + gap);
                if (fitWithTrailer >= 1)
                {
                    trailerW = tw;
                    visible = fitWithTrailer < n ? fitWithTrailer : n;
                }
            }

            const int chipArea = rowW - trailerW;
            const int chipW = (chipArea - (visible - 1) * gap) / visible;

            int x = rowRc.left;
            for (int i = 0; i < visible; ++i)
            {
                const Tab& tab = win.tabs[order[i]];
                const int right = (i == visible - 1) ? rowRc.left + chipArea : x + chipW;
                RECT chip = { x, rowRc.top, right, rowRc.bottom };
                HBRUSH chipBrush = CreateSolidBrush(tab.active ? kChipActiveBg : kChipBg);
                FillRect(hdc, &chip, chipBrush);
                DeleteObject(chipBrush);

                RECT txt = { chip.left + chipPad, chip.top,
                             chip.right - chipPad, chip.bottom };
                SetTextColor(hdc, tab.active ? kTextActive : kTextPrimary);
                DrawTextW(hdc, tab.title.c_str(), -1, &txt,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                x += chipW + gap;
            }

            const int hidden = n - visible;
            if (hidden > 0 && trailerW > 0)
            {
                wchar_t buf[16];
                swprintf_s(buf, L"+%d", hidden);
                RECT tr = { rowRc.left + chipArea + gap, rowRc.top, rowRc.right, rowRc.bottom };
                SetTextColor(hdc, kTextSecond);
                DrawTextW(hdc, buf, -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }

        SelectObject(hdc, old);
        DeleteObject(titleFont);
        DeleteObject(tabFont);
    }
}

namespace Renderer
{
    std::vector<CardHit> CardLayout(const RECT& rc, UINT dpi, const Store& store)
    {
        const int dpiI = dpi ? static_cast<int>(dpi) : 96;

        // The dock surfaces what you can't already see: only minimized windows.
        std::vector<HWND> mins;
        for (const auto& [hwnd, w] : store.All())
            if (w.minimized) mins.push_back(hwnd);

        std::vector<CardHit> cards;
        const int n = static_cast<int>(mins.size());
        if (n == 0) return cards;

        const int pad   = ScalePx(4, dpiI);
        const int rcW   = rc.right - rc.left;
        const int cardW = (rcW - pad * (n + 1)) / n;
        int x           = rc.left + pad;

        cards.reserve(n);
        for (HWND h : mins)
        {
            cards.push_back({ { x, rc.top + pad, x + cardW, rc.bottom - pad }, h });
            x += cardW + pad;
        }
        return cards;
    }

    void Paint(HDC hdc, const RECT& rc, UINT dpi, const Store& store)
    {
        HBRUSH bg = CreateSolidBrush(kBgColor);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        const int dpiI = dpi ? static_cast<int>(dpi) : 96;

        const auto cards = CardLayout(rc, dpi, store);
        if (cards.empty())
        {
            HFONT font    = MakeFont(12, FW_NORMAL, dpiI);
            HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, kTextSecond);
            RECT textRc = rc;
            DrawTextW(hdc, L"no minimized browsers", -1, &textRc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFont);
            DeleteObject(font);
            return;
        }

        const auto& all = store.All();
        for (const CardHit& c : cards)
        {
            auto it = all.find(c.hwnd);
            if (it != all.end())
                DrawCard(hdc, c.rect, it->second, dpiI);
        }
    }
}

