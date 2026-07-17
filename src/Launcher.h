#pragma once
#include <windows.h>
#include <map>
#include <string>
#include <vector>

enum class ButtonStyle  { Pill, Icon };
enum class ButtonAction { Url, Shortcut, Command, FolderFan };

struct Button {
    ButtonStyle  style  = ButtonStyle::Pill;
    std::wstring label;
    std::wstring iconPath;
    ButtonAction action = ButtonAction::Url;
    std::wstring target;
    // FolderFan only: empty until worker-thread scan completes (Load() never blocks on filesystem)
    std::vector<std::wstring> folderEntries;
};

// Owns the automation-button config (CLAUDE.md: Launcher component, Stage 5).
// Minimum-functionality config is a line-based text file (hard rule 2 — no JSON
// dependency); one button per line: style|label|action|target|iconPath(optional).
// Blank lines and lines starting with '#' or ';' are ignored. Malformed lines are
// skipped with an OutputDebugStringW note; a missing file yields zero buttons.
class Launcher
{
public:
    void Load();
    const std::vector<Button>& Buttons() const { return m_buttons; }

    // From optional `theme=<name>` config line; feed to Paint::SetActiveTheme (unknown → "slate")
    const std::wstring& ThemeName() const { return m_themeName; }

    // Fire-and-forget: launches on a detached MTA worker so a slow shell handler
    // never blocks the dock's UI pump (CLAUDE.md rule 5).
    void Execute(const Button& b) const;

    // Fire-and-forget: launches `wt.exe -d "<folderPath>" cmd /k claude` the same way
    // (detached MTA worker). folderPath is the fan-picked subfolder's full path.
    void LaunchFolder(const std::wstring& folderPath) const;

    // Roots (from this Load()'s FolderFan buttons) not yet in the cache — the host must
    // scan each on a worker thread and report back via ApplyFolderScan. Empty once every
    // FolderFan root has a cached result.
    const std::vector<std::wstring>& PendingFolderScans() const { return m_pendingFolderScans; }

    // De-duplicated target roots of every current FolderFan button — the host watches
    // each for changes (one directory watcher per distinct root).
    std::vector<std::wstring> FolderFanRoots() const;

    // UI thread: a worker-thread scan of `root` completed with `entries`. Caches the
    // result (so a later Load() of the same root skips rescanning) and fills in
    // folderEntries on every current FolderFan button targeting `root`.
    void ApplyFolderScan(const std::wstring& root, std::vector<std::wstring> entries);

    // Immediate subdirectory names of `root` (skips files and "."/".."), sorted
    // case-insensitively. Empty on a missing/inaccessible root. Pure function of `root` —
    // safe to call from any thread (the host runs this on a worker, never on the UI thread).
    static std::vector<std::wstring> ScanImmediateSubfolders(const std::wstring& root);

    static std::wstring ConfigDir();       // %LOCALAPPDATA%\Peekbar
    static std::wstring ConfigFileName();  // config.txt
    static std::wstring ConfigPath();      // ConfigDir()\ConfigFileName()

private:
    std::vector<Button> m_buttons;
    std::wstring        m_themeName;
    // FolderFan cache: reuse cached entries, queue uncached roots in m_pendingFolderScans
    std::map<std::wstring, std::vector<std::wstring>> m_folderFanCache;
    std::vector<std::wstring>                         m_pendingFolderScans;
};
