#pragma once
#include <windows.h>
#include <vector>
#include "Store.h"
#include "Launcher.h"

namespace Renderer
{
    struct CardHit   { RECT rect; HWND hwnd; };
    struct ButtonHit { RECT rect; int  index; };

    void Paint(HDC hdc, const RECT& rc, UINT dpi, const Store& store,
               const std::vector<Button>& buttons);

    // Card rects (dock client coords) for the minimized windows, stacked top→bottom.
    // Shared with hover hit-testing so the layout lives in exactly one place.
    std::vector<CardHit> CardLayout(const RECT& rc, UINT dpi, const Store& store);

    // Automation-button rects (dock client coords), pinned top-right, overlaying the
    // cards. Shared with click hit-testing — single source, same as CardLayout.
    std::vector<ButtonHit> ButtonLayout(const RECT& rc, UINT dpi,
                                        const std::vector<Button>& buttons);
}
