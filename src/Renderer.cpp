#include "Renderer.h"
#include "PaintUtil.h"
#include <algorithm>
#include <string>
#include <vector>

using namespace Paint;

namespace
{
    using namespace Paint;

    void FillRoundedThemed(HDC hdc, const RECT& rc, COLORREF top, COLORREF bottom,
                           COLORREF border, bool gradient, int dpiI)
    {
        // Clamp corner diameter to height to avoid doubled-radius look
        const int d = (std::min)(ScalePx(6, dpiI), static_cast<int>(rc.bottom - rc.top));

        // SaveDC/RestoreDC to preserve caller's existing clip region
        HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom, d, d);
        const int saved = SaveDC(hdc);
        SelectClipRgn(hdc, rgn);
        DeleteObject(rgn);
        if (gradient)
        {
            FillMetallic(hdc, rc, top, bottom);
        }
        else
        {
            HBRUSH br = CreateSolidBrush(top);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
        }
        RestoreDC(hdc, saved);

        HPEN    pen = CreatePen(PS_SOLID, (std::max)(1, ScalePx(1, dpiI)), border);
        HGDIOBJ op  = SelectObject(hdc, pen);
        HGDIOBJ ob  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, d, d);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(pen);
    }
}

namespace Renderer
{
    void DrawButton(HDC hdc, const RECT& rc, const Button& b, int dpiI)
    {
        const Theme& t = ActiveTheme();
        FillRoundedThemed(hdc, rc, t.pillTop, t.pillBottom, t.pillBorder, t.gradient, dpiI);

        const int ptSize = (rc.bottom - rc.top) <= ScalePx(20, dpiI) ? 7 : 8;
        HFONT font = MakeFont(ptSize, FW_MEDIUM, dpiI);
        HGDIOBJ of = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, t.pillText);
        const int tp = ScalePx(3, dpiI);
        RECT txt = { rc.left + tp, rc.top, rc.right - tp, rc.bottom };
        if (txt.right > txt.left)
            DrawTextW(hdc, b.label.c_str(), -1, &txt,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, of);
        DeleteObject(font);
    }

    void DrawChip(HDC hdc, const RECT& rc, const std::wstring& title, int dpiI)
    {
        const Theme& t = ActiveTheme();
        FillRoundedThemed(hdc, rc, t.chipTop, t.chipBottom, t.chipBorder, t.gradient, dpiI);

        HFONT font = MakeFont(9, FW_NORMAL, dpiI);
        HGDIOBJ of = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, t.chipText);
        const int tp = ScalePx(8, dpiI);
        RECT txt = { rc.left + tp, rc.top, rc.right - tp, rc.bottom };
        if (txt.right > txt.left)
            DrawTextW(hdc, title.c_str(), -1, &txt,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, of);
        DeleteObject(font);
    }

    GapLayout GapChipLayout(const RECT& rc, UINT dpi, const Store& store,
                            const std::vector<Button>& buttons)
    {
        GapLayout out;

        const int dpiI  = dpi ? static_cast<int>(dpi) : 96;
        const int edge  = ScalePx(4, dpiI);
        const int gap   = ScalePx(6, dpiI);
        const int chipMax = ScalePx(160, dpiI);
        const int chipMin = ScalePx(60, dpiI);
        const int rowH  = rc.bottom - rc.top;
        const int innerH = rowH - 2 * edge;
        const int left  = rc.left + edge;
        const int right = rc.right - edge;
        const int avail = right - left;
        if (avail <= 0) return out;

        std::vector<HWND> wins;
        const auto& all = store.All();
        for (HWND hwnd : store.Ordered())
        {
            auto it = all.find(hwnd);
            if (it != all.end() && it->second.minimized) wins.push_back(hwnd);
        }
        const int nWin = static_cast<int>(wins.size());

        const bool twoRow = (nWin > 0 && !buttons.empty());

        int pillTop, pillH, chipTop, chipH;
        if (twoRow)
        {
            const int vgap = ScalePx(2, dpiI);
            pillH = ScalePx(14, dpiI);
            // Defensive floor: innerH-pillH-vgap can still go negative with short taskbars
            chipH = (std::max)(1, (std::min)(ScalePx(24, dpiI), innerH - pillH - vgap));
            const int stackH = pillH + vgap + chipH;
            pillTop = rc.top + (rowH - stackH) / 2;
            chipTop = pillTop + pillH + vgap;
        }
        else
        {
            const int h = (std::min)(ScalePx(28, dpiI), innerH);
            if (h < 1) return out;
            pillH = chipH = h;
            pillTop = chipTop = rc.top + (rowH - h) / 2;
        }

        int x = left;
        int k = 0;
        if (nWin > 0)
        {
            int maxK = (avail + gap) / (chipMin + gap);
            if (maxK < 0) maxK = 0;
            k = (std::min)(nWin, maxK);
        }
        if (k > 0)
        {
            int chipW = (avail - (k - 1) * gap) / k;
            if (chipW > chipMax) chipW = chipMax;
            out.chips.reserve(k);
            for (int i = 0; i < k; ++i)
            {
                out.chips.push_back({ { x, chipTop, x + chipW, chipTop + chipH }, wins[i] });
                x += chipW + gap;
            }
        }

        // FolderFan pills evict non-FolderFan pills rather than drop; others drop from right
        const int pillW = ScalePx(71, dpiI);
        int px = twoRow ? left : x;
        for (int i = 0; i < static_cast<int>(buttons.size()); ++i)
        {
            if (px + pillW > right)
            {
                if (buttons[i].action == ButtonAction::FolderFan)
                {
                    auto evict = std::find_if(out.buttons.rbegin(), out.buttons.rend(),
                        [&](const ButtonHit& h) { return buttons[h.index].action != ButtonAction::FolderFan; });
                    if (evict != out.buttons.rend())
                    {
                        px = evict->rect.left;
                        out.buttons.erase(std::next(evict).base());
                        out.buttons.push_back({ { px, pillTop, px + pillW, pillTop + pillH }, i });
                        px += pillW + gap;
                    }
                }
                continue;
            }
            out.buttons.push_back({ { px, pillTop, px + pillW, pillTop + pillH }, i });
            px += pillW + gap;
        }
        return out;
    }
}

