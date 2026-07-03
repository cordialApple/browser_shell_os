#pragma once
#include <windows.h>
#include <vector>

// All browser-window heuristics live here only (CLAUDE.md rule 6).
// IsBrowserFrame: pure, no state. ScanBrowserFrames: calls OpenProcess +
// QueryFullProcessImageNameW per window — blocking. Call only pre-loop on
// the UI thread, or from a worker once the message loop is running.
bool IsBrowserFrame(HWND hwnd);

// Synchronous scan of all top-level windows via EnumWindows.
std::vector<HWND> ScanBrowserFrames();
