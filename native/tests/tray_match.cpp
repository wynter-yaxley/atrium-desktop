#include "shell/tray_match.h"
#include <windows.h>
#include <iostream>

using namespace atrium::tray;

namespace {
bool g_failed = false;

void Check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << name << '\n';
        g_failed = true;
    }
}
Record R(const std::wstring& name, size_t order, bool promoted = false)
{
    Record r;
    r.id = std::to_wstring(order);
    r.exe = name + L".exe";
    r.tip = name;
    r.identity = L"guid:" + name;
    r.live = true;
    r.promoted = promoted;
    r.order = order;
    r.names = {name};
    r.bounds = {900, 1000, 920, 1020};
    return r;
}
void TestMojibakeAndKnownShell()
{
    Check(IsShellExplorerExe(L"C:\\Windows\\explorer.exe") && IsShellExplorerExe(L"EXPLORER.EXE"), "explorer exe detection");
    Check(!IsShellExplorerExe(L"C:\\Tools\\myexplorer.exe") && !IsShellExplorerExe(L""), "non-explorer exe rejected");
    const auto bt = KnownShellTray(L"{6DA68F06-00F1-4E6E-8158-7F1FFD4F9DB9}");
    Check(bt && std::wstring(bt->iconDll) == L"bthprops.cpl" && bt->aliases.size() == 3, "known bluetooth shell tray");
    const auto usb = KnownShellTray(L"guid:{7820ae78-23e3-4229-82c1-e41cb67d5b9c}");
    Check(usb && std::wstring(usb->iconDll) == L"hotplug.dll" && usb->aliases.size() == 2, "known safely remove shell tray");
    const auto sec = KnownShellTray(L"{BA82E2DC-F405-47AD-B032-CF0FAA0E3933}");
    Check(sec && sec->iconDll == nullptr && sec->aliases.size() == 2 && sec->aliases[0] == L"Windows 安全中心" && sec->aliases[1] == L"Windows Security",
          "known windows security shell tray");
    const auto secLower = KnownShellTray(L"guid:{ba82e2dc-f405-47ad-b032-cf0faa0e3933}");
    Check(secLower.has_value() && sec.has_value() && secLower->iconDll == nullptr && secLower->aliases == sec->aliases,
          "known windows security lowercase guid");
    Check(!KnownShellTray(L"{00000000-0000-0000-0000-000000000000}"), "unknown guid returns nullopt");

    for (const std::wstring& clean : {std::wstring(L"蓝牙设备"), std::wstring(L"安全删除硬件并弹出媒体"), std::wstring(L"模式"),
                                      std::wstring(L"Steam - syncing"), std::wstring(L"100% ↑↓ •"), std::wstring()})
        Check(RepairMojibakeTip(clean) == clean, "clean UTF-16 and ASCII tooltips stay untouched");

    const UINT acp = GetACP();
    if (acp != CP_UTF8) {
        const std::wstring expected = L"VPN - 中文测试";
        const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, expected.data(), int(expected.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size_t(utf8Len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, expected.data(), int(expected.size()), utf8.data(), utf8Len, nullptr, nullptr);
        const int wLen = MultiByteToWideChar(acp, MB_ERR_INVALID_CHARS, utf8.data(), utf8Len, nullptr, 0);
        if (wLen > 0) {
            std::wstring mojibake(size_t(wLen), L'\0');
            if (MultiByteToWideChar(acp, MB_ERR_INVALID_CHARS, utf8.data(), utf8Len, mojibake.data(), wLen) == wLen) {
                const int rtLen = WideCharToMultiByte(acp, WC_NO_BEST_FIT_CHARS, mojibake.data(), wLen, nullptr, 0, nullptr, nullptr);
                std::string rt(size_t(rtLen), '\0');
                BOOL usedDef = FALSE;
                if (rtLen == utf8Len && WideCharToMultiByte(acp, WC_NO_BEST_FIT_CHARS, mojibake.data(), wLen, rt.data(), rtLen, nullptr, &usedDef) == rtLen &&
                    !usedDef && rt == utf8)
                    Check(RepairMojibakeTip(mojibake) == expected, "UTF-8-as-ACP mojibake Chinese+English round-trip repaired");
            }
        }
    }
}
void TestOrderAndDedup()
{
    auto rWork = R(L"WorkBuddy", 0);
    auto result = Match({{L"Tencent WorkBuddy (Online)", false}}, {rWork});
    Check(result.items.size() == 1 && result.items[0].record == 0 && result.items[0].method == Method::Name, "delimited contains score 60 match");

    auto a = R(L"Alpha", 0), b = R(L"Beta", 1), c = R(L"Charlie", 2);
    result = Match({{L"Alpha", false}, {L"Custom Tip", false}, {L"Charlie", false}}, {a, b, c});
    Check(result.items.size() == 3 && result.items[1].record == 1 && result.items[1].method == Method::Order, "remainder Order after partial Name anchors");

    auto top = R(L"TopApp", 0, true);
    result = Match({{L"TopApp", false}, {L"X1", false}, {L"X2", false}}, {top, a, b});
    Check(result.items.size() == 3 && result.items[1].record == 1 && result.items[2].record == 2 && result.items[1].method == Method::Order,
          "remainder Order ignores cross-partition anchor");

    auto wb = R(L"WorkBuddy", 0);
    wb.order = kNone;
    result = Match({{L"", false}}, {wb});
    Check(result.items.size() == 1 && result.items[0].button == 0 && result.items[0].record == 0,
          "single empty button attaches single unmatched record instead of gray disc");

    std::vector<HostedEntry> hosted{
        {L"hwnd:1:1", L"c:\\vpn\\vpn.exe", L"VPN - Connected", L"VPN Client", L"VPN App", 1, true, 10},
        {L"hwnd:2:2", L"", L"NVIDIA Settings", L"", L"", 2, true},
        {L"hwnd:3:3", L"c:\\app\\workbuddy.exe", L"", L"WorkBuddy", L"Tencent WorkBuddy", 3, true},
    };
    std::vector<ExplorerEntry> explorer{
        {L"uid:10:1:1", L"c:\\vpn\\vpn.exe", L"VPN - Connected", true},
        {L"uid:10:9:9", L"c:\\vpn\\vpn.exe", L"VPN - Idle", true},
        {L"guid:{11111111-1111-1111-1111-111111111111}", L"c:\\vpn\\vpn.exe", L"VPN Helper", true},
        {L"uid:20:2:2", L"c:\\nvidia\\nv.exe", L"NVIDIA Settings - GPU", true},
        {L"", L"", L"WorkBuddy - Online", false},
        {L"", L"", L"Tencent WorkBuddy - Status", false},
        {L"", L"", L"", false},
        {L"guid:{22222222-2222-2222-2222-222222222222}", L"c:\\other\\other.exe", L"Other", true},
    };
    const auto merged = MergeHostedIcons(hosted, explorer);
    Check(merged.hostedFallback[0] == 0, "hosted VPN paired by uid");
    Check(merged.hostedFallback[1] == 3, "elevated empty-exe hosted paired by delimited-prefix tip");
    Check(merged.hostedFallback[2] == 4, "empty-exe explorer paired against hosted fallbackTip");
    Check(merged.extraExplorer.size() == 2 && merged.extraExplorer[0] == 2 && merged.extraExplorer[1] == 7,
          "suppresses duplicate VPN uid, ghost empty tip, productName gray disc, and keeps distinct unpaired GUID and unrelated app");
}
void TestAttachRemainingAndMultiInstance()
{
    auto r1 = R(L"DupApp", kNone);
    r1.id = L"r1";
    r1.identity = L"uid:10:1:0";
    r1.bounds = {10, 10, 30, 30};
    auto r2 = R(L"DupApp", kNone);
    r2.id = L"r2";
    r2.identity = L"uid:20:2:0";
    r2.bounds = {40, 40, 60, 60};

    auto res = Match({{L"DupApp", false}}, {r1, r2});
    Check(res.items.size() == 3 && res.items[0].button == 0 && res.items[0].record == kNone && res.items[1].method == Method::Record &&
              res.items[2].method == Method::Record,
          "AttachRemaining refuses to match when two unused live records tie on one button");

    res = Match({{L"Geom", false, r1.bounds}, {L"DupApp", false}, {L"DupApp", true}}, {r1, r2});
    Check(res.items.size() == 3 && res.items[0].record == 0 && res.items[0].method == Method::Geometry && res.items[1].record == 1 &&
              res.items[1].method == Method::Name && res.items[2].record == kNone,
          "AttachRemaining matches remaining record once tying peer is already used");

    auto rTop = r1;
    rTop.promoted = true;
    res = Match({{L"DupApp", true}, {L"Other", false}}, {rTop, r2});
    Check(res.items[0].record == 0 && res.items[0].method == Method::Name, "AttachRemaining matches unique record in same promoted partition");

    const std::wstring multiExe = L"c:\\app\\multi.exe";
    std::vector<HostedEntry> hostedHwnd{
        {L"hwnd:0000000000001000:0", multiExe, L"", L"", L"", 0, true, 100},
        {L"hwnd:0000000000002000:0", multiExe, L"", L"", L"", 0, true, 200},
    };
    std::vector<ExplorerEntry> explorerHwnd{
        {L"uid:200:8192:0", multiExe, L"Instance B", true},
        {L"uid:100:4096:0", multiExe, L"Instance A", true},
    };
    const auto mergedHwnd = MergeHostedIcons(hostedHwnd, explorerHwnd);
    Check(mergedHwnd.hostedFallback[0] == 1 && mergedHwnd.hostedFallback[1] == 0 && mergedHwnd.extraExplorer.empty(),
          "MergeHostedIcons pairs same-exe uID=0 instances by exact (hwnd, uid) in Tier 0");

    std::vector<HostedEntry> hostedTip{
        {L"hwnd:100:0", multiExe, L"Alpha Profile", L"", L"", 0, true, 100},
        {L"hwnd:200:0", multiExe, L"Beta Profile", L"", L"", 0, true, 200},
    };
    std::vector<ExplorerEntry> explorerTip{
        {L"uid:200:999:0", multiExe, L"Beta Profile", true},
        {L"uid:100:888:0", multiExe, L"Alpha Profile", true},
    };
    const auto mergedTip = MergeHostedIcons(hostedTip, explorerTip);
    Check(mergedTip.hostedFallback[0] == 1 && mergedTip.hostedFallback[1] == 0 && mergedTip.extraExplorer.empty(),
          "MergeHostedIcons defers colliding bare uid to Tier 2 tooltip match");

    std::vector<HostedEntry> hostedPartial{
        {L"hwnd:1000:0", multiExe, L"Other Tip", L"", L"", 0, true, 100},
        {L"hwnd:200:0", multiExe, L"Remaining", L"", L"", 0, true, 200},
    };
    std::vector<ExplorerEntry> explorerPartial{
        {L"uid:200:999:0", multiExe, L"Different Tip", true},
        {L"uid:100:4096:0", multiExe, L"Alpha", true},
    };
    const auto mergedPartial = MergeHostedIcons(hostedPartial, explorerPartial);
    Check(mergedPartial.hostedFallback[0] == 1 && mergedPartial.hostedFallback[1] == 0,
          "MergeHostedIcons allows Tier 1 bare uid once prior tiers leave a unique unpaired uid");

    const std::wstring clientExe = L"c:\\app\\client.exe";
    std::vector<HostedEntry> hostedPid{
        {L"hwnd:1000:1", clientExe, L"Client A", L"", L"", 1, true, 100},
    };
    std::vector<ExplorerEntry> explorerPid{
        {L"uid:100:4096:1", clientExe, L"Client A", true},
        {L"uid:100:4096:2", clientExe, L"Client A Extra", true},
        {L"uid:200:8192:1", clientExe, L"Client B", true},
        {L"", clientExe, L"Legacy Ghost", true},
    };
    const auto mergedPid = MergeHostedIcons(hostedPid, explorerPid);
    Check(mergedPid.hostedFallback[0] == 0 && mergedPid.extraExplorer.size() == 1 && mergedPid.extraExplorer[0] == 2,
          "MergeHostedIcons preserves distinct-PID uid icon of same exe while suppressing same-PID and pid=0 duplicates");
}
void TestElevatedGuidAndConcatenatedTips()
{
    std::vector<HostedEntry> hostedGuid{
        {L"guid:{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}", L"", L"Elevated Service", L"", L"", 0, false, 0},
        {L"uid:1:2:3", L"", L"Should Not Match Non-Guid", L"", L"", 3, true, 1},
    };
    std::vector<ExplorerEntry> explorerGuid{
        {L"guid:{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}", L"c:\\svc\\svc.exe", L"Different Tip", true},
        {L"uid:1:2:3", L"c:\\other\\other.exe", L"Other", true},
        {L"guid:{99999999-9999-9999-9999-999999999999}", L"", L"Unmatched Guid", true},
    };
    const auto mergedGuid = MergeHostedIcons(hostedGuid, explorerGuid);
    Check(mergedGuid.hostedFallback[0] == 0, "T1: empty-exe guid: hosted matches Explorer guid: record in Tier 0");
    Check(mergedGuid.hostedFallback[1] == kNone, "T1: non-guid recordId still requires sameExe in Tier 0");
    Check(mergedGuid.extraExplorer.size() == 2 && mergedGuid.extraExplorer[0] == 1 && mergedGuid.extraExplorer[1] == 2,
          "T1: guid: dedup suppresses matched guid: without false-suppressing unrelated icons");

    std::vector<HostedEntry> hostedSeg{
        {L"hwnd:10:1", L"c:\\windows\\system32\\securityhealthsystray.exe", L"Windows 安全中心", L"", L"", 1, true, 10},
        {L"hwnd:20:1", L"", L"OpenVPN", L"", L"", 1, true, 20},
    };
    std::vector<ExplorerEntry> explorerSeg{
        {L"", L"", L"系统托盘；Windows 安全中心。无需操作", false},
        {L"", L"", L"Tray - OpenVPN; Connected", false},
    };
    const auto mergedSeg = MergeHostedIcons(hostedSeg, explorerSeg);
    Check(mergedSeg.hostedFallback[0] == 0 && mergedSeg.hostedFallback[1] == 1 && mergedSeg.extraExplorer.empty(),
          "T2: Win11 concatenated multi-segment tooltips pair and claim unknown in Tier 5");

    const auto mergedShort =
        MergeHostedIcons({{L"hwnd:1:1", L"", L"Open", L"", L"", 1, true, 1}}, {{L"", L"c:\\other\\other.exe", L"Tray - Open; Ready", false}});
    Check(mergedShort.hostedFallback[0] == kNone, "T2: empty-exe short 4-char ASCII tip does not mispair in Tier 5");

    std::vector<HostedEntry> hostedCode{
        {L"hwnd:100:1", L"c:\\apps\\code.exe", L"VS Code", L"", L"Code", 1, true, 100},
    };
    std::vector<ExplorerEntry> explorerCode{
        {L"uid:100:100:1", L"c:\\apps\\code.exe", L"VS Code", true},
        {L"", L"", L"QR Code Generator", false},
    };
    const auto mergedCode = MergeHostedIcons(hostedCode, explorerCode);
    Check(mergedCode.hostedFallback[0] == 0 && mergedCode.extraExplorer.size() == 1 && mergedCode.extraExplorer[0] == 1,
          "T2 negative: code.exe does not falsely claim unrelated 'QR Code Generator' button");

    Record secRec;
    secRec.id = L"sec";
    secRec.exe = L"c:\\windows\\system32\\securityhealthsystray.exe";
    secRec.identity = L"guid:{BA82E2DC-F405-47AD-B032-CF0FAA0E3933}";
    secRec.live = true;
    secRec.order = 0;
    secRec.names = {L"", L"Windows Security notification icon", L"Windows Security", L"SecurityHealthSystray", L"Windows 安全中心", L"Windows Security"};
    secRec.firstTitle = secRec.names.size();
    const auto secMatch = Match({{L"系统状态；Windows 安全中心。无需操作", false}}, {secRec});
    Check(secMatch.items.size() == 1 && secMatch.items[0].record == 0 && secMatch.items[0].method == Method::Name,
          "T5: KnownShellTray Windows Security alias matches concatenated UIA tooltip");
}
void TestNvidiaContainerIcons()
{
    const auto dm = KnownShellTray(L"{7120A42D-CB02-4A51-B3B5-58E36E8B14E1}");
    const auto dmLegacy = KnownShellTray(L"guid:{d68540cc-e0d8-4298-b55c-e6e324121f77}");
    const auto gpu = KnownShellTray(L"guid:{d36b22a6-995e-4340-8339-3de08c6840df}");
    Check(dm && dmLegacy && dm->iconDll == nullptr && dm->aliases == dmLegacy->aliases && dm->aliases.front() == L"管理显示模式",
          "KnownShellTray maps NVIDIA Manage Display Mode GUIDs");
    Check(gpu && gpu->iconDll == nullptr && gpu->aliases.front() == L"NVIDIA GPU 活动", "KnownShellTray maps NVIDIA GPU Activity GUID");

    auto makeNvRecord = [](const std::wstring& id, const std::wstring& exe, const std::wstring& tip, const std::wstring& identity, size_t order) {
        Record r;
        r.id = id;
        r.exe = exe;
        r.tip = tip;
        r.identity = identity;
        r.live = true;
        r.order = order;
        r.names = {tip, L"NVIDIA Container", L"NVIDIA Container", L"nvcontainer"};
        if (const auto known = KnownShellTray(identity))
            r.names.insert(r.names.end(), known->aliases.begin(), known->aliases.end());
        r.firstTitle = r.names.size();
        return r;
    };
    const Record settings =
        makeNvRecord(L"16133554932753985167", L"c:\\windows\\system32\\driverstore\\nvdisplay.container.exe", L"NVIDIA 设置", L"uid:4560:1234:1051", 94);
    const Record displayMode = makeNvRecord(L"4834343228493306829", L"c:\\program files\\nvidia corporation\\nvcontainer\\nvcontainer.exe", L"",
                                            L"guid:{7120A42D-CB02-4A51-B3B5-58E36E8B14E1}", 100);
    const Record gpuActivity = makeNvRecord(L"13289861806036237432", L"c:\\program files\\nvidia corporation\\nvcontainer\\nvcontainer.exe", L"",
                                            L"guid:{D36B22A6-995E-4340-8339-3DE08C6840DF}", 114);
    Check(FallbackRecordTip(displayMode) == L"管理显示模式", "FallbackRecordTip uses Manage Display Mode alias instead of NVIDIA Container");
    Check(FallbackRecordTip(gpuActivity) == L"NVIDIA GPU 活动", "FallbackRecordTip uses GPU Activity alias instead of NVIDIA Container");

    const auto zh = Match(
        {
            {L"NVIDIA 设置", false},
            {L"管理显示模式", false},
            {L"NVIDIA GeForce RTX 4060 Laptop GPU（0 个显示器，3 个程序）  NVIDIA GeForce RTX 4060 Laptop GPU（0 个显示器，17 个程序）", false},
        },
        {settings, displayMode, gpuActivity});
    Check(zh.items.size() == 3 && zh.items[0].record == 0 && zh.items[1].record == 1 && zh.items[2].record == 2 && zh.items[0].method == Method::Name &&
              zh.items[1].method == Method::Name && zh.items[2].method == Method::Name,
          "all 3 Chinese NVIDIA tray buttons pair with their live records with zero duplicate ghosts");

    Record enSettings = settings;
    enSettings.tip = L"NVIDIA Settings";
    enSettings.names[0] = L"NVIDIA Settings";
    const auto en = Match(
        {
            {L"NVIDIA Settings", false},
            {L"Manage Display Mode", false},
            {L"NVIDIA RTX A2000 (1 display, 2 programs)", false},
        },
        {enSettings, displayMode, gpuActivity});
    Check(en.items.size() == 3 && en.items[0].record == 0 && en.items[1].record == 1 && en.items[2].record == 2 && en.items[0].method == Method::Name &&
              en.items[1].method == Method::Name && en.items[2].method == Method::Name,
          "English NVIDIA tray buttons match cleanly without colliding with NVIDIA Settings");
}
void TestHostedTargetVsFallbackSeparation()
{
    // Strict Tier 0/1/2 matches grant hostedTarget when identity fields (GUID or non-zero PID/HWND/UID) agree;
    // conflicting HWND, Tier 3..6, and FallbackHostedMatch only populate hostedFallback.
    const std::vector<HostedEntry> hosted{
        {L"guid:{AAAAAAAA-1111-2222-3333-444444444444}", L"c:\\app\\strict_guid.exe", L"Strict GUID", L"", L"", 0, false, 10},
        {L"hwnd:100:1", L"c:\\app\\strict_uid.exe", L"Alpha", L"", L"", 1, true, 20},
        {L"hwnd:6f:11", L"c:\\app\\matched_uid.exe", L"Matched Hwnd", L"", L"", 11, true, 21},
        {L"hwnd:200:2", L"c:\\app\\mismatch.exe", L"Mismatch", L"", L"", 2, true, 30},
        {L"hwnd:300:3", L"", L"Elevated Tip", L"", L"", 3, true, 40},
        {L"hwnd:400:4", L"c:\\app\\shared.exe", L"Primary Icon", L"", L"", 4, true, 50},
        {L"hwnd:500:5", L"c:\\app\\shared.exe", L"Secondary Icon", L"", L"", 5, true, 50},
    };
    const std::vector<ExplorerEntry> explorer{
        {L"guid:{AAAAAAAA-1111-2222-3333-444444444444}", L"c:\\app\\strict_guid.exe", L"Different Tip", true},
        {L"uid:20:999:1", L"c:\\app\\strict_uid.exe", L"Different Alpha", true},
        {L"uid:21:111:11", L"c:\\app\\matched_uid.exe", L"Different Hwnd Tip", true},
        {L"uid:30:888:99", L"c:\\app\\mismatch.exe", L"Mismatch", true},
        {L"uid:40:777:3", L"c:\\app\\elevated.exe", L"Elevated Tip", true},
        {L"uid:50:666:9", L"c:\\app\\shared.exe", L"Shared Explorer Tip", true},
    };
    const auto merged = MergeHostedIcons(hosted, explorer);
    Check(merged.hostedFallback[0] == 0 && merged.hostedTarget[0] == 0, "Tier 0 GUID match sets both hostedFallback and hostedTarget");
    Check(merged.hostedFallback[1] == 1 && merged.hostedTarget[1] == kNone,
          "Tier 1 unique uid with conflicting HWND keeps hostedFallback but rejects hostedTarget");
    Check(merged.hostedFallback[2] == 2 && merged.hostedTarget[2] == 2,
          "Tier 1 unique same-exe uid match with agreeing HWND sets both hostedFallback and hostedTarget");
    Check(merged.hostedFallback[3] == 3 && merged.hostedTarget[3] == kNone,
          "Tier 2 tip match with conflicting uid keeps hostedFallback but rejects hostedTarget");
    Check(merged.hostedFallback[4] == 4 && merged.hostedTarget[4] == kNone, "Tier 3 empty-exe tip match borrows hostedFallback without granting hostedTarget");
    Check(merged.hostedFallback[5] == 5 && merged.hostedTarget[5] == kNone, "Tier 6 same-exe fallback borrows hostedFallback without granting hostedTarget");
    Check(merged.hostedFallback[6] == 5 && merged.hostedTarget[6] == kNone, "shared FallbackHostedMatch borrows hostedFallback without granting hostedTarget");
    Check(merged.extraExplorer.empty(), "matched/suppressed explorer entries leave no extras");

    // Two tray windows in one process using the same uID=1 on incomplete sides never grant cross-window hostedTarget.
    const auto crossHwnd = MergeHostedIcons({{L"hwnd:100:1", L"c:\\app\\dual.exe", L"Account A", L"", L"", 1, true, 70}},
                                            {{L"uid:70:200:1", L"c:\\app\\dual.exe", L"Account B", true}});
    Check(crossHwnd.hostedFallback[0] == 0 && crossHwnd.hostedTarget[0] == kNone,
          "two tray windows in same exe with same uID never grant cross-window hostedTarget");

    // A hwnd: hosted icon paired with a guid: Explorer record in Tier 2 must not claim hostedTarget.
    const auto guidOnly = MergeHostedIcons({{L"hwnd:10:1", L"c:\\app\\x.exe", L"X Tip", L"", L"", 1, true, 10}},
                                           {{L"guid:{BBBBBBBB-1111-2222-3333-444444444444}", L"c:\\app\\x.exe", L"X Tip", true}});
    Check(guidOnly.hostedFallback[0] == 0 && guidOnly.hostedTarget[0] == kNone,
          "hwnd hosted icon paired to guid Explorer entry in Tier 2 never grants hostedTarget");
}
} // namespace

int main()
{
    auto steam = R(L"Steam", 0, true), qq = R(L"QQ", 1), tailscale = R(L"Tailscale", 2);
    steam.bounds = {20, 20, 40, 40};
    std::vector<Record> rs{steam, qq, tailscale};
    auto result = Match({{L"Steam", true, steam.bounds}}, rs);
    Check(result.items.size() == 3 && result.items[1].button == kNone, "unopened overflow supplements live records");
    Check(result.sharedPlace.size() >= 3 && result.sharedPlace[1] && result.sharedPlace[2], "different exe sharedPlace regression");
    result = Match({{L"Steam", true, steam.bounds}, {L"QQ", false}, {L"Tailscale", false}}, rs);
    Check(result.items.size() == 3 && result.items[2].record == 2, "opened overflow has no duplicates");
    result = Match({{L"QQ", false}}, rs);
    Check(result.items.size() == 3, "partial overflow still supplements missing identities");
    auto nvidia = R(L"NVIDIA", 0), second = nvidia;
    second.id = L"1";
    second.identity = L"uid:42:100:2";
    second.order = 1;
    result = Match({}, {nvidia, second});
    Check(result.items.size() == 2 && !result.sharedPlace.empty() && result.sharedPlace[0],
          "same exe GUID and UID survive shared rectangle and identical names");
    result = Match({{L"NVIDIA", false}, {L"NVIDIA", false}}, {nvidia, second});
    Check(result.items.size() == 2 && result.items[0].method == Method::Order, "same exe distinct icons use consistent full order");
    auto old = qq;
    old.id = L"old";
    old.order = 100;
    result = Match({}, {old, qq});
    Check(result.items.size() == 1 && result.items[0].record == 1 && result.aliases[0] == 1, "re-registration aliases deduplicate by live identity");
    old.identity = L"guid:obsolete";
    old.live = false;
    result = Match({}, {old, qq});
    Check(result.items.size() == 1, "dead registration never resurrects");
    qq.live = false;
    result = Match({{L"QQ", false}}, {qq});
    Check(result.items.size() == 1 && result.items[0].record == kNone, "stale tooltip cannot claim live UIA button");
    result = Match({{L"Steam - 正在同步", false}}, {}, {{L"steam.exe", {L"Steam"}}});
    Check(result.items.size() == 1 && result.items[0].method == Method::Running, "Windows 10 running program fallback and changing tooltip");
    result = Match({{L"", false}, {L"", false}}, {nvidia, second});
    Check(result.items.size() == 2 && result.items[1].record == 1, "empty tooltip complete ordering");
    result = Match({{L"QQ", false}}, {}, {{L"a.exe", {L"QQ"}}, {L"b.exe", {L"QQ"}}});
    Check(!result.items.empty() && result.items[0].program == kNone, "ambiguous program name is not ownership");
    result = Match({{L"Steam", false}}, {}, {{L"browser.exe", {L"Steam - website"}, 0}});
    Check(!result.items.empty() && result.items[0].program == kNone, "window title prefix cannot claim another app");
    for (const auto& tip : {L"SteamVR", L"SteamHelper", L"QQMusic"}) {
        result = Match({{tip, false}}, {}, {{L"steam.exe", {L"Steam"}}, {L"qq.exe", {L"QQ"}}});
        Check(!result.items.empty() && result.items[0].program == kNone, "program prefix needs a real status delimiter");
    }
    result = Match({{L"STEAM：正在同步", false}}, {}, {{L"steam.exe", {L"Steam"}}});
    Check(!result.items.empty() && result.items[0].program == 0, "localized status delimiter and case remain supported");
    auto unnamedOrder = R(L"Steam", kNone);
    result = Match({{L"SteamVR", false}}, {unnamedOrder});
    Check(!result.items.empty() && result.items[0].record == kNone, "registration name prefix also needs delimiter");
    auto a = R(L"Alpha", 0), b = R(L"Beta", 1);
    result = Match({{L"Beta", false}, {L"Alpha", false}}, {a, b});
    Check(result.items.size() == 2 && result.items[0].record == 1, "names override obsolete order");
    result = Match({{L"Alpha", false}, {L"当前线路：香港", false}}, {a, b});
    Check(result.items.size() == 2 && result.items[1].method == Method::Order, "fully changed tooltip uses consistent complete order");
    result = Match({{L"", false, a.bounds}}, {a, b});
    Check(!result.items.empty() && result.items[0].record == kNone, "shared rectangle never becomes unique after consumption");
    Check(Title(L" \r\n Steam \r\n status") == L"Steam", "tooltip first line");
    TestMojibakeAndKnownShell();
    TestOrderAndDedup();
    TestAttachRemainingAndMultiInstance();
    TestElevatedGuidAndConcatenatedTips();
    TestNvidiaContainerIcons();
    TestHostedTargetVsFallbackSeparation();
    if (g_failed)
        return 1;
    std::cout << "tray matching: all scenarios passed\n";
    return 0;
}
