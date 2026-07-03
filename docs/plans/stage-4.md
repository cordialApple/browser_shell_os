# Stage 4 plan — variant D: minimized-only side-by-side cards + per-window hover-fan

Spec basis: `docs/ARCHITECTURE.md` §6, **superseded by variant D** (decided
2026-07-03 with the user). Acceptance: §12 row 4 (reinterpreted for D).

## Why variant D (not the original staggered-stack)

User workflow: **2–3 browser windows, 10+ tabs each**. On a full-width
1920px strip, 2–3 side-by-side cards get 640–960px each — window count is
never the pressure. The real pressure is **intra-card**: 10+ tabs won't fit
legibly in one ~26px row. So: keep side-by-side cards, drop all many-window
overflow machinery (YAGNI), and solve tabs-per-window with a transient
per-window **hover-fan** popup. The reserved AppBar strip height never
changes. Keep the card renderer pluggable so the staggered-stack model can
drop in later if window count ever climbs.

## Step 4.1 — Minimized-only cards ✅ (done 2026-07-03)

**Built:** `Renderer::Paint` filters `Store` to minimized windows only and
draws one side-by-side card each; open/visible browser windows are not shown.
Empty → "no minimized browsers". `Store`/`WindowMonitor` already multi-window
(HWND-keyed) from Stage 3.

**Checkpoint:** minimize 2–3 windows → a card each; a non-minimized browser
window shows nothing in the dock. (Runtime verified with user.)

## Step 4.2 — Collapsed card polish: count badge + legible chips

**Build:** header shows window title + right-aligned `N tabs` badge. Content
row: chips with an enforced **legible min width (~120px)**, left-packed, as
many as fit, remainder into a trailing `+N` chip (stop stretching chips to
fill — legibility over count). If the UIA snapshot exposes the selected tab,
show the active tab first and highlight it (see 4.2a).

**Checkpoint:** card with 12 tabs → a few readable chips + `+8`; active tab
first and visually marked; nothing unreadable.

### Step 4.2a — Selected-tab state in TabReader (prereq for highlight)

**Build:** extend `TabReader` snapshot to capture per-tab selected state
(`UIA_SelectionItemIsSelectedPropertyId`, cached). Add `bool active` to `Tab`.
All UIA assumptions stay in `TabReader` (rule 6).

**Checkpoint:** debug dump marks exactly one tab active per window, matching
the browser's actual active tab.

## Step 4.3 — Per-window hover-fan popup

**Build:** `TrackMouseEvent` (TME_LEAVE) over each card's rect → hover-intent
timer (~250ms) → show a transient popup: separate `WS_POPUP`,
`WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED`. Bottom
edge = strip top edge, x aligned to the hovered card, **grows upward**. Lists
**all** that window's tabs, one ~24px row each (full title, active marked).
One reusable popup HWND, repositioned/repopulated per card. Dismiss on
`WM_MOUSELEAVE` / focus loss. The AppBar reservation never changes.

**Checkpoint:** hover a card → fan opens upward over other windows without
stealing focus; lists all 10+ tabs; move away → closes. Maximized Notepad's
size is identical before/during/after (strip height constant).

## Step 4.4 — Click-to-restore + hit testing

**Build:** hit-test card rects (and fan rows) → HWND. Click a card (or its
fan header) → `ShowWindow(SW_RESTORE)` + `SetForegroundWindow`; remove the
card; re-settle. Foreground-permission fallback: `SwitchToThisWindow` / flash.
(Per-tab activation from a fan row is a later upgrade — restores window for
now.)

**Checkpoint:** §12 row 4 (D reinterpretation): 3 windows minimized, click
card #2 → that window restores and focuses; its card disappears; the other
two remain side by side.

## Step 4.5 — Snapshot debounce polish

**Build:** debounce UIA snapshots when many windows minimize at once (Win+M)
so the worker queue doesn't thrash; dock stays responsive.

**Checkpoint:** Win+M with 3 windows → all cards populate within ~1s, dock
responsive, CPU settles to ~0%.

## Definition of done

- [ ] Only minimized windows appear; open windows never shown.
- [ ] 10+ tabs per window reachable via hover-fan; collapsed row stays legible.
- [ ] Reserved strip height constant; fan is a transient popup only.
- [ ] Stage 1–3 acceptance still passes.
- [ ] Staggered-stack model remains a drop-in behind the card-renderer seam
      (documented, not built).
