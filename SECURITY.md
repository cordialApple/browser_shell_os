# Security & privacy

`Peekbar` sits close to sensitive surfaces. It reads your browser
windows and draws over the taskbar, so here is exactly what it does and does
not do.

## What it reads

- **Tab titles only**, via Windows UI Automation (UIA), the same accessibility
  API screen readers use. It reads the `Name` property of tab controls in
  minimized browser windows.
- **Full URLs are not read.** UIA only exposes the address-bar value of the
  browser's *currently active* tab, and even that is not captured.
  Background-tab URLs are not available through this API at all. Only visible
  tab titles are stored.
- Tab data is snapshotted on minimize and kept in memory for as long as the
  window stays minimized. It is never written to disk.

## What it never does

- **Never injects code into `explorer.exe` or any browser process.** The dock
  is a separate top-level window drawn over the taskbar's empty space. Not a
  hook, not a DLL injection, not a shell extension.
- **Never sends anything over the network.** There are no network calls in the
  shell binary: no telemetry endpoint, no update checker, no analytics.
- **Never writes log files.** Diagnostics use Windows ETW (TraceLogging)
  events only, which live in the OS trace buffer and require a separate,
  explicitly-launched consumer (`shell_profiler`, a distinct executable never
  bundled with the shell) or a tool like Windows Performance Recorder to
  view. Nothing is captured unless you deliberately start a trace session.

## Automation buttons

The taskbar automation buttons (Stage 5) only do what you configure them to
do: open a URL you specify or launch a shortcut you point at, via the standard
Windows `ShellExecuteW` "open" verb. That is the same as double-clicking a link
or `.lnk` file yourself. No URLs or shortcuts ship preconfigured.

## Source

This is source-available under the MIT license. Read `src/` yourself rather
than taking the above on faith. [`docs/`](docs/) has the
technical design if you want the "why," not just the "what."

## Reporting a vulnerability

Open a GitHub issue, or if it's sensitive, contact the maintainer directly
(see the GitHub profile for contact info). This is a hobby project without a
dedicated security team, so response time is best-effort.
