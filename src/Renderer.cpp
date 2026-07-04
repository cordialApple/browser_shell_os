#include "Renderer.h"
#include "PaintUtil.h"
#include <algorithm>
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

        HFONT tabFont = MakeFont(9, FW_NORMAL, dpiI);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, tabFont));
        SetBkMode(hdc, TRANSPARENT);

        // No title/count header: the window title just echoes the active tab, so it
        // carried no info. The whole card height goes to legible tab chips (active
        // chip first + highlighted); the freed strip space can stack another window.
        const RECT rowRc = { cardRc.left + pad, cardRc.top + pad,
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
                if (txt.right > txt.left)
                {
                    SetTextColor(hdc, tab.active ? kTextActive : kTextPrimary);
                    DrawTextW(hdc, tab.title.c_str(), -1, &txt,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
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
        DeleteObject(tabFont);
    }

    void DrawButton(HDC hdc, const RECT& rc, const Button& b, int dpiI)
    {
        HBRUSH br  = CreateSolidBrush(kButtonBg);
        HPEN   pen = CreatePen(PS_SOLID, (std::max)(1, ScalePx(1, dpiI)), kButtonBorder);
        HGDIOBJ ob = SelectObject(hdc, br);
        HGDIOBJ op = SelectObject(hdc, pen);
        const int r = (std::min)(ScalePx(6, dpiI), static_cast<int>(rc.bottom - rc.top) / 2);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, r, r);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(pen);
        DeleteObject(br);

        HFONT font = MakeFont(9, FW_MEDIUM, dpiI);
        HGDIOBJ of = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kTextOnBg);
        const int tp = ScalePx(6, dpiI);
        RECT txt = { rc.left + tp, rc.top, rc.right - tp, rc.bottom };
        if (txt.right > txt.left)
            DrawTextW(hdc, b.label.c_str(), -1, &txt,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, of);
        DeleteObject(font);
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
        const int total = static_cast<int>(mins.size());
        if (total == 0) return cards;

        // Stack windows vertically: each is a full-width band, one layer of chips.
        // More windows add rows, not taller chips. The dock height is sized to fit
        // `n` bands (DockWindow::DockHeightPx), so cardH lands on ~kBandHeightDip.
        const int n     = (std::min)(total, Paint::kMaxBands);
        const int pad   = ScalePx(Paint::kBandPadDip, dpiI);
        const int rcH   = rc.bottom - rc.top;
        const int cardH = (rcH - pad * (n + 1)) / n;
        if (cardH < 1) return cards;

        int y = rc.top + pad;
        cards.reserve(n);
        for (int i = 0; i < n; ++i)
        {
            cards.push_back({ { rc.left + pad, y, rc.right - pad, y + cardH }, mins[i] });
            y += cardH + pad;
        }
        return cards;
    }

    std::vector<ButtonHit> ButtonLayout(const RECT& rc, UINT dpi,
                                        const std::vector<Button>& buttons)
    {
        std::vector<ButtonHit> hits;
        const int n = static_cast<int>(buttons.size());
        if (n == 0) return hits;

        const int dpiI  = dpi ? static_cast<int>(dpi) : 96;
        const int pad   = ScalePx(4, dpiI);
        const int gap   = ScalePx(4, dpiI);
        const int pillW = ScalePx(84, dpiI);
        const int pillH = (std::min)(ScalePx(22, dpiI), static_cast<int>(rc.bottom - rc.top) - 2 * pad);
        if (pillH < 1) return hits;

        // As many as fit in the strip width; right-anchored group in the top corner.
        int fit = (rc.right - rc.left - 2 * pad + gap) / (pillW + gap);
        if (fit < 0) fit = 0;
        const int show = (std::min)(n, fit);
        if (show == 0) return hits;

        const int top = rc.top + pad;
        int x = rc.right - pad - (show * pillW + (show - 1) * gap);
        hits.reserve(show);
        for (int i = 0; i < show; ++i)
        {
            hits.push_back({ { x, top, x + pillW, top + pillH }, i });
            x += pillW + gap;
        }
        return hits;
    }

    void Paint(HDC hdc, const RECT& rc, UINT dpi, const Store& store,
               const std::vector<Button>& buttons)
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
            SetTextColor(hdc, kTextOnBg);
            RECT textRc = rc;
            DrawTextW(hdc, L"no minimized browsers", -1, &textRc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFont);
            DeleteObject(font);
        }
        else
        {
            const auto& all = store.All();
            for (const CardHit& c : cards)
            {
                auto it = all.find(c.hwnd);
                if (it != all.end())
                    DrawCard(hdc, c.rect, it->second, dpiI);
            }
        }

        // Buttons overlay the cards (drawn last), pinned top-right.
        for (const ButtonHit& h : ButtonLayout(rc, dpi, buttons))
            DrawButton(hdc, h.rect, buttons[h.index], dpiI);
    }
}

