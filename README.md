# browser_shell_os

Windows shell-layer experiment. Goal: when browser windows minimize, they no
longer vanish to the taskbar — instead they collapse into a **dock strip above
the taskbar** as layered, interactive "session stacks."

## The idea

- Persistent always-on-top strip sits just above the Windows taskbar.
- Minimized browser windows become stacked cards (not flat taskbar buttons).
- Multiple windows group into "session clusters" (Work / Videos / Research).
- Hover a stack → cards slide up like a deck; click → window comes forward.
- Uses open taskbar real estate + the space directly above the toolbar.

Think: macOS Mission Control + Windows taskbar + cross-window tab manager,
merged into one layer.

## Status

Planning stage. Architecture + milestones to come.
