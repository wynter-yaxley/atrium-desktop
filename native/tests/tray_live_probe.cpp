// Manual integration probe. No CTest registration: --click changes foreground/menu state.
// Links atrium_core_lib directly for the production TrayModel adapter and helpers.
#include "shell/tray_model.h"

#include <objbase.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <winrt/base.h>

#include <algorithm>
#include <iostream>

using namespace atrium;
namespace {
constexpr UINT kResult = WM_APP + 42;
int g_completion = -1;
HWND g_fixtureView = nullptr;
bool g_fixtureRespond = true;
std::vector<std::pair<UINT, UINT>> g_fixtureEvents;
LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == kResult && w)
        g_completion = int(w);
    if (m == WM_TIMER && w == 99) {
        EndMenu();
        return 0;
    }
    if (g_fixtureView && (m == WM_APP + 43 || m == WM_APP + 44)) {
        const bool v4 = m == WM_APP + 44;
        const UINT id = v4 ? HIWORD(l) : UINT(w), event = v4 ? LOWORD(l) : UINT(l);
        g_fixtureEvents.emplace_back(id, event);
        if (!g_fixtureRespond)
            return 0; // valid background-only action; no window/menu response
        if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT)
            ShowWindow(g_fixtureView, SW_SHOWNOACTIVATE);
        if (event == UINT(v4 ? WM_CONTEXTMENU : WM_RBUTTONUP)) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Atrium test menu (closes automatically)");
            SetTimer(h, 99, 450, nullptr);
            POINT at{};
            GetCursorPos(&at);
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, h, nullptr);
            KillTimer(h, 99);
            DestroyMenu(menu);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
void Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void PumpFor(DWORD ms)
{
    HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    }
    const ULONGLONG until = GetTickCount64() + ms;
    for (;;) {
        Pump();
        const ULONGLONG now = GetTickCount64();
        if (now >= until)
            break;
        const DWORD remaining = static_cast<DWORD>(until - now);
        if (timer) {
            if (MsgWaitForMultipleObjects(1, &timer, FALSE, remaining, QS_ALLINPUT) == WAIT_OBJECT_0)
                break;
        } else {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, remaining, QS_ALLINPUT);
        }
    }
    if (timer)
        CloseHandle(timer);
}

void WaitForModelLoaded(TrayModel& model, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (!model.Loaded() && GetTickCount64() < deadline)
        PumpFor(10);
}

void WaitForCompletion(DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (g_completion < 0 && GetTickCount64() < deadline)
        PumpFor(10);
}

bool OpenOverflow(TrayModel& model)
{
    g_completion = -1;
    model.OpenSystemTray();
    WaitForCompletion(5000);
    return g_completion == 5;
}

int RunWatch()
{
    // Read-only production worker exercise, including bounded startup retries.
    TrayModel model(nullptr, 0);
    model.Refresh();
    PumpFor(16500);
    std::wcout << L"worker loaded=" << model.Loaded() << L" available=" << model.Available() << L" icons=" << model.Icons().size() << L'\n';
    return model.Loaded() ? 0 : 1;
}

bool ExpectFixtureIcons(TrayModel& probe, const wchar_t* path, size_t expected)
{
    size_t count = 0;
    for (int i = 0; i < 20; ++i) {
        Pump();
        probe.Refresh();
        PumpFor(50);
        const auto icons = probe.Icons();
        count = size_t(std::ranges::count_if(icons, [&](const TrayIcon& icon) { return _wcsicmp(icon.exe.c_str(), path) == 0; }));
        if (count == expected)
            break;
        if (i == 19)
            for (const auto& icon : icons)
                std::wcout << L"fixture icon tip=" << icon.tip << L" exe=" << (icon.exe.empty() ? L"?" : icon.exe) << L'\n';
    }
    std::wcout << L"fixture expected=" << expected << L" actual=" << count << L'\n';
    return count == expected;
}

bool TestSingleFixtureClick(TrayModel& model, const wchar_t* path, const wchar_t* tip, UINT id, bool right)
{
    ShowWindow(g_fixtureView, SW_HIDE);
    model.Refresh();
    WaitForModelLoaded(model, 5000);
    const std::vector<TrayIcon> icons = model.Icons();
    const TrayIcon* chosen = nullptr;
    for (const auto& icon : icons) {
        if (_wcsicmp(icon.exe.c_str(), path) == 0 && icon.tip == tip) {
            if (chosen) {
                chosen = nullptr;
                break;
            }
            chosen = &icon;
        }
    }
    g_completion = -1;
    g_fixtureEvents.clear();
    if (chosen) {
        model.Click(*chosen, right);
        WaitForCompletion(5000);
    }
    const UINT expected = right ? (id == 4243 ? WM_CONTEXTMENU : WM_RBUTTONUP) : id == 4243 ? NIN_SELECT : WM_LBUTTONUP;
    size_t callbacks = 0;
    bool wrong = false;
    for (const auto& [recipient, event] : g_fixtureEvents) {
        if (event == expected || (!right && id == 4243 && event == NIN_KEYSELECT)) {
            ++callbacks;
            wrong |= recipient != id;
        }
    }
    const bool passed = g_completion == 1 && callbacks == 1 && !wrong;
    std::wcout << L"fixture id=" << id << L" action=" << (right ? L"right" : L"left") << L" callbacks=" << callbacks << L" completion=" << g_completion
               << L" passed=" << passed << L'\n';
    return passed;
}

bool TestFixtureBackgroundOnly(HWND window, const wchar_t* path, const wchar_t* guidTip)
{
    TrayModel model(window, kResult);
    model.Refresh();
    WaitForModelLoaded(model, 5000);
    g_fixtureRespond = false;
    g_completion = -1;
    g_fixtureEvents.clear();
    bool clicked = false;
    for (const auto& icon : model.Icons()) {
        if (icon.tip == guidTip && _wcsicmp(icon.exe.c_str(), path) == 0) {
            model.Click(icon, false);
            clicked = true;
            break;
        }
    }
    if (clicked)
        WaitForCompletion(5000);
    const bool quiet = g_completion == 4 && !g_fixtureEvents.empty();
    std::wcout << L"fixture background-only completion=" << g_completion << L" passed=" << quiet << L'\n';
    g_fixtureRespond = true;
    return quiet;
}

bool RunFixtureClicks(HWND window, const WNDCLASSW& wc, const wchar_t* path, const wchar_t* uidTip, const wchar_t* guidTip, TrayModel& probe)
{
    g_fixtureView = CreateWindowW(wc.lpszClassName, L"Atrium tray interaction test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 360, 180, nullptr,
                                  nullptr, wc.hInstance, nullptr);
    // Explicit manual test: materialize the native popup, then use the production worker.
    bool ok = g_fixtureView && OpenOverflow(probe);
    std::wcout << L"fixture popup prepared=" << ok << '\n';
    if (ok) {
        TrayModel model(window, kResult);
        for (UINT id : {4242u, 4243u})
            for (bool right : {false, true})
                ok = TestSingleFixtureClick(model, path, id == 4243 ? guidTip : uidTip, id, right) && ok;
    }
    ok = TestFixtureBackgroundOnly(window, path, guidTip) && ok;
    if (g_fixtureView) {
        DestroyWindow(g_fixtureView);
        g_fixtureView = nullptr;
    }
    return ok;
}

int RunFixture(bool interactions)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = ProbeProc;
    wc.lpszClassName = L"AtriumTrayFixture";
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    struct Fixture {
        NOTIFYICONDATAW uid{}, guid{};
        Fixture() = default;
        Fixture(const Fixture&) = delete;
        Fixture& operator=(const Fixture&) = delete;
        ~Fixture()
        {
            Shell_NotifyIconW(NIM_DELETE, &uid);
            Shell_NotifyIconW(NIM_DELETE, &guid);
        }
    } fixture;
    auto& uid = fixture.uid;
    auto& guid = fixture.guid;
    uid.cbSize = sizeof(uid);
    uid.hWnd = window;
    uid.uID = 4242;
    uid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    uid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    uid.uCallbackMessage = WM_APP + 43;
    wcscpy_s(uid.szTip, L"tray_live_probe - fixture UID");
    guid = uid;
    guid.uID = 4243;
    guid.uFlags |= NIF_GUID;
    CLSIDFromString(L"{DCA21D39-29A7-4E50-82A9-D8CA9B6A29F9}", &guid.guidItem);
    wcscpy_s(guid.szTip, L"tray_live_probe - fixture GUID");
    if (interactions)
        guid.uCallbackMessage = WM_APP + 44;
    if (!Shell_NotifyIconW(NIM_ADD, &uid) || !Shell_NotifyIconW(NIM_ADD, &guid)) {
        DestroyWindow(window);
        return 1;
    }
    for (const auto& nid : {uid, guid}) {
        NOTIFYICONIDENTIFIER identifier{sizeof(identifier), nid.hWnd, nid.uID, nid.guidItem};
        RECT bounds{};
        std::wcout << L"fixture getrect id=" << nid.uID << L" hr=" << std::hex << Shell_NotifyIconGetRect(&identifier, &bounds) << std::dec << L'\n';
    }
    if (interactions) {
        guid.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &guid)) {
            DestroyWindow(window);
            return 1;
        }
    }
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    TrayModel probe(window, kResult);
    if (interactions)
        std::wcout << L"fixture materialize=" << OpenOverflow(probe) << L'\n';
    bool ok = ExpectFixtureIcons(probe, path, 2);
    if (interactions && ok)
        ok = RunFixtureClicks(window, wc, path, uid.szTip, guid.szTip, probe);
    Shell_NotifyIconW(NIM_DELETE, &uid);
    ok = ExpectFixtureIcons(probe, path, 1) && ok;
    Shell_NotifyIconW(NIM_DELETE, &guid);
    ok = ExpectFixtureIcons(probe, path, 0) && ok;
    DestroyWindow(window);
    return ok ? 0 : 1;
}

void DumpSnapshot()
{
    const auto begin = GetTickCount64();
    std::vector<TrayIcon> snapshot;
    {
        TrayModel model(nullptr, 0);
        model.Refresh();
        WaitForModelLoaded(model, 10000);
        snapshot = model.Icons();
    }
    const auto buttonCount = std::ranges::count(snapshot, false, &TrayIcon::fromRecord);
    std::wcout << L"buttons=" << buttonCount << L" reconciled=" << snapshot.size() << L" matchMs=" << GetTickCount64() - begin << L" taskbarVisible="
               << IsWindowVisible(FindWindowW(L"Shell_TrayWnd", nullptr)) << L'\n';
    for (size_t i = 0; i < snapshot.size(); ++i)
        std::wcout << i << L" source=" << (snapshot[i].fromRecord ? L"record" : L"uia") << L" match=" << snapshot[i].matchMethod << L" identity="
                   << snapshot[i].recordId << L" exe=" << (snapshot[i].exe.empty() ? L"?" : snapshot[i].exe) << L'\n';
}

int RunTree(IUIAutomation* uia)
{
    HWND overflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
    for (HWND host : {FindWindowW(L"Shell_TrayWnd", nullptr), overflow ? overflow : FindWindowW(L"NotifyIconOverflowWindow", nullptr)}) {
        if (HWND bridge = FindWindowExW(host, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr))
            host = bridge;
        winrt::com_ptr<IUIAutomationElement> root;
        winrt::com_ptr<IUIAutomationCondition> all;
        winrt::com_ptr<IUIAutomationElementArray> elements;
        uia->CreateTrueCondition(all.put());
        if (!host || FAILED(uia->ElementFromHandle(host, root.put())) || !root || FAILED(root->FindAll(TreeScope_Descendants, all.get(), elements.put())) ||
            !elements)
            continue;
        int count = 0;
        elements->get_Length(&count);
        for (int i = 0; i < count; ++i) {
            winrt::com_ptr<IUIAutomationElement> e;
            elements->GetElement(i, e.put());
            if (!e)
                continue;
            BSTR cls = nullptr, id = nullptr;
            BOOL offscreen = TRUE;
            RECT r{};
            e->get_CurrentClassName(&cls);
            e->get_CurrentAutomationId(&id);
            e->get_CurrentIsOffscreen(&offscreen);
            e->get_CurrentBoundingRectangle(&r);
            std::wcout << L"class=" << (cls ? cls : L"") << L" id=" << (id ? id : L"") << L" offscreen=" << offscreen << L" rect=" << r.left << L"," << r.top
                       << L"," << r.right << L"," << r.bottom << L'\n';
            SysFreeString(cls);
            SysFreeString(id);
        }
    }
    return 0;
}

int RunClick(const std::wstring& mode, const wchar_t* exePath, const std::wstring& action)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = ProbeProc;
    wc.lpszClassName = L"AtriumTrayProbe";
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    int result = 2;
    {
        TrayModel model(window, kResult);
        model.Refresh();
        WaitForModelLoaded(model, 10000);
        const auto icons = model.Icons();
        const TrayIcon* chosen = nullptr;
        for (const auto& icon : icons) {
            if (_wcsicmp(icon.exe.c_str(), exePath) == 0) {
                if (chosen) {
                    std::wcerr << L"Ambiguous executable; refusing to choose an icon\n";
                    DestroyWindow(window);
                    return 2;
                }
                chosen = &icon;
            }
        }
        if (!chosen) {
            std::wcerr << L"No live icon; no application was launched\n";
        } else {
            if (mode == L"--visible-click")
                std::wcout << L"prepared=" << OpenOverflow(model) << L'\n';
            if (action == L"double")
                model.DoubleClick(*chosen);
            else
                model.Click(*chosen, action == L"right");
            WaitForCompletion(10000);
            std::wcout << L"completion=" << g_completion << L" (1=observed,2=system-popup,3=needs-user,4=accepted-unconfirmed,5=explicit-popup)\n";
            result = g_completion == 1 ? 0 : 1;
        }
    }
    DestroyWindow(window);
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IUIAutomation> uia;
    winrt::check_hresult(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put())));
    if (argc == 2 && std::wstring(argv[1]) == L"--watch")
        return RunWatch();
    if (argc == 2 && (std::wstring(argv[1]) == L"--fixture" || std::wstring(argv[1]) == L"--fixture-clicks"))
        return RunFixture(std::wstring(argv[1]) == L"--fixture-clicks");
    DumpSnapshot();
    if (argc == 1)
        return 0; // read-only by default
    if (argc == 2 && std::wstring(argv[1]) == L"--tree")
        return RunTree(uia.get());
    if (argc != 4 || (std::wstring(argv[1]) != L"--click" && std::wstring(argv[1]) != L"--visible-click") ||
        (std::wstring(argv[3]) != L"left" && std::wstring(argv[3]) != L"right" && std::wstring(argv[3]) != L"double")) {
        std::wcerr << L"usage: tray_live_probe [--click FULL_EXE_PATH left|right|double]\n";
        return 2;
    }
    return RunClick(argv[1], argv[2], argv[3]);
}
