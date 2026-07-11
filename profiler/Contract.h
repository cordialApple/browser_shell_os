#pragma once

// ETW contract with the shell — the ONLY coupling between the two programs (hard rule 8).
// Duplicated here independently; no shell header included. Source of truth: docs/ARCHITECTURE.md §10.
//
// Provider name: BrowserShellOs.Perf
// GUID is DERIVED from this name at runtime (TraceLogging/EventSource convention, see
// ProviderGuidFromName in EtwSession.cpp) so a hardcoded GUID never drifts from the name.
//
// Events (self-describing TraceLogging; decoded via TDH, no manifest):
//   Paint             duration_us, dirty_w, dirty_h (TaskbarOverlayWindow::Paint;
//                     AppBarNegotiate dropped — chip-rework killed the AppBar dock)
//   WinEventCallback  event id, hwnd
//   UiaSnapshot       duration_us, hwnd, tab_count, hr
//   StoreUpdate       tracked_windows, total_tabs
//   LauncherAction    action type, duration_us, hr
//   FanActivateLatency  outcome, us_click_to_restore, us_restore_to_tabfound,
//                       us_tabfound_to_select, us_select_to_confirm, duration_us,
//                       us_gate1_wait, gate1_attempts, us_gate2_wait, gate2_attempts,
//                       us_first_walk, us_last_walk, us_element_from_handle,
//                       us_findall_tabctrls, us_is_inside_document, us_findall_tabitems,
//                       tabctrl_candidates, guided_descent_used
//                       (click -> tab-visible latency chain; duration_us is click to
//                       activation-confirmed, a proxy for first frame, not a true paint
//                       signal. gate1 = window-visible wait, gate2 = UIA tab-tree-walkable
//                       wait; first/last_walk = first vs latest FindLiveTabItems call in
//                       gate 2. element_from_handle/findall_tabctrls/is_inside_document/
//                       findall_tabitems/tabctrl_candidates break down that winning
//                       FindLiveTabItems call; findall_tabctrls uses guided TreeScope_Children
//                       descent pruning Document/web-content subtrees, falling back to blanket
//                       TreeScope_Descendants FindAll if guided descent finds nothing;
//                       is_inside_document is a cheap redundant safety re-check even for
//                       guided candidates. guided_descent_used: 1 if guided descent supplied
//                       candidates, 0 if it fell back to blanket search.
//                       FIELD ORDER IS APPEND-ONLY — TDH decodes positionally; never reorder
//                       or insert ahead of existing fields.)

namespace contract {

inline constexpr wchar_t kProviderName[] = L"BrowserShellOs.Perf";

// Field name the profiler treats as a latency measurement to aggregate.
inline constexpr wchar_t kDurationField[] = L"duration_us";

}  // namespace contract
