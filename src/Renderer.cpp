#include "Renderer.h"
#include <string>

namespace Renderer
{
    void Paint(HDC hdc, const RECT& rc, UINT dpi, const Store& store)
    {
        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 30));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        const int dpiI = dpi ? static_cast<int>(dpi) : 96;
        LOGFONTW lf = {};
        lf.lfHeight  = -MulDiv(12, dpiI, 72);
        lf.lfWeight  = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        HFONT font    = CreateFontIndirectW(&lf);
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(220, 220, 220));

        std::wstring label;
        const auto& all = store.All();
        if (all.empty())
        {
            label = L"browser: none";
        }
        else
        {
            const TrackedWindow& first = all.begin()->second;
            if (first.minimized)
            {
                label = first.title + L" — minimized";
            }
            else
            {
                label = L"browser: ";
                label += first.title;
            }
            if (all.size() > 1)
            {
                wchar_t extra[32];
                swprintf_s(extra, L" (+%zu)", all.size() - 1);
                label += extra;
            }
        }

        RECT textRc = rc;
        DrawTextW(hdc, label.c_str(), -1, &textRc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(hdc, oldFont);
        DeleteObject(font);
    }
}
