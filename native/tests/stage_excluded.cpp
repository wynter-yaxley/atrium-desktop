#include "stage_excluded.h"

#include <iostream>

using namespace atrium;

int main()
{
    int failures = 0;
    auto run = [&](const char* name, auto test) {
        const char* failure = nullptr;
        auto require = [&](bool condition, const char* message) {
            if (!condition && !failure)
                failure = message;
        };
        test(require);
        if (!failure) {
            std::cout << "PASS: " << name << '\n';
        } else {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << failure << '\n';
        }
    };

    run("an executable name excludes every install path", [&](auto& require) {
        require(StageAppExcluded({L"wechat.exe"}, L"d:\\apps\\wechat\\wechat.exe"), "the file name must match a full app key");
        require(StageAppExcluded({L"wechat.exe"}, L"c:\\program files\\wechat\\wechat.exe"), "another install path must match too");
    });
    run("AUMIDs match by prefix", [&](auto& require) {
        require(StageAppExcluded({L"microsoft.screensketch_"}, L"microsoft.screensketch_8wekyb3d8bbwe!app"), "a package prefix must match its AUMID");
    });
    run("window keys match their app", [&](auto& require) {
        require(StageAppExcluded({L"wechat.exe"}, L"d:\\apps\\wechat\\wechat.exe|2a4"), "a per-window key must match by its app key");
        require(StageAppExcluded({L"microsoft.screensketch_"}, L"microsoft.screensketch_8wekyb3d8bbwe!app|2a4"),
                "a packaged window key must match its package prefix");
    });
    run("case does not matter",
        [&](auto& require) { require(StageAppExcluded({L"WeChat.EXE"}, L"D:\\Apps\\WeChat\\wechat.exe"), "entries and app keys compare without case"); });
    run("a full path entry matches exactly",
        [&](auto& require) { require(StageAppExcluded({L"d:\\apps\\tool.exe"}, L"d:\\apps\\tool.exe"), "a full key must match"); });
    run("folder prefixes require a trailing separator", [&](auto& require) {
        require(StageAppExcluded({L"c:\\games\\hero\\"}, L"c:\\games\\hero\\bin\\run.exe"), "a trailing-backslash folder matches apps inside it");
        require(StageAppExcluded({L"c:/games/hero/"}, L"c:/games/hero/bin/run.exe"), "a trailing-slash folder matches apps inside it");
        require(!StageAppExcluded({L"c:\\app"}, L"c:\\application\\a.exe"), "a non-folder non-underscore prefix must not match");
    });
    run("unrelated apps are not excluded", [&](auto& require) {
        require(!StageAppExcluded({L"wechat.exe"}, L"d:\\apps\\notepad.exe"), "no substring or unrelated match");
        require(!StageAppExcluded({}, L"d:\\apps\\wechat.exe"), "an empty list excludes nothing");
        require(!StageAppExcluded({L""}, L"d:\\apps\\wechat.exe"), "an empty entry is ignored");
    });
    run("the defaults cover the Windows screenshot tools", [&](auto& require) {
        const auto defaults = DefaultStageExcluded();
        require(StageAppExcluded(defaults, L"c:\\windows\\system32\\snippingtool.exe"), "Windows Snipping Tool");
        require(StageAppExcluded(defaults, L"c:\\windows\\system32\\screenclippinghost.exe"), "Screen Clipping Host");
        require(StageAppExcluded(defaults, L"microsoft.screensketch_8wekyb3d8bbwe!app"), "packaged Snipping Tool");
        require(!StageAppExcluded(defaults, L"d:\\apps\\wechat\\wechat.exe"), "other screenshot tools are opt-in");
        require(!StageAppExcluded(defaults, L"c:\\windows\\notepad.exe"), "ordinary apps stay managed");
    });
    run("non-ASCII letters compare as Windows compares paths", [&](auto& require) {
        require(StageAppExcluded({L"C:\\Apps\\\u00c9diteur\\APP.exe"}, L"c:\\apps\\\u00e9diteur\\app.exe"), "É and é are one letter in an app key");
    });
    return failures ? 1 : 0;
}
