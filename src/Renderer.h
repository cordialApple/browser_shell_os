#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Store.h"
#include "Launcher.h"

namespace Renderer
{
    struct ButtonHit { RECT rect; int  index; };
    struct ChipHit   { RECT rect; HWND hwnd; };

    // One gap layout: chips (minimized-window active tabs) laid out first, then the
    // automation pills filling whatever gap space remains. Single source shared by the
    // overlay's paint + hover/click hit-testing.
    struct GapLayout {
        std::vector<ChipHit>   chips;
        std::vector<ButtonHit> buttons;
    };

    // Taskbar-gap layout for the chip model: chips for minimized windows (in
    // Store::Ordered order) laid out left-anchored first, then automation pills filling
    // the leftover and dropping first when the gap is tight. A 4px dead-zone is left
    // inside each edge so the overlay never covers the taskbar's own hit regions.
    // Single source shared by the overlay's paint + hit-testing.
    GapLayout GapChipLayout(const RECT& rc, UINT dpi, const Store& store,
                            const std::vector<Button>& buttons);

    // Draw one chip pill (a minimized window's active-tab title, ellipsized).
    void DrawChip(HDC hdc, const RECT& rc, const std::wstring& title, int dpi);

    // Draw one automation-button pill. Shared by the dock strip and the gap overlay.
    void DrawButton(HDC hdc, const RECT& rc, const Button& b, int dpi);
}
