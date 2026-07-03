#include "Renderer.h"
#include <string>

namespace Renderer
{
    void Paint(HDC hdc, const RECT& rc, UINT dpi, const std::vector<HWND>& browsers)
    {
        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 30));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        // 12pt Segoe UI, DPI-scaled
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
        if (browsers.empty())
        {
            label = L"browser: none";
        }
        else
        {
            wchar_t title[256] = {};
            GetWindowTextW(browsers[0], title, _countof(title));
            label = L"browser: ";
            label += title;
            if (browsers.size() > 1)
            {
                wchar_t extra[32];
                swprintf_s(extra, L" (+%zu)", browsers.size() - 1);
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
