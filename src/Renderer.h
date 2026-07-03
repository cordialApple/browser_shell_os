#pragma once
#include <windows.h>
#include <vector>

namespace Renderer
{
    // Paint dock surface. Call from BeginPaint/EndPaint block.
    void Paint(HDC hdc, const RECT& rc, UINT dpi, const std::vector<HWND>& browsers);
}
