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

    struct GapLayout {
        std::vector<ChipHit>   chips;
        std::vector<ButtonHit> buttons;
    };

    GapLayout GapChipLayout(const RECT& rc, UINT dpi, const Store& store,
                            const std::vector<Button>& buttons);

    void DrawChip(HDC hdc, const RECT& rc, const std::wstring& title, int dpi);

    void DrawButton(HDC hdc, const RECT& rc, const Button& b, int dpi);
}
