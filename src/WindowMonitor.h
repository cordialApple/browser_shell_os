#pragma once
#include <windows.h>
#include <vector>

bool IsBrowserFrame(HWND hwnd);

std::vector<HWND> ScanBrowserFrames();
