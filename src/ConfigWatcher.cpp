#include "ConfigWatcher.h"

ConfigWatcher::ConfigWatcher(HWND dockHwnd, UINT changeMsg)
    : m_hwnd(dockHwnd), m_changeMsg(changeMsg)
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
}

ConfigWatcher::~ConfigWatcher()
{
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

void ConfigWatcher::Start(std::wstring dir, std::wstring fileName)
{
    m_dir  = std::move(dir);
    m_file = std::move(fileName);
    if (m_dir.empty() || !m_stopEvent) return;
    m_thread = std::thread([this] { WorkerLoop(); });
}

void ConfigWatcher::WorkerLoop()
{
    const HANDLE hDir = CreateFileW(
        m_dir.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (hDir == INVALID_HANDLE_VALUE) return;

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
    if (!ov.hEvent) { CloseHandle(hDir); return; }

    alignas(DWORD) BYTE buf[4096];
    const DWORD filter = FILE_NOTIFY_CHANGE_LAST_WRITE |
                         FILE_NOTIFY_CHANGE_FILE_NAME |
                         FILE_NOTIFY_CHANGE_DIR_NAME |
                         FILE_NOTIFY_CHANGE_SIZE;
    const bool anyChange = m_file.empty();
    HANDLE waits[2] = { ov.hEvent, m_stopEvent };

    bool pending = false;
    while (true)
    {
        ResetEvent(ov.hEvent);
        if (!ReadDirectoryChangesW(hDir, buf, sizeof(buf), FALSE, filter, nullptr, &ov, nullptr))
            break;
        pending = true;

        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
            break;

        DWORD transferred = 0;
        pending = false;
        if (!GetOverlappedResult(hDir, &ov, &transferred, FALSE))
            break;

        bool matched = anyChange || (transferred == 0);
        for (DWORD offset = 0; !matched && offset < transferred; )
        {
            const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf + offset);
            const std::wstring name(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
            if (_wcsicmp(name.c_str(), m_file.c_str()) == 0) matched = true;
            if (fni->NextEntryOffset == 0) break;
            offset += fni->NextEntryOffset;
        }
        if (matched && anyChange)
        {
            auto* dir = new std::wstring(m_dir);
            if (!PostMessageW(m_hwnd, m_changeMsg, 0, reinterpret_cast<LPARAM>(dir)))
                delete dir;
        }
        else if (matched)
        {
            PostMessageW(m_hwnd, m_changeMsg, 0, 0);
        }
    }

    // Drain pending I/O so kernel doesn't signal/write to stack after close
    if (pending)
    {
        CancelIo(hDir);
        DWORD drained = 0;
        GetOverlappedResult(hDir, &ov, &drained, TRUE);
    }
    CloseHandle(ov.hEvent);
    CloseHandle(hDir);
}
