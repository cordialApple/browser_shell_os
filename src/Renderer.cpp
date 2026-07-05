#include "Renderer.h"
#include "PaintUtil.h"
#include <algorithm>
#include <string>
#include <vector>

using namespace Paint;

namespace Renderer
{
    void DrawButton(HDC hdc, const RECT& rc, const Button& b, int dpiI)
    {
        HBRUSH br  = CreateSolidBrush(kButtonBg);
        HPEN   pen = CreatePen(PS_SOLID, (std::max)(1, ScalePx(1, dpiI)), kButtonBorder);
        HGDIOBJ ob = SelectObject(hdc, br);
        HGDIOBJ op = SelectObject(hdc, pen);
        // RoundRect's last two args are the corner ELLIPSE diameter (2×radius); clamp
        // to the pill height so it reads as a rounded end, not a doubled radius.
        const int d = (std::min)(ScalePx(6, dpiI), static_cast<int>(rc.bottom - rc.top));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, d, d);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(pen);
        DeleteObject(br);

        HFONT font = MakeFont(8, FW_MEDIUM, dpiI);
        HGDIOBJ of = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kTextOnBg);
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
        HBRUSH br  = CreateSolidBrush(kCardBg);
        HPEN   pen = CreatePen(PS_SOLID, (std::max)(1, ScalePx(1, dpiI)), kButtonBorder);
        HGDIOBJ ob = SelectObject(hdc, br);
        HGDIOBJ op = SelectObject(hdc, pen);
        const int d = (std::min)(ScalePx(6, dpiI), static_cast<int>(rc.bottom - rc.top));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, d, d);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(pen);
        DeleteObject(br);

        HFONT font = MakeFont(9, FW_NORMAL, dpiI);
        HGDIOBJ of = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kTextPrimary);
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

        const int dpiI = dpi ? static_cast<int>(dpi) : 96;
        const int edge = ScalePx(4, dpiI);    // dead-zone: never cover the taskbar edges
        const int gap  = ScalePx(6, dpiI);
        const int chipMax = ScalePx(160, dpiI);
        const int chipMin = ScalePx(90, dpiI);
        const int rowH = rc.bottom - rc.top;
        const int h    = (std::min)(ScalePx(28, dpiI), rowH - 2 * edge);
        if (h < 1) return out;

        const int left  = rc.left + edge;
        const int right = rc.right - edge;
        const int top   = rc.top + (rowH - h) / 2;
        const int avail = right - left;
        if (avail <= 0) return out;

        // Chips: minimized windows only, in stable insertion order.
        std::vector<HWND> wins;
        const auto& all = store.All();
        for (HWND hwnd : store.Ordered())
        {
            auto it = all.find(hwnd);
            if (it != all.end() && it->second.minimized) wins.push_back(hwnd);
        }

        int x = left;
        const int nWin = static_cast<int>(wins.size());
        int k = 0;
        if (nWin > 0)
        {
            int maxK = (avail + gap) / (chipMin + gap);   // how many fit at the floor width
            if (maxK < 0) maxK = 0;
            k = (std::min)(nWin, maxK);                    // overflow drops newest (tail)
        }
        if (k > 0)
        {
            int chipW = (avail - (k - 1) * gap) / k;       // spread across the row...
            if (chipW > chipMax) chipW = chipMax;          // ...but cap so pills keep room
            out.chips.reserve(k);
            for (int i = 0; i < k; ++i)
            {
                out.chips.push_back({ { x, top, x + chipW, top + h }, wins[i] });
                x += chipW + gap;
            }
        }

        // Pills fill the leftover to the right of the chips; drop first when tight.
        const int pillW = ScalePx(71, dpiI);
        for (int i = 0; i < static_cast<int>(buttons.size()); ++i)
        {
            if (x + pillW > right) break;
            out.buttons.push_back({ { x, top, x + pillW, top + h }, i });
            x += pillW + gap;
        }
        return out;
    }
}

