#include "shell/launchpad_order.h"
#include "ui/launchpad_model.h"
#include "ui/launchpad_page_ids.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace atrium;

namespace {

int g_failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

bool Near(float a, float b, float eps = 0.01f) { return std::abs(a - b) <= eps; }

void TestGridLayout()
{
    const auto large = ComputeLaunchpadGrid(1920.0f, 1080.0f);
    Check(large.cols == 7 && large.rows == 5 && large.perPage == 35, "1920x1080 uses 7x5 grid");
    Check(Near(large.iconDip, 64.0f), "large grid uses 64 DIP icons");
    Check(Near(large.gridLeft * 2.0f + large.cellW * 7.0f, 1920.0f), "large grid is horizontally centered");
    Check(Near((large.gridTop - 96.0f) * 2.0f + large.cellH * 5.0f, 1080.0f - 96.0f - 64.0f),
          "large grid is vertically centered between top and bottom reserves");

    const auto boundary = ComputeLaunchpadGrid(1200.0f, 720.0f);
    Check(boundary.cols == 7 && boundary.rows == 5 && Near(boundary.iconDip, 64.0f), "1200x720 boundary uses 7x5 grid");

    const auto medium = ComputeLaunchpadGrid(1024.0f, 600.0f);
    Check(medium.cols == 6 && medium.rows == 4 && medium.perPage == 24, "1024x600 uses 6x4 grid");
    Check(Near(medium.iconDip, 56.0f), "medium grid uses 56 DIP icons");
    Check(Near(medium.gridLeft * 2.0f + medium.cellW * 6.0f, 1024.0f), "medium grid is horizontally centered");

    const auto compact = ComputeLaunchpadGrid(800.0f, 480.0f);
    Check(compact.cols == 5 && compact.rows == 4 && compact.perPage == 20, "800x480 uses 5x4 grid");
    Check(Near(compact.iconDip, 52.0f), "compact grid uses 52 DIP icons");
    Check(compact.gridTop >= 96.0f, "compact grid respects top search bar reserve");
}

void TestPaging()
{
    Check(PageCount(0, 35) == 0, "0 items yields 0 pages");
    Check(PageCount(10, 0) == 0, "non-positive perPage yields 0 pages");
    Check(PageCount(35, 35) == 1, "exact page size yields 1 page");
    Check(PageCount(36, 35) == 2, "overflow by 1 yields 2 pages");

    const auto s0 = PageSlice(40, 0, 35);
    Check(s0.begin == 0 && s0.end == 35, "first slice covers [0, 35)");
    const auto s1 = PageSlice(40, 1, 35);
    Check(s1.begin == 35 && s1.end == 40, "second slice covers [35, 40)");
    const auto sBad = PageSlice(40, 2, 35);
    Check(sBad.begin == 0 && sBad.end == 0, "out-of-range page yields empty slice");
    const auto sNeg = PageSlice(40, -1, 35);
    Check(sNeg.begin == 0 && sNeg.end == 0, "negative page yields empty slice");
}

void TestMoveSelection()
{
    int page = 0;
    Check(MoveSelection(-1, 1, 0, 0, 7, 5, page) == -1 && page == 0, "empty list has no selection");

    page = 1;
    Check(MoveSelection(-1, 1, 0, 50, 7, 5, page) == 35, "forward arrow from none selects first item of page");
    Check(MoveSelection(-1, -1, 0, 50, 7, 5, page) == 49, "backward arrow from none selects last item of page");

    page = 0;
    Check(MoveSelection(0, 1, 0, 50, 7, 5, page) == 1 && page == 0, "move right within row");
    Check(MoveSelection(1, 0, 1, 50, 7, 5, page) == 8 && page == 0, "move down within page");
    Check(MoveSelection(0, -1, 0, 50, 7, 5, page) == 0 && page == 0, "left edge on first page clamps");

    // Cross page boundary: col 6 row 1 (index 13) -> next page col 0 row 1 (35 + 7 = 42)
    Check(MoveSelection(13, 1, 0, 50, 7, 5, page) == 42 && page == 1, "right past last col advances to next page");
    // Cross back: col 0 row 1 on page 1 (index 42) -> prev page col 6 row 1 (index 13)
    Check(MoveSelection(42, -1, 0, 50, 7, 5, page) == 13 && page == 0, "left past first col returns to previous page");
    // Clamp on partial last page
    page = 1;
    Check(MoveSelection(48, 0, 1, 50, 7, 5, page) == 49 && page == 1, "moving down on partial last page clamps to last item");
}

void TestSettlePageDrag()
{
    Check(SettlePageDrag(0, 0, -0.5f, -2.0f) == 0, "empty page count settles to 0");
    Check(SettlePageDrag(1, 3, -0.20f, 0.0f) == 1, "small left drag snaps back");
    Check(SettlePageDrag(1, 3, 0.25f, 0.0f) == 1, "exactly 25% drag snaps back");
    Check(SettlePageDrag(1, 3, -0.26f, 0.0f) == 2, "left drag over 25% advances page");
    Check(SettlePageDrag(1, 3, 0.30f, 0.0f) == 0, "right drag over 25% goes to previous page");
    Check(SettlePageDrag(1, 3, -0.05f, -1.5f) == 2, "fast left fling advances page");
    Check(SettlePageDrag(1, 3, 0.05f, 1.5f) == 0, "fast right fling goes to previous page");
    Check(SettlePageDrag(0, 3, 0.8f, 2.0f) == 0, "clamps at first page");
    Check(SettlePageDrag(2, 3, -0.8f, -2.0f) == 2, "clamps at last page");
}

void TestTileMutations()
{
    std::vector<LaunchpadTile> tiles = {
        {L"app.a", L"A", false, {}},
        {L"app.b", L"B", false, {}},
        {L"app.c", L"C", false, {}},
        {L"app.d", L"D", false, {}},
    };

    Check(!ReorderTiles(tiles, 1, 1), "no-op reorder returns false");
    Check(!ReorderTiles(tiles, 0, 4), "out-of-range reorder returns false");
    Check(ReorderTiles(tiles, 0, 2), "forward reorder succeeds");
    Check(tiles[0].id == L"app.b" && tiles[1].id == L"app.c" && tiles[2].id == L"app.a" && tiles[3].id == L"app.d",
          "forward reorder places item at target index");
    Check(ReorderTiles(tiles, 3, 1), "backward reorder succeeds");
    Check(tiles[1].id == L"app.d" && tiles[2].id == L"app.c", "backward reorder shifts intermediate items");

    // Merge app.b (0) onto app.d (1) -> creates folder at index 0 after erasing 0
    Check(MergeIntoFolder(tiles, 0, 1, L"folder.1", L"New folder"), "merging two apps creates a folder");
    Check(tiles.size() == 3, "tile count decreases by 1 after merge");
    Check(tiles[0].folder && tiles[0].id == L"folder.1" && tiles[0].name == L"New folder", "created folder has expected id and name");
    Check(tiles[0].appIds.size() == 2 && tiles[0].appIds[0] == L"app.d" && tiles[0].appIds[1] == L"app.b", "created folder contains target then dragged app");

    // Merge app.a (2) into existing folder (0)
    Check(MergeIntoFolder(tiles, 2, 0, L"ignored", L"ignored"), "merging app into existing folder succeeds");
    Check(tiles.size() == 2 && tiles[0].appIds.size() == 3 && tiles[0].appIds[2] == L"app.a", "app is appended to existing folder");

    // Cannot merge a folder into another tile
    Check(!MergeIntoFolder(tiles, 0, 1, L"f2", L"F2"), "merging a folder is rejected");

    // Remove middle app from folder (still 2 apps -> remains folder)
    Check(RemoveFromFolder(tiles, 0, 1), "removing one of three apps succeeds");
    Check(tiles[0].folder && tiles[0].appIds.size() == 2, "folder with 2 apps remains a folder");

    // Remove another app -> dissolves back into single app tile
    Check(RemoveFromFolder(tiles, 0, 0), "removing down to 1 app succeeds");
    Check(!tiles[0].folder && tiles[0].id == L"app.a" && tiles[0].appIds.empty(), "folder with 1 remaining app dissolves into single app tile");
    Check(!RemoveFromFolder(tiles, 0, 0), "cannot remove from non-folder tile");
}

void TestExpandLaunchpadPageIds()
{
    const std::vector<CatalogApp> shown{
        {.name = L"Chrome", .id = L"Chrome"},
        {.name = L"Microsoft apps", .id = L"#microsoft"},
        {.name = L"System apps", .id = L"#system"},
    };
    const std::vector<CatalogApp> microsoft{
        {.name = L"Edge", .id = L"MSEdge"},
        {.name = L"Notepad", .id = L"Notepad"},
    };
    const std::vector<CatalogApp> system{
        {.name = L"Taskmgr", .id = L"Taskmgr"},
    };
    const auto ids = ExpandLaunchpadPageIds(shown, system, 0, 10, L"#system", microsoft, L"#microsoft");
    Check(ids.size() == 4 && ids[0] == L"Chrome" && ids[1] == L"MSEdge" && ids[2] == L"Notepad" && ids[3] == L"Taskmgr",
          "ExpandLaunchpadPageIds expands both #microsoft and #system folders");
    const std::vector<CatalogApp> custom{
        {.name = L"A1", .id = L"App1"},
        {.name = L"A2", .id = L"App2"},
    };
    const std::vector<CatalogApp> shownWithCustom{
        {.name = L"Tools", .id = L"folder:1"},
        {.name = L"Microsoft apps", .id = L"#microsoft"},
    };
    const auto expanded = ExpandLaunchpadPageIds(shownWithCustom, 0, 10, [&](std::wstring_view id) -> const std::vector<CatalogApp>* {
        if (id == L"folder:1")
            return &custom;
        if (id == L"#microsoft")
            return &microsoft;
        return nullptr;
    });
    Check(expanded.size() == 4 && expanded[0] == L"App1" && expanded[1] == L"App2" && expanded[2] == L"MSEdge",
          "ExpandLaunchpadPageIds expands custom folder:<n> entries");
}

void TestLaunchpadLayoutJson()
{
    LaunchpadLayout legacy;
    Check(ParseLaunchpadLayoutJson(LR"({"order":["A","B","#system"]})", legacy), "parses legacy order-only launchpad.json");
    Check(legacy.order.size() == 3 && legacy.folders.empty() && legacy.hidden.empty(), "legacy json leaves folders and hidden empty");

    LaunchpadLayout l;
    l.order = {L"folder:1", L"App3", L"AppHidden", L"App3"};
    l.folders = {
        {L"folder:1", L"Work", {L"App1", L"App2", L"AppHidden", L"folder:2", L"App1"}},
        {L"folder:2", L"DupOnly", {L"App2"}},
    };
    l.hidden = {L"AppHidden", L"", L"AppHidden"};
    l.Clamp();
    Check(l.hidden.size() == 1 && l.hidden[0] == L"AppHidden", "Clamp deduplicates and strips empty hidden entries");
    Check(l.folders.size() == 1 && l.folders[0].apps.size() == 2 && l.folders[0].apps[0] == L"App1" && l.folders[0].apps[1] == L"App2",
          "Clamp strips hidden/invalid/duplicate apps across folders and drops emptied folders");
    Check(l.order.size() == 2 && l.order[0] == L"folder:1" && l.order[1] == L"App3", "Clamp strips hidden and duplicate root order entries");
    Check(l.NewFolderId() == L"folder:2", "NewFolderId increments beyond highest existing folder id");

    const std::wstring json = SerializeLaunchpadLayoutJson(l);
    LaunchpadLayout parsed;
    Check(ParseLaunchpadLayoutJson(json, parsed), "serialized LaunchpadLayout parses cleanly");
    Check(parsed.order == l.order && parsed.hidden == l.hidden && parsed.folders.size() == 1 && parsed.folders[0].id == L"folder:1" &&
              parsed.folders[0].name == L"Work" && parsed.folders[0].apps == l.folders[0].apps,
          "LaunchpadLayout survives JSON round trip");
}

} // namespace

int main()
{
    TestGridLayout();
    TestPaging();
    TestMoveSelection();
    TestSettlePageDrag();
    TestTileMutations();
    TestExpandLaunchpadPageIds();
    TestLaunchpadLayoutJson();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PASS: launchpad_model_tests\n";
    return 0;
}
