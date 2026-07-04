#pragma once
#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>

// Stage 5b. ALL taskbar-geometry heuristics live in this one file (CLAUDE.md
// hard rule 6): a Windows update that reshuffles the taskbar is a one-file fix.
//
// 5b.1 scope: measure the empty gap between the last task-button and the widgets/
// tray cluster, and draw a click-through debug outline over it.
//
// Win11's taskbar is XAML — task buttons and the Widgets button have NO HWND, only
// UI Automation elements, so measurement must use UIA to read their bounding rects.
// UIA is blocking cross-process work, so it runs on a WORKER thread (rule 5, like
// TabReader); the worker posts the measured rect back and the UI thread moves the
// outline window. No AppBar is registered here → no ABM_REMOVE obligation.
class TaskbarOverlayWindow
{
public:
    TaskbarOverlayWindow() = default;
    ~TaskbarOverlayWindow();
    TaskbarOverlayWindow(const TaskbarOverlayWindow&) = delete;
    TaskbarOverlayWindow& operator=(const TaskbarOverlayWindow&) = delete;

    bool Create(HINSTANCE instance);
    void Destroy();

    // Ask the worker to re-measure (non-blocking). Safe to call from a timer or
    // ABN_POSCHANGED / WM_DISPLAYCHANGE / WM_DPICHANGED on the UI thread.
    void RequestMeasure();

    HWND Hwnd() const { return m_hwnd; }

private:
    struct Gap { RECT rc; bool valid; };

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC hdc);
    void ApplyGap(const Gap& g);   // UI thread: position/show/hide the outline

    void WorkerLoop();             // worker thread: owns IUIAutomation
    Gap  MeasureGap(struct IUIAutomation* automation) const;
    static HWND FindTaskbar();     // Shell_TrayWnd verified owned by explorer.exe
    static bool IsAutoHide();

    HWND m_hwnd  = nullptr;
    bool m_shown = false;

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_stop    = false;
    bool                    m_pending = false;
};
