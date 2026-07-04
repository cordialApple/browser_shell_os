#pragma once
#include <windows.h>
#include <vector>
#include "Store.h"

namespace Renderer
{
    struct CardHit { RECT rect; HWND hwnd; };

    void Paint(HDC hdc, const RECT& rc, UINT dpi, const Store& store);

    // Card rects (dock client coords) for the minimized windows, left→right.
    // Shared with hover hit-testing so the layout lives in exactly one place.
    std::vector<CardHit> CardLayout(const RECT& rc, UINT dpi, const Store& store);
}
