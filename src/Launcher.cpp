#include "Launcher.h"
#include <shlobj.h>
#include <cstdarg>
#include <cstring>
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
        // _TRUNCATE: an unbounded config line must not trip vswprintf's fatal handler.
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
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

    // Read the whole file and decode to UTF-16, honoring a UTF-16LE or UTF-8 BOM;
    // no BOM is treated as UTF-8 (what Notepad/VS Code save by default).
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
        if (s == L"url")      { out = ButtonAction::Url;      return true; }
        if (s == L"shortcut") { out = ButtonAction::Shortcut; return true; }
        if (s == L"command")  { out = ButtonAction::Command;  return true; }
        return false;
    }
}

std::wstring Launcher::ConfigPath()
{
    PWSTR local = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
    std::wstring path;
    if (SUCCEEDED(hr) && local) path = local;
    CoTaskMemFree(local);  // no-op on null; safe on every path
    if (path.empty()) return L"";
    path += L"\\browser_shell_os\\config.txt";
    return path;
}

void Launcher::Load()
{
    m_buttons.clear();

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

        b.id = L"btn" + std::to_wstring(m_buttons.size());
        m_buttons.push_back(std::move(b));
    }

    DebugPrintf(L"[Launcher] loaded %zu button(s) from %s\n", m_buttons.size(), path.c_str());
}

void Launcher::Execute(const Button& b) const
{
    const ButtonAction action = b.action;
    std::wstring target = b.target;
    std::thread([action, target = std::move(target)]() mutable {
        // MTA, not STA: this worker runs no message pump, so an STA (which requires
        // one) could hang a DDE-style shell handler. Balance CoUninitialize only on
        // a successful init (RPC_E_CHANGED_MODE must not be uninitialized).
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        switch (action)
        {
        case ButtonAction::Url:
        case ButtonAction::Shortcut:
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
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
            break;
        }
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();
}
