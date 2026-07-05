#pragma once
#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "Launcher.h"
#include "Store.h"
#include "Renderer.h"

// Stage 5b. ALL taskbar-geometry heuristics live in this one file (CLAUDE.md
// hard rule 6): a Windows update that reshuffles the taskbar is a one-file fix.
//
// 5b: measure the empty gap between the last task-button and the widgets/tray
// cluster, size a click-through overlay to it, and host the automation buttons
// (the same Launcher model as 5a's dock strip) there.
//
// Win11's taskbar is XAML — task buttons and the Widgets button have NO HWND, only
// UI Automation elements, so measurement must use UIA to read their bounding rects.
// UIA is blocking cross-process work, so it runs on a WORKER thread (rule 5, like
// TabReader); the worker posts the measured rect back and the UI thread moves the
// overlay. Clicks outside a button pass to the taskbar (WM_NCHITTEST→HTTRANSPARENT).
// No AppBar is registered here → no ABM_REMOVE obligation.
class TaskbarOverlayWindow
{
public:
    TaskbarOverlayWindow() = default;
    ~TaskbarOverlayWindow();
    TaskbarOverlayWindow(const TaskbarOverlayWindow&) = delete;
    TaskbarOverlayWindow& operator=(const TaskbarOverlayWindow&) = delete;

    // dockHwnd/stateMsg: the overlay posts stateMsg (wparam = 1 active / 0 inactive)
    // whenever it starts/stops hosting the buttons, so the dock can hide/show its
    // own fallback strip (5b.3).
    // store: read-only, UI-thread only (like m_launcher) — the measurement worker never
    // touches it. chipClickMsg: posted to dockHwnd (wparam = window HWND) on a chip click.
    // chipHoverMsg: posted to dockHwnd (wparam = hovered window HWND, or 0 on leave) as the
    // hovered chip changes — the dock opens/graces the fan.
    bool Create(HINSTANCE instance, const Launcher* launcher, const Store* store,
                HWND dockHwnd, UINT stateMsg, UINT chipClickMsg, UINT chipHoverMsg);
    void Destroy();

    // UI thread: screen rect of the chip for `hwnd` in the current layout, or false if it
    // isn't currently shown as a chip. The dock anchors the fan above it.
    bool ChipRectScreen(HWND hwnd, RECT* out) const;

    // Ask the worker to re-measure (non-blocking). Safe to call from a timer or
    // ABN_POSCHANGED / WM_DISPLAYCHANGE / WM_DPICHANGED on the UI thread.
    void RequestMeasure();

    // UI thread: chip content (Store) changed. Re-evaluate fit against the last measured
    // gap (chip count can flip shown/hidden without any taskbar geometry change) and
    // repaint — no UIA re-measure.
    void RefreshContent();

    bool Shown() const { return m_shown; }

    // UI thread. Force-hide the overlay regardless of the measured gap (Start/Search
    // flyout open, or a fullscreen app on the taskbar's monitor). While suppressed the
    // overlay never shows. The caller re-measures after clearing suppression.
    void SetSuppressed(bool suppressed);

    HWND Hwnd() const { return m_hwnd; }

private:
    struct Gap { RECT rc; bool valid; };

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC hdc);
    // UI thread: position/show/hide the overlay. allowHysteresis=false (content-only
    // refresh on a known-valid gap) hides immediately instead of the mid-animation
    // 300ms retry, which exists only for UIA zero-rects during a taskbar animation.
    void ApplyGap(const Gap& g, bool allowHysteresis = true);
    void PostState();              // UI thread: tell the dock our host state (deduped)
    int  ButtonAt(POINT ptClient) const;   // UI thread: pill hit-test, or -1
    HWND ChipAt(POINT ptClient) const;     // UI thread: chip hit-test, or nullptr
    void UpdateHover(HWND chip);           // UI thread: post chipHoverMsg on hover change

    const std::vector<Button>& Buttons() const;   // launcher's buttons, or empty
    Renderer::GapLayout ComputeGapLayout() const; // requires m_store

    void WorkerLoop();             // worker thread: owns IUIAutomation
    Gap  MeasureGap(struct IUIAutomation* automation) const;
    static HWND FindTaskbar();     // Shell_TrayWnd verified owned by explorer.exe
    static bool IsAutoHide();

    HWND            m_hwnd     = nullptr;
    bool            m_shown    = false;
    const Launcher* m_launcher = nullptr;
    const Store*    m_store    = nullptr;    // UI-thread read only; worker never touches it
    HWND            m_dockHwnd    = nullptr;
    UINT            m_stateMsg    = 0;
    UINT            m_chipClickMsg = 0;
    UINT            m_chipHoverMsg = 0;
    HWND            m_hoverChip   = nullptr;   // last chip we posted hover for (dedupe)
    bool            m_mouseTracking = false;   // TME_LEAVE armed (whole-overlay leave)
    Gap             m_lastGap     = { {}, false };  // last measured gap, for RefreshContent re-fit
    bool            m_statePosted     = false;
    bool            m_lastPostedShown = false;
    bool            m_suppressed      = false;  // flyout/fullscreen force-hide (UI thread)
    int             m_invalidStreak   = 0;      // consecutive bad measures (hysteresis, UI thread)

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_stop    = false;
    bool                    m_pending = false;
    bool                    m_measurePending = false;  // UI thread: a RequestMeasure is in-flight
};
