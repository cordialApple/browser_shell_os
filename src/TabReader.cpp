#include "TabReader.h"
#include "TabHop.h"
#include "Trace.h"
#include <UIAutomation.h>
#include <wrl/client.h>
#include <cwchar>
#include <string>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace
{

// 15ms: worker thread has no message pump for WinEventHook; tighter poll ceiling caps latency at 15ms vs 50ms.
constexpr int kPollIntervalMs  = 15;
constexpr int kReadyTimeoutMs  = 3000;   // window visible && !iconic
constexpr int kTreeTimeoutMs   = 3000;   // TabControl with TabItems present
constexpr int kConfirmSettleMs = 60;     // settle before re-reading IsSelected

// Depth alone doesn't bound cost: wide sibling fan-out would turn one FindLiveTabItems into unbounded FindAll calls.
constexpr int kGuidedDescentMaxDepth = 25;
constexpr int kGuidedDescentMaxNodes = 500;

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

static const wchar_t* OutcomeName(ActivateOutcome o)
{
    switch (o)
    {
    case ActivateOutcome::Selected:           return L"Selected";
    case ActivateOutcome::NoMatch:            return L"NoMatch";
    case ActivateOutcome::PatternUnavailable: return L"PatternUnavailable";
    case ActivateOutcome::Failed:             return L"Failed";
    }
    return L"Unknown";
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

// Truncated walk's candidate list cannot be trusted as complete; caller must fall back to blanket search.
struct GuidedDescentState
{
    int  visited   = 0;
    bool truncated = false;
};

// Pruned recursive descent vs. blanket TreeScope_Descendants FindAll. Prunes at Document boundary but every candidate still rechecked via IsInsideDocument (safety backstop).
static void FindTabControlsGuided(IUIAutomationElement* node, IUIAutomationCondition* trueCond,
                                   int depth, std::vector<ComPtr<IUIAutomationElement>>& outCandidates,
                                   GuidedDescentState& state)
{
    if (depth > kGuidedDescentMaxDepth) { state.truncated = true; return; }

    ComPtr<IUIAutomationElementArray> children;
    if (FAILED(node->FindAll(TreeScope_Children, trueCond, &children)) || !children)
    {
        state.truncated = true;
        return;
    }

    int count = 0;
    children->get_Length(&count);
    for (int i = 0; i < count; ++i)
    {
        if (state.visited >= kGuidedDescentMaxNodes) { state.truncated = true; return; }
        ++state.visited;

        ComPtr<IUIAutomationElement> child;
        if (FAILED(children->GetElement(i, &child)) || !child) { state.truncated = true; continue; }

        CONTROLTYPEID ct = 0;
        if (FAILED(child->get_CurrentControlType(&ct))) { state.truncated = true; continue; }
        if (ct == UIA_DocumentControlTypeId) continue;   // web content, skip entire subtree

        if (ct == UIA_TabControlTypeId) outCandidates.push_back(child);

        FindTabControlsGuided(child.Get(), trueCond, depth + 1, outCandidates, state);
    }
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

// Re-read IsSelected: Select() returns S_OK even on silent-no-op failure.
static bool IsItemSelected(IUIAutomationElement* item)
{
    VARIANT v = {};
    bool sel = false;
    if (SUCCEEDED(item->GetCurrentPropertyValue(UIA_SelectionItemIsSelectedPropertyId, &v)))
        sel = (v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE);
    VariantClear(&v);
    return sel;
}

// Per-candidate fields accumulate across ci loop into scalars (1-2 candidates typical).
struct WalkTiming
{
    long long usElementFromHandle = 0;
    long long usFindAllTabCtrls   = 0;   // guided descent, or the blanket-search fallback if it found nothing
    long long usIsInsideDocument  = 0;   // accrues every candidate, incl. continue'd ones
    long long usFindAllTabItems   = 0;   // accrues only when FindAllBuildCache actually runs
    int       tabCtrlCandidates   = 0;   // ctrlCount
    bool      guidedDescentUsed   = false;
};

// Keeps live TabItem array; elements not durable across restore so activation must re-walk fresh.
// Single FindAllBuildCache vs. 2N+1 COM calls per item. Returned elements still live for Select/SetFocus.
static HRESULT FindLiveTabItems(IUIAutomation* automation, HWND hwnd,
                                ComPtr<IUIAutomationElementArray>& outItems,
                                std::vector<Tab>& outTabs,
                                WalkTiming& timing)
{
    outItems.Reset();
    outTabs.clear();
    timing = WalkTiming{};

    long long tStartUs = trace::NowUs();
    ComPtr<IUIAutomationElement> elem;
    if (FAILED(automation->ElementFromHandle(hwnd, &elem)) || !elem) return E_FAIL;
    timing.usElementFromHandle = trace::NowUs() - tStartUs;

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

    long long tFindCtrlsUs = trace::NowUs();
    std::vector<ComPtr<IUIAutomationElement>> guidedCandidates;
    ComPtr<IUIAutomationCondition> trueCond;
    GuidedDescentState guidedState;
    if (SUCCEEDED(automation->CreateTrueCondition(&trueCond)) && trueCond)
        FindTabControlsGuided(elem.Get(), trueCond.Get(), 0, guidedCandidates, guidedState);

    // Fallback if guided truncated: blanket search so correctness never regresses.
    ComPtr<IUIAutomationElementArray> tabCtrls;
    const bool usedGuided = !guidedCandidates.empty() && !guidedState.truncated;
    int ctrlCount = 0;
    if (usedGuided)
    {
        ctrlCount = static_cast<int>(guidedCandidates.size());
    }
    else
    {
        if (FAILED(elem->FindAll(TreeScope_Descendants, tabCtrlCond.Get(), &tabCtrls)) || !tabCtrls)
            return E_FAIL;
        tabCtrls->get_Length(&ctrlCount);
    }
    timing.usFindAllTabCtrls = trace::NowUs() - tFindCtrlsUs;
    timing.tabCtrlCandidates = ctrlCount;
    timing.guidedDescentUsed = usedGuided;

    vt.lVal = UIA_TabItemControlTypeId;
    ComPtr<IUIAutomationCondition> tabItemCond;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &tabItemCond)))
        return E_FAIL;

    for (int ci = 0; ci < ctrlCount; ++ci)
    {
        ComPtr<IUIAutomationElement> tabCtrl;
        if (usedGuided) tabCtrl = guidedCandidates[ci];
        else if (FAILED(tabCtrls->GetElement(ci, &tabCtrl)) || !tabCtrl) continue;

        long long tInsideDocUs = trace::NowUs();
        const bool insideDoc = IsInsideDocument(automation, tabCtrl.Get());
        timing.usIsInsideDocument += trace::NowUs() - tInsideDocUs;
        if (insideDoc) continue;

        ComPtr<IUIAutomationElementArray> items;
        long long tFindItemsUs = trace::NowUs();
        const HRESULT hrItems = tabCtrl->FindAllBuildCache(TreeScope_Descendants, tabItemCond.Get(),
                                                            cacheReq.Get(), &items);
        timing.usFindAllTabItems += trace::NowUs() - tFindItemsUs;
        if (FAILED(hrItems) || !items) continue;

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

// Never selects wrong tab: title-first, fallbackIndex breaks ties only.
// Emits FanActivateLatency on every exit path, reusing existing timestamps.
TabActivateResult ActivateTab(IUIAutomation* automation, HWND hwnd,
                              const std::wstring& wantedTitle, int fallbackIndex,
                              const std::atomic<bool>& stop,
                              long long tClickUs, long long tRestoreUs)
{
    TabActivateResult r{ hwnd, ActivateOutcome::Failed, -1, {} };

    long long tTabFoundUs = 0, tSelectAttemptUs = 0, tConfirmUs = 0;
    long long tReadyUs = 0;
    int gate1Attempts = 0, gate2Attempts = 0;
    long long tFirstWalkUs = -1, tLastWalkUs = -1;
    WalkTiming lastWalkTiming;
    auto Finish = [&]() -> TabActivateResult
    {
        const long long tEndUs = tConfirmUs ? tConfirmUs : trace::NowUs();
        TRACE_EVENT("FanActivateLatency",
            TraceLoggingWideString(OutcomeName(r.outcome), "outcome"),
            TraceLoggingInt64(tRestoreUs - tClickUs, "us_click_to_restore"),
            TraceLoggingInt64(tTabFoundUs ? tTabFoundUs - tRestoreUs : -1, "us_restore_to_tabfound"),
            TraceLoggingInt64(tSelectAttemptUs ? tSelectAttemptUs - tTabFoundUs : -1, "us_tabfound_to_select"),
            TraceLoggingInt64(tConfirmUs ? tConfirmUs - tSelectAttemptUs : -1, "us_select_to_confirm"),
            TraceLoggingInt64(tEndUs - tClickUs, "duration_us"),
            TraceLoggingInt64(tReadyUs ? tReadyUs - tRestoreUs : -1, "us_gate1_wait"),
            TraceLoggingInt32(gate1Attempts, "gate1_attempts"),
            TraceLoggingInt64((tTabFoundUs && tReadyUs) ? tTabFoundUs - tReadyUs : -1, "us_gate2_wait"),
            TraceLoggingInt32(gate2Attempts, "gate2_attempts"),
            TraceLoggingInt64(tFirstWalkUs, "us_first_walk"),
            TraceLoggingInt64(tLastWalkUs, "us_last_walk"),
            TraceLoggingInt64(lastWalkTiming.usElementFromHandle, "us_element_from_handle"),
            TraceLoggingInt64(lastWalkTiming.usFindAllTabCtrls, "us_findall_tabctrls"),
            TraceLoggingInt64(lastWalkTiming.usIsInsideDocument, "us_is_inside_document"),
            TraceLoggingInt64(lastWalkTiming.usFindAllTabItems, "us_findall_tabitems"),
            TraceLoggingInt32(lastWalkTiming.tabCtrlCandidates, "tabctrl_candidates"),
            TraceLoggingInt32(lastWalkTiming.guidedDescentUsed ? 1 : 0, "guided_descent_used"));
        return std::move(r);
    };

    if (!automation) return Finish();

    const long long t0 = NowMs();

    bool ready = false;
    while (IsWindow(hwnd) && NowMs() - t0 < kReadyTimeoutMs)
    {
        ++gate1Attempts;
        if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) { ready = true; break; }
        if (CancelableSleep(stop, kPollIntervalMs)) return Finish();
    }
    if (!ready) return Finish();
    tReadyUs = trace::NowUs();

    ComPtr<IUIAutomationElementArray> items;
    std::vector<Tab> tabs;
    bool haveTree = false;
    while (NowMs() - t0 < kTreeTimeoutMs)
    {
        ++gate2Attempts;
        const long long tWalkStartUs = trace::NowUs();
        WalkTiming walkTiming;
        const bool walkOk = SUCCEEDED(FindLiveTabItems(automation, hwnd, items, tabs, walkTiming)) && items && !tabs.empty();
        const long long walkUs = trace::NowUs() - tWalkStartUs;
        if (tFirstWalkUs < 0) tFirstWalkUs = walkUs;
        tLastWalkUs = walkUs;
        lastWalkTiming = walkTiming;
        if (walkOk) { haveTree = true; break; }
        if (CancelableSleep(stop, kPollIntervalMs)) return Finish();
    }
    if (!haveTree) return Finish();
    tTabFoundUs = trace::NowUs();   // D: tab tree available to match against

    r.freshTabs = tabs;

    int idx = -1;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i)
        if (tabs[i].title == wantedTitle)
        {
            if (idx < 0) idx = i;
            if (i == fallbackIndex) idx = i;
        }
    if (idx < 0) { r.outcome = ActivateOutcome::NoMatch; return Finish(); }

    auto markSelected = [&] {
        r.matchedIndex = idx;
        r.outcome = ActivateOutcome::Selected;
        for (auto& t : r.freshTabs) t.active = false;
        if (idx < static_cast<int>(r.freshTabs.size())) r.freshTabs[idx].active = true;
    };

    ComPtr<IUIAutomationElement> item;
    if (FAILED(items->GetElement(idx, &item)) || !item) return Finish();

    ComPtr<IUIAutomationSelectionItemPattern> selPat;
    if (SUCCEEDED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selPat)))
        && selPat)
    {
        selPat->Select();
    }
    else
    {
        r.outcome = ActivateOutcome::PatternUnavailable;
    }
    tSelectAttemptUs = trace::NowUs();   // E: activation attempted

    if (CancelableSleep(stop, kConfirmSettleMs)) return Finish();
    if (IsItemSelected(item.Get()))
    {
        tConfirmUs = trace::NowUs();   // F: activation-confirmed proxy
        markSelected();
        return Finish();
    }

    // Fallback: SetFocus → LegacyIAccessible.DoDefaultAction (not Invoke).
    item->SetFocus();
    if (CancelableSleep(stop, kConfirmSettleMs)) return Finish();
    if (!IsItemSelected(item.Get()))
    {
        ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
        if (SUCCEEDED(item->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                                IID_PPV_ARGS(&legacy))) && legacy)
        {
            legacy->DoDefaultAction();
            if (CancelableSleep(stop, kConfirmSettleMs)) return Finish();
        }
    }

    if (IsItemSelected(item.Get()))
    {
        tConfirmUs = trace::NowUs();   // F: activation-confirmed proxy (fallback path)
        markSelected();
    }
    else if (r.outcome != ActivateOutcome::PatternUnavailable)
    {
        r.outcome = ActivateOutcome::Failed;
    }
    return Finish();
}

// Experiment (exp/keystroke-optimal): skip the UIA walk+Select. PlanTabHops returns the minimal
// ring-hop sequence (direct Ctrl+digit, relative Ctrl+PgUp/PgDn walk, or jump+walk); this maps
// each Hop to its VK, and KeystrokeHop fires the whole sequence as one batched SendInput.
static WORD HopVk(const Hop& h)
{
    switch (h.kind)
    {
    case HopKind::Next:      return VK_NEXT;
    case HopKind::Prev:      return VK_PRIOR;
    case HopKind::JumpDigit: return static_cast<WORD>('0' + h.digit);
    case HopKind::JumpLast:  return '9';
    }
    return 0;
}

static TabActivateResult KeystrokeHop(HWND hwnd, int activeIndex, int targetIndex, int tabCount,
                                      const std::atomic<bool>& stop,
                                      long long tClickUs, long long tRestoreUs)
{
    TabActivateResult r{ hwnd, ActivateOutcome::Failed, -1, {} };

    long long tReadyUs = 0, tDoneUs = 0;
    int hopCount = 0, usedJump = 0;
    auto Finish = [&]() -> TabActivateResult {
        const long long tEndUs = tDoneUs ? tDoneUs : trace::NowUs();
        TRACE_EVENT("KeystrokeHopLatency",
            TraceLoggingWideString(OutcomeName(r.outcome), "outcome"),
            TraceLoggingInt32(activeIndex, "active_index"),
            TraceLoggingInt32(targetIndex, "target_index"),
            TraceLoggingInt32(tabCount, "tab_count"),
            TraceLoggingInt32(hopCount, "hop_count"),
            TraceLoggingInt32(usedJump, "used_jump"),
            TraceLoggingInt64(tRestoreUs - tClickUs, "us_click_to_restore"),
            TraceLoggingInt64(tReadyUs ? tReadyUs - tRestoreUs : -1, "us_restore_to_ready"),
            TraceLoggingInt64((tDoneUs && tReadyUs) ? tDoneUs - tReadyUs : -1, "us_ready_to_done"),
            TraceLoggingInt64(tEndUs - tClickUs, "duration_us"));
        return std::move(r);
    };

    // Injected input lands on the foreground window: gate until the target actually IS foreground.
    const long long t0 = NowMs();
    bool ready = false;
    while (IsWindow(hwnd) && NowMs() - t0 < kReadyTimeoutMs)
    {
        if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd) { ready = true; break; }
        if (CancelableSleep(stop, kPollIntervalMs)) return Finish();
    }
    if (!ready) return Finish();
    tReadyUs = trace::NowUs();

    r.matchedIndex = targetIndex;
    r.outcome = ActivateOutcome::Selected;   // optimistic: this path never confirms via UIA

    const bool activeKnown = activeIndex >= 0;
    const std::vector<Hop> hops = PlanTabHops(
        activeKnown ? activeIndex + 1 : 1, targetIndex + 1, tabCount, activeKnown);
    hopCount = static_cast<int>(hops.size());
    if (!hops.empty())
    {
        if (GetForegroundWindow() != hwnd) { r.outcome = ActivateOutcome::Failed; return Finish(); }

        std::vector<INPUT> seq;
        seq.reserve(hops.size() * 2 + 2);
        auto key = [&](WORD vk, DWORD flags) {
            INPUT e{};
            e.type = INPUT_KEYBOARD;
            e.ki.wVk = vk;
            e.ki.dwFlags = flags;
            seq.push_back(e);
        };

        key(VK_CONTROL, 0);
        for (const Hop& h : hops)
        {
            if (h.kind == HopKind::JumpDigit || h.kind == HopKind::JumpLast) usedJump = 1;
            const WORD vk = HopVk(h);
            key(vk, 0);
            key(vk, KEYEVENTF_KEYUP);
        }
        key(VK_CONTROL, KEYEVENTF_KEYUP);

        SendInput(static_cast<UINT>(seq.size()), seq.data(), sizeof(INPUT));
    }

    tDoneUs = trace::NowUs();
    return Finish();
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

    // m_stop only bounds sleeps; cannot interrupt wedged UIA/COM calls. Bounded-join: if call hangs after timeout, detach (shutdown-only UB).
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
            if (req.kind == ReqKind::Snapshot && req.hwnd == hwnd) return;
        m_queue.push_back({ ReqKind::Snapshot, hwnd, {}, 0 });
    }
    m_cv.notify_one();
}

void TabReader::RequestActivate(HWND hwnd, std::wstring wantedTitle, int fallbackIndex,
                                long long tClickUs, long long tRestoreUs)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_stop) return;
        m_queue.push_back({ ReqKind::Activate, hwnd, std::move(wantedTitle), fallbackIndex,
                            tClickUs, tRestoreUs });
    }
    m_cv.notify_one();
}

void TabReader::RequestKeystrokeHop(HWND hwnd, int activeIndex, int targetIndex, int tabCount,
                                    long long tClickUs, long long tRestoreUs)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_stop) return;
        Request req{ ReqKind::KeystrokeHop, hwnd, {}, 0, tClickUs, tRestoreUs,
                     targetIndex, tabCount, activeIndex };
        m_queue.push_back(std::move(req));
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
                const long long tStartUs = trace::NowUs();
                std::vector<Tab> tabs;
                if (automation)
                    tabs = SnapshotTabs(automation.Get(), req.hwnd);

                const bool failed = tabs.empty();
                TRACE_EVENT("UiaSnapshot",
                    TraceLoggingInt64(trace::NowUs() - tStartUs, "duration_us"),
                    TraceLoggingPointer(req.hwnd, "hwnd"),
                    TraceLoggingInt32(static_cast<int32_t>(tabs.size()), "tab_count"),
                    TraceLoggingInt32(automation ? (failed ? E_FAIL : S_OK) : E_HANDLE, "hr"));
                auto* payload = new TabSnapshot{ req.hwnd, std::move(tabs), failed };
                if (!PostMessageW(m_dockHwnd, m_snapshotMsg,
                                  reinterpret_cast<WPARAM>(req.hwnd),
                                  reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            }
            else  // ReqKind::Activate or ReqKind::KeystrokeHop
            {
                TabActivateResult result{ req.hwnd, ActivateOutcome::Failed, -1, {} };
                if (req.kind == ReqKind::KeystrokeHop)
                    result = KeystrokeHop(req.hwnd, req.activeIndex, req.targetIndex, req.tabCount,
                                          m_stop, req.tClickUs, req.tRestoreUs);
                else if (automation)
                    result = ActivateTab(automation.Get(), req.hwnd,
                                         req.wantedTitle, req.fallbackIndex, m_stop,
                                         req.tClickUs, req.tRestoreUs);

                auto* payload = new TabActivateResult(std::move(result));
                if (!PostMessageW(m_dockHwnd, m_activateMsg,
                                  reinterpret_cast<WPARAM>(req.hwnd),
                                  reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            }
        }
        catch (...) {}
    }

    CoUninitialize();
}
