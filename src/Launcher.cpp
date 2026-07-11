#include "Launcher.h"
#include "Trace.h"
#include <shlobj.h>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <thread>

namespace
{
    void DebugPrintf(const wchar_t* fmt, ...)
    {
        wchar_t buf[512];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);  // _TRUNCATE prevents overflow fatality
        va_end(args);
        OutputDebugStringW(buf);
    }

    void DebugSkip(size_t lineNo, const wchar_t* why, const std::wstring& line)
    {
        DebugPrintf(L"[Launcher] skip line %zu (%s): %s\n", lineNo, why, line.c_str());
    }

    std::wstring Trim(const std::wstring& s)
    {
        const wchar_t* ws = L" \t\r\n";
        const size_t b = s.find_first_not_of(ws);
        if (b == std::wstring::npos) return L"";
        const size_t e = s.find_last_not_of(ws);
        return s.substr(b, e - b + 1);
    }

    // Decode to UTF-16, honoring UTF-16LE or UTF-8 BOM (no BOM → UTF-8 default)
    bool ReadTextFile(const std::wstring& path, std::wstring& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty()) { out.clear(); return true; }

        const auto u = [&](size_t i) { return static_cast<unsigned char>(bytes[i]); };
        if (bytes.size() >= 2 && u(0) == 0xFF && u(1) == 0xFE)
        {
            const size_t n = (bytes.size() - 2) / sizeof(wchar_t);
            out.resize(n);
            std::memcpy(out.data(), bytes.data() + 2, n * sizeof(wchar_t));  // avoid misaligned wchar_t read
            return true;
        }
        size_t off = (bytes.size() >= 3 && u(0) == 0xEF && u(1) == 0xBB && u(2) == 0xBF) ? 3 : 0;
        const int len = static_cast<int>(bytes.size() - off);
        if (len <= 0) { out.clear(); return true; }
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + off, len, nullptr, 0);
        if (wlen <= 0) { out.clear(); return false; }
        out.assign(wlen, L'\0');
        if (MultiByteToWideChar(CP_UTF8, 0, bytes.data() + off, len, out.data(), wlen) == 0)
        {
            out.clear();
            return false;
        }
        return true;
    }

    bool ParseStyle(const std::wstring& s, ButtonStyle& out)
    {
        if (s == L"pill") { out = ButtonStyle::Pill; return true; }
        if (s == L"icon") { out = ButtonStyle::Icon; return true; }
        return false;
    }

    bool ParseAction(const std::wstring& s, ButtonAction& out)
    {
        if (s == L"url")       { out = ButtonAction::Url;       return true; }
        if (s == L"shortcut")  { out = ButtonAction::Shortcut;  return true; }
        if (s == L"command")   { out = ButtonAction::Command;   return true; }
        if (s == L"folderfan") { out = ButtonAction::FolderFan; return true; }
        return false;
    }

    const wchar_t* ActionName(ButtonAction a)
    {
        switch (a)
        {
        case ButtonAction::Url:       return L"url";
        case ButtonAction::Shortcut:  return L"shortcut";
        case ButtonAction::Command:   return L"command";
        case ButtonAction::FolderFan: return L"folderfan";
        }
        return L"unknown";
    }

    // Fire-and-forget on MTA worker (not STA: no pump prevents DDE-style handler hang); balance CoUninitialize only on successful init
    void RunCommandWorker(std::wstring cmdLine, const wchar_t* traceAction)
    {
        std::thread([cmdLine = std::move(cmdLine), traceAction]() mutable {
            const long long tStartUs = trace::NowUs();
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HRESULT actionHr = S_OK;
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                               0, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            else
            {
                actionHr = HRESULT_FROM_WIN32(GetLastError());
            }
            if (SUCCEEDED(hr)) CoUninitialize();
            TRACE_EVENT("LauncherAction",
                TraceLoggingWideString(traceAction, "action"),
                TraceLoggingInt64(trace::NowUs() - tStartUs, "duration_us"),
                TraceLoggingInt32(actionHr, "hr"));
        }).detach();
    }

}

// static
std::vector<std::wstring> Launcher::ScanImmediateSubfolders(const std::wstring& root)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = root + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (std::wcscmp(fd.cFileName, L".") == 0 || std::wcscmp(fd.cFileName, L"..") == 0) continue;
        out.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    return out;
}

std::wstring Launcher::ConfigDir()
{
    PWSTR local = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
    std::wstring dir;
    if (SUCCEEDED(hr) && local) dir = local;
    CoTaskMemFree(local);  // no-op on null; safe on every path
    if (dir.empty()) return L"";
    dir += L"\\Peekbar";
    return dir;
}

std::wstring Launcher::ConfigFileName()
{
    return L"config.txt";
}

std::wstring Launcher::ConfigPath()
{
    const std::wstring dir = ConfigDir();
    if (dir.empty()) return L"";
    return dir + L"\\" + ConfigFileName();
}

// NOTE: the host owns theme activation — after Load() it should call
// Paint::SetActiveTheme(ThemeName()) (single line, added post-merge by the host workstream).
void Launcher::Load()
{
    m_buttons.clear();
    m_themeName.clear();
    m_pendingFolderScans.clear();

    const std::wstring path = ConfigPath();
    std::wstring text;
    if (path.empty() || !ReadTextFile(path, text))
    {
        OutputDebugStringW(L"[Launcher] no config file — zero buttons\n");
        return;
    }

    size_t lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size())
    {
        const size_t nl = text.find(L'\n', pos);
        const std::wstring raw = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        ++lineNo;

        const std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;

        if (line.rfind(L"theme=", 0) == 0)
        {
            m_themeName = Trim(line.substr(6));
            continue;
        }

        std::vector<std::wstring> f;
        size_t fp = 0;
        while (true)
        {
            const size_t bar = line.find(L'|', fp);
            f.push_back(Trim(line.substr(fp, bar == std::wstring::npos ? std::wstring::npos : bar - fp)));
            if (bar == std::wstring::npos) break;
            fp = bar + 1;
        }

        if (f.size() < 4) { DebugSkip(lineNo, L"need style|label|action|target", line); continue; }

        Button b;
        if (!ParseStyle(f[0], b.style))   { DebugSkip(lineNo, L"bad style",  line); continue; }
        b.label = f[1];
        if (!ParseAction(f[2], b.action)) { DebugSkip(lineNo, L"bad action", line); continue; }
        b.target = f[3];
        if (f.size() >= 5) b.iconPath = f[4];
        if (b.label.empty() || b.target.empty()) { DebugSkip(lineNo, L"empty label/target", line); continue; }

        if (b.action == ButtonAction::FolderFan)
        {
            // No filesystem access here (UI thread rule); cached root fills immediately, uncached root queued for worker scan
            auto cached = m_folderFanCache.find(b.target);
            if (cached != m_folderFanCache.end())
            {
                b.folderEntries = cached->second;
            }
            else if (std::find(m_pendingFolderScans.begin(), m_pendingFolderScans.end(), b.target)
                     == m_pendingFolderScans.end())
            {
                m_pendingFolderScans.push_back(b.target);
            }
        }

        b.id = L"btn" + std::to_wstring(m_buttons.size());
        m_buttons.push_back(std::move(b));
    }

    // Drop cache entries for roots no longer referenced by any FolderFan button — a config
    // that edits a FolderFan target repeatedly would otherwise grow this map forever.
    const std::vector<std::wstring> currentRoots = FolderFanRoots();
    for (auto it = m_folderFanCache.begin(); it != m_folderFanCache.end(); )
    {
        if (std::find(currentRoots.begin(), currentRoots.end(), it->first) == currentRoots.end())
            it = m_folderFanCache.erase(it);
        else
            ++it;
    }

    DebugPrintf(L"[Launcher] loaded %zu button(s) from %s\n", m_buttons.size(), path.c_str());
}

std::vector<std::wstring> Launcher::FolderFanRoots() const
{
    std::vector<std::wstring> roots;
    for (const Button& b : m_buttons)
    {
        if (b.action != ButtonAction::FolderFan) continue;
        if (std::find(roots.begin(), roots.end(), b.target) == roots.end())
            roots.push_back(b.target);
    }
    return roots;
}

void Launcher::ApplyFolderScan(const std::wstring& root, std::vector<std::wstring> entries)
{
    m_folderFanCache[root] = entries;
    for (Button& b : m_buttons)
        if (b.action == ButtonAction::FolderFan && b.target == root)
            b.folderEntries = entries;
}

void Launcher::Execute(const Button& b) const
{
    const ButtonAction action = b.action;
    std::wstring target = b.target;
    std::thread([action, target = std::move(target)]() mutable {
        const long long tStartUs = trace::NowUs();
        // MTA (not STA: no pump prevents DDE-style handler hang); balance CoUninitialize only on successful init
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT actionHr = S_OK;
        switch (action)
        {
        case ButtonAction::Url:
        case ButtonAction::Shortcut:
        {
            const HINSTANCE ret = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(ret) <= 32) actionHr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        case ButtonAction::Command:
        {
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, target.data(), nullptr, nullptr, FALSE,
                               0, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            else
            {
                actionHr = HRESULT_FROM_WIN32(GetLastError());
            }
            break;
        }
        case ButtonAction::FolderFan:
            // Picking a subfolder happens through the fan (LaunchFolder), not a direct
            // click on the button itself — nothing to launch here.
            break;
        }
        if (SUCCEEDED(hr)) CoUninitialize();
        TRACE_EVENT("LauncherAction",
            TraceLoggingWideString(ActionName(action), "action"),
            TraceLoggingInt64(trace::NowUs() - tStartUs, "duration_us"),
            TraceLoggingInt32(actionHr, "hr"));
    }).detach();
}

void Launcher::LaunchFolder(const std::wstring& folderPath) const
{
    // cmd /k keeps tab alive after claude exits (doesn't depend on user's default shell)
    RunCommandWorker(L"wt.exe -d \"" + folderPath + L"\" cmd /k claude", L"folderfan");
}
