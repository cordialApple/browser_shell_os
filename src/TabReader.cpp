#include "TabReader.h"
#include <UIAutomation.h>
#include <wrl/client.h>
#include <cwchar>
#include <string>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace
{

// Activation retry budget (R2). Restore is async; Select on a still-iconic or
// mid-animation window returns S_OK but SILENTLY no-ops. Poll readiness, then the
// (async-rebuilt) tab tree, then confirm the selection actually took.
// 15ms (not event-driven — see ActivateTab comment): the worker thread has no
// message pump, so a WinEventHook here can't fire; that fix needs a UI-thread
// hook + a new signal path, deferred. Cheap interim win: tighter poll ceiling
// caps Gate 1/2 worst-case added latency at 15ms instead of 50ms.
constexpr int kPollIntervalMs  = 15;
constexpr int kReadyTimeoutMs  = 3000;   // window visible && !iconic
constexpr int kTreeTimeoutMs   = 3000;   // TabControl with TabItems present
constexpr int kConfirmSettleMs = 60;     // settle before re-reading IsSelected

static long long NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Sleep that observes the worker's stop flag so teardown (join on the UI thread)
// is never blocked for a full retry budget. Returns true if a stop was requested.
static bool CancelableSleep(const std::atomic<bool>& stop, int ms)
{
    if (stop.load()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return stop.load();
}

static bool EndsWith(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Edge appends status/telemetry suffixes to tab TabItem names, e.g.
// "<title> - Pinned - Sleeping - Memory usage - 103 MB". Strip them.
static std::wstring CleanTabTitle(std::wstring s)
{
    const size_t mu = s.rfind(L" - Memory usage");
    if (mu != std::wstring::npos)
        s.erase(mu);
    for (bool changed = true; changed; )
    {
        changed = false;
        for (const wchar_t* suf : { L" - Sleeping", L" - Pinned" })
            if (EndsWith(s, suf)) { s.erase(s.size() - wcslen(suf)); changed = true; }
    }
    return s;
}

// Walk up the parent chain looking for a Document control type.
// Browser chrome elements are never inside a document; web content always is.
static bool IsInsideDocument(IUIAutomation* automation, IUIAutomationElement* startElem)
{
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_RawViewWalker(&walker)) || !walker) return false;

    ComPtr<IUIAutomationElement> current;
    startElem->QueryInterface(IID_PPV_ARGS(&current));

    for (int depth = 0; depth < 30 && current; ++depth)
    {
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(current.Get(), &parent)) || !parent) break;

        CONTROLTYPEID ct = 0;
        if (SUCCEEDED(parent->get_CurrentControlType(&ct)) &&
            ct == UIA_DocumentControlTypeId)
            return true;

        current = std::move(parent);
    }
    return false;
}

std::vector<Tab> SnapshotTabs(IUIAutomation* automation, HWND hwnd)
{
    ComPtr<IUIAutomationElement> elem;
    if (FAILED(automation->ElementFromHandle(hwnd, &elem)) || !elem) return {};

    ComPtr<IUIAutomationCacheRequest> cacheReq;
    if (FAILED(automation->CreateCacheRequest(&cacheReq))) return {};
    cacheReq->AddProperty(UIA_NamePropertyId);
    cacheReq->AddProperty(UIA_SelectionItemIsSelectedPropertyId);

    VARIANT vt = {};
    vt.vt   = VT_I4;
    vt.lVal = UIA_TabControlTypeId;
    ComPtr<IUIAutomationCondition> tabCtrlCond;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &tabCtrlCond)))
        return {};

    ComPtr<IUIAutomationElementArray> tabCtrls;
    if (FAILED(elem->FindAll(TreeScope_Descendants, tabCtrlCond.Get(), &tabCtrls)) || !tabCtrls)
        return {};

    int ctrlCount = 0;
    tabCtrls->get_Length(&ctrlCount);

    vt.lVal = UIA_TabItemControlTypeId;
    ComPtr<IUIAutomationCondition> tabItemCond;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &tabItemCond)))
        return {};

    for (int ci = 0; ci < ctrlCount; ++ci)
    {
        ComPtr<IUIAutomationElement> tabCtrl;
        if (FAILED(tabCtrls->GetElement(ci, &tabCtrl)) || !tabCtrl) continue;

        // Skip web-content tab controls — they live inside a Document node.
        if (IsInsideDocument(automation, tabCtrl.Get())) continue;

        // TabItems are nested inside layout panes, not direct children — use Descendants.
        ComPtr<IUIAutomationElementArray> items;
        if (FAILED(tabCtrl->FindAllBuildCache(TreeScope_Descendants, tabItemCond.Get(),
                                               cacheReq.Get(), &items)) || !items)
            continue;

        int count = 0;
        items->get_Length(&count);
        if (count == 0) continue;

        std::vector<Tab> tabs;
        tabs.reserve(count);
        bool sawActive = false;
        for (int i = 0; i < count; ++i)
        {
            ComPtr<IUIAutomationElement> item;
            if (FAILED(items->GetElement(i, &item)) || !item) continue;
            BSTR name = nullptr;
            if (SUCCEEDED(item->get_CachedName(&name)) && name)
            {
                std::wstring title = CleanTabTitle(name);
                if (!title.empty())
                {
                    bool active = false;
                    VARIANT sel = {};
                    if (SUCCEEDED(item->GetCachedPropertyValue(
                            UIA_SelectionItemIsSelectedPropertyId, &sel)))
                        active = (sel.vt == VT_BOOL && sel.boolVal != VARIANT_FALSE);
                    VariantClear(&sel);
                    if (active && sawActive) active = false;  // one active tab max
                    sawActive = sawActive || active;
                    tabs.push_back({ std::move(title), active });
                }
                SysFreeString(name);
            }
        }
        if (!tabs.empty())
            return tabs;
    }
    return {};
}

// Confirm a tab is selected by re-reading IsSelected (Select() returns S_OK even
// on the silent-no-op failure mode). Worker thread, UIA only.
static bool IsItemSelected(IUIAutomationElement* item)
{
    VARIANT v = {};
    bool sel = false;
    if (SUCCEEDED(item->GetCurrentPropertyValue(UIA_SelectionItemIsSelectedPropertyId, &v)))
        sel = (v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE);
    VariantClear(&v);
    return sel;
}

// Like SnapshotTabs but KEEPS the live TabItem element array (elements are not
// durable across restore, so activation must re-walk fresh and act immediately).
// Names/IsSelected are read from a single FindAllBuildCache round trip (mirrors
// SnapshotTabs) instead of one GetCurrentName+GetCurrentPropertyValue pair per
// item — was up to 2N+1 cross-process COM calls on the click-to-activate path,
// now one. BuildCache only prefetches properties; the returned elements are
// still live and safe to Select()/SetFocus() afterward.
// Returns S_OK with a non-empty array + parallel tabs, or E_FAIL if no tab tree yet.
static HRESULT FindLiveTabItems(IUIAutomation* automation, HWND hwnd,
                                ComPtr<IUIAutomationElementArray>& outItems,
                                std::vector<Tab>& outTabs)
{
    outItems.Reset();
    outTabs.clear();

    ComPtr<IUIAutomationElement> elem;
    if (FAILED(automation->ElementFromHandle(hwnd, &elem)) || !elem) return E_FAIL;

    ComPtr<IUIAutomationCacheRequest> cacheReq;
    if (FAILED(automation->CreateCacheRequest(&cacheReq))) return E_FAIL;
    cacheReq->AddProperty(UIA_NamePropertyId);
    cacheReq->AddProperty(UIA_SelectionItemIsSelectedPropertyId);

    VARIANT vt = {};
    vt.vt   = VT_I4;
    vt.lVal = UIA_TabControlTypeId;
    ComPtr<IUIAutomationCondition> tabCtrlCond;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &tabCtrlCond)))
        return E_FAIL;

    ComPtr<IUIAutomationElementArray> tabCtrls;
    if (FAILED(elem->FindAll(TreeScope_Descendants, tabCtrlCond.Get(), &tabCtrls)) || !tabCtrls)
        return E_FAIL;

    int ctrlCount = 0;
    tabCtrls->get_Length(&ctrlCount);

    vt.lVal = UIA_TabItemControlTypeId;
    ComPtr<IUIAutomationCondition> tabItemCond;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &tabItemCond)))
        return E_FAIL;

    for (int ci = 0; ci < ctrlCount; ++ci)
    {
        ComPtr<IUIAutomationElement> tabCtrl;
        if (FAILED(tabCtrls->GetElement(ci, &tabCtrl)) || !tabCtrl) continue;
        if (IsInsideDocument(automation, tabCtrl.Get())) continue;

        ComPtr<IUIAutomationElementArray> items;
        if (FAILED(tabCtrl->FindAllBuildCache(TreeScope_Descendants, tabItemCond.Get(),
                                               cacheReq.Get(), &items)) || !items)
            continue;

        int count = 0;
        items->get_Length(&count);
        if (count == 0) continue;

        std::vector<Tab> tabs;
        tabs.reserve(count);
        bool sawActive = false;
        for (int i = 0; i < count; ++i)
        {
            ComPtr<IUIAutomationElement> item;
            std::wstring title;
            bool active = false;
            if (SUCCEEDED(items->GetElement(i, &item)) && item)
            {
                BSTR name = nullptr;
                if (SUCCEEDED(item->get_CachedName(&name)) && name)
                {
                    title = CleanTabTitle(name);
                    SysFreeString(name);
                }
                VARIANT sel = {};
                if (SUCCEEDED(item->GetCachedPropertyValue(
                        UIA_SelectionItemIsSelectedPropertyId, &sel)))
                    active = (sel.vt == VT_BOOL && sel.boolVal != VARIANT_FALSE);
                VariantClear(&sel);
            }
            if (active && sawActive) active = false;
            sawActive = sawActive || active;
            tabs.push_back({ std::move(title), active });   // keep index parity with items
        }

        outItems = std::move(items);
        outTabs  = std::move(tabs);
        return S_OK;
    }
    return E_FAIL;
}

// Activate a specific tab in an already-restoring window. Never selects a wrong
// tab: title-first match, fallbackIndex only breaks ties among title matches, a
// bare index never selects alone. Restore is driven by the UI thread; here we only
// gate on readiness, act, and confirm. matchedIndex.active is set on success.
TabActivateResult ActivateTab(IUIAutomation* automation, HWND hwnd,
                              const std::wstring& wantedTitle, int fallbackIndex,
                              const std::atomic<bool>& stop)
{
    TabActivateResult r{ hwnd, ActivateOutcome::Failed, -1, {} };
    if (!automation) return r;

    const long long t0 = NowMs();

    // Gate 1: window readiness (restore is async). Bail fast if the window died
    // between the fan click and here (closed while the request was queued).
    bool ready = false;
    while (IsWindow(hwnd) && NowMs() - t0 < kReadyTimeoutMs)
    {
        if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) { ready = true; break; }
        if (CancelableSleep(stop, kPollIntervalMs)) return r;
    }
    if (!ready) return r;

    // Gate 2: tab tree rebuilt (async after restore).
    ComPtr<IUIAutomationElementArray> items;
    std::vector<Tab> tabs;
    bool haveTree = false;
    while (NowMs() - t0 < kTreeTimeoutMs)
    {
        if (SUCCEEDED(FindLiveTabItems(automation, hwnd, items, tabs)) && items && !tabs.empty())
        { haveTree = true; break; }
        if (CancelableSleep(stop, kPollIntervalMs)) return r;
    }
    if (!haveTree) return r;

    r.freshTabs = tabs;   // carry the re-snapshot back regardless of match outcome

    // Match: title-first, fallbackIndex tiebreak among matches only.
    int idx = -1;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i)
        if (tabs[i].title == wantedTitle)
        {
            if (idx < 0) idx = i;
            if (i == fallbackIndex) idx = i;
        }
    if (idx < 0) { r.outcome = ActivateOutcome::NoMatch; return r; }

    auto markSelected = [&] {
        r.matchedIndex = idx;
        r.outcome = ActivateOutcome::Selected;
        for (auto& t : r.freshTabs) t.active = false;
        if (idx < static_cast<int>(r.freshTabs.size())) r.freshTabs[idx].active = true;
    };

    ComPtr<IUIAutomationElement> item;
    if (FAILED(items->GetElement(idx, &item)) || !item) return r;

    // Select via SelectionItemPattern (live pattern — the action runs on the provider).
    ComPtr<IUIAutomationSelectionItemPattern> selPat;
    if (SUCCEEDED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selPat)))
        && selPat)
    {
        selPat->Select();
    }
    else
    {
        r.outcome = ActivateOutcome::PatternUnavailable;
        // fall through to the SetFocus/Legacy fallback below
    }

    if (CancelableSleep(stop, kConfirmSettleMs)) return r;
    if (IsItemSelected(item.Get()))
    {
        markSelected();
        return r;
    }

    // Fallback chain (R2): SetFocus → LegacyIAccessible.DoDefaultAction. NOT Invoke.
    item->SetFocus();
    if (CancelableSleep(stop, kConfirmSettleMs)) return r;
    if (!IsItemSelected(item.Get()))
    {
        ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
        if (SUCCEEDED(item->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                                IID_PPV_ARGS(&legacy))) && legacy)
        {
            legacy->DoDefaultAction();
            if (CancelableSleep(stop, kConfirmSettleMs)) return r;
        }
    }

    if (IsItemSelected(item.Get()))
    {
        markSelected();
    }
    else if (r.outcome != ActivateOutcome::PatternUnavailable)
    {
        r.outcome = ActivateOutcome::Failed;
    }
    return r;
}

} // namespace

TabReader::TabReader(HWND dockHwnd, UINT snapshotMsg, UINT activateMsg)
    : m_dockHwnd(dockHwnd), m_snapshotMsg(snapshotMsg), m_activateMsg(activateMsg),
      m_exit(std::make_shared<ExitSignal>())
{
    m_thread = std::thread([this, exit = m_exit] {
        WorkerLoop();
        {
            std::lock_guard<std::mutex> lk(exit->m);
            exit->exited = true;
        }
        exit->cv.notify_all();
    });
}

TabReader::~TabReader()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stop = true;
    }
    m_cv.notify_one();

    // m_stop only bounds the sleeps; it cannot interrupt an in-flight cross-process
    // UIA/COM call into a wedged browser provider — ActivateTab's Select/SetFocus/
    // DoDefaultAction OR SnapshotTabs/FindLiveTabItems' FindAll (F-02). A plain join()
    // would then stall WM_DESTROY forever, wedging process teardown. Bounded-join
    // instead: give an in-flight call a
    // moment to finish, else detach so teardown proceeds. Detach is shutdown-only: if
    // the call unhangs while the process is still tearing down (WM_DESTROY → message
    // loop exit, tens–hundreds of ms), the leaked worker touches freed members — UB,
    // but unobservable since the process is exiting.
    bool exited = false;
    {
        std::unique_lock<std::mutex> lk(m_exit->m);
        exited = m_exit->cv.wait_for(lk, std::chrono::seconds(2),
                                     [this] { return m_exit->exited; });
    }
    if (exited)
        m_thread.join();
    else if (m_thread.joinable())
        m_thread.detach();
}

void TabReader::RequestSnapshot(HWND hwnd)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_stop) return;
        for (const Request& req : m_queue)
            if (req.kind == ReqKind::Snapshot && req.hwnd == hwnd) return;  // de-dupe snapshots only
        m_queue.push_back({ ReqKind::Snapshot, hwnd, {}, 0 });
    }
    m_cv.notify_one();
}

void TabReader::RequestActivate(HWND hwnd, std::wstring wantedTitle, int fallbackIndex)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_stop) return;
        m_queue.push_back({ ReqKind::Activate, hwnd, std::move(wantedTitle), fallbackIndex });
    }
    m_cv.notify_one();
}

void TabReader::WorkerLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IUIAutomation> automation;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&automation));

    while (true)
    {
        Request req;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this] { return m_stop.load() || !m_queue.empty(); });
            if (m_stop) break;
            req = std::move(m_queue.front());
            m_queue.pop_front();
        }

        try
        {
            if (req.kind == ReqKind::Snapshot)
            {
                std::vector<Tab> tabs;
                if (automation)
                    tabs = SnapshotTabs(automation.Get(), req.hwnd);

                const bool failed = tabs.empty();
                auto* payload = new TabSnapshot{ req.hwnd, std::move(tabs), failed };
                if (!PostMessageW(m_dockHwnd, m_snapshotMsg,
                                  reinterpret_cast<WPARAM>(req.hwnd),
                                  reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            }
            else  // ReqKind::Activate
            {
                TabActivateResult result{ req.hwnd, ActivateOutcome::Failed, -1, {} };
                if (automation)
                    result = ActivateTab(automation.Get(), req.hwnd,
                                         req.wantedTitle, req.fallbackIndex, m_stop);

                auto* payload = new TabActivateResult(std::move(result));
                if (!PostMessageW(m_dockHwnd, m_activateMsg,
                                  reinterpret_cast<WPARAM>(req.hwnd),
                                  reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            }
        }
        catch (...) {}  // bad_alloc etc. → skip iteration, continue loop
    }

    CoUninitialize();
}
