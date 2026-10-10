// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <algorithm>
#include <string>
#include <vector>
#include "eden_uwp/folder_path.h"
#include "eden_uwp/storage_path.h"

int main() {
    using namespace EdenXbox;
    for (const auto* relative : {"", "game.nsp", "Games/Title.xci", "a/b/c.nro"}) {
        const auto parsed = ParseStoragePath(MakeStoragePath("eden-games-012345", relative));
        assert(parsed && parsed->token == "eden-games-012345" && parsed->relative == relative);
    }
    for (const auto* relative : {"/a.nsp", "a//b", "a/", "../a", "a/../b", "./a", "a/./b",
                                 "a\\b", "E:/a", "a:stream", "a/../../b"}) {
        assert(!ValidStorageRelative(relative));
        assert(!ParseStoragePath(MakeStoragePath("eden-games-1", relative)));
    }
    assert(!ValidStorageRelative(std::string{"a\0b", 3}));
    for (const auto* path : {"eden-usb:/bad/game.nsp", "eden-usb:/eden-games-1?/a", "E:/a.nsp",
                             "eden-usb:/eden-games-1_2/a", "eden-usb://eden-games-1/a"})
        assert(!ParseStoragePath(path));
    const auto a = MakeStoragePath("eden-games-1", "same.nsp");
    const auto b = MakeStoragePath("eden-games-2", "same.nsp");
    assert(a != b);
    assert(!ParseStoragePath(MakeStoragePath(std::string(101, 'a'))));

    for (const auto* path : {L"D:\\Games", L"C:\\Users\\Player\\LocalState",
                             L"\\\\server\\DevelopmentFiles", L"\\\\server\\share\\Games",
                             L"\\\\server\\share\\"})
        assert(IsAllowedAbsoluteFolderPath(path));
    for (const auto* path : {L"", L"Games", L"D:Games", L"\\\\server", L"\\\\server\\",
                             L"\\\\.\\C:\\", L"\\\\?\\C:\\Users\\Public", L"\\\\?\\UNC\\server\\share",
                             L"D:\\Games\\..\\Other", L"\\\\server\\share\\..\\Other",
                             L"D:\\\\Games", L"\\\\server\\share\\\\Games"})
        assert(!IsAllowedAbsoluteFolderPath(path));
    assert(NormalizeExternalFolderPath(L"  D:/Games  ") == L"D:\\Games");
    assert(NormalizeExternalFolderPath(L" \t\r\n ").empty());
    const auto ascii_equal = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; };
            if (lower(a[i]) != lower(b[i])) return false;
        }
        return true;
    };
    assert(IsSameOrDescendantFolderPath(L"D:\\Games", L"d:\\games\\", ascii_equal));
    assert(IsSameOrDescendantFolderPath(L"D:\\GAMES\\Mario", L"d:\\games", ascii_equal));
    assert(IsSameOrDescendantFolderPath(L"D:\\Games\\Mario", L"D:\\", ascii_equal));
    assert(!IsSameOrDescendantFolderPath(L"D:\\Games-backup", L"D:\\Games", ascii_equal));
    assert(!IsSameOrDescendantFolderPath(L"D:\\Games2\\Mario", L"D:\\Games", ascii_equal));
    assert(IsAllowedAbsoluteFolderPath(L"D:\\Games\\Rock & Roll (2026) [USA]'s"));
    assert(FolderPathKeyboardKey(6, true) == L"&");
    assert(FolderPathKeyboardKey(8, true) == L"(");
    assert(FolderPathKeyboardKey(9, true) == L")");
    assert(FolderPathKeyboardKey(10, true) == L"[");
    assert(FolderPathKeyboardKey(11, true) == L"]");
    assert(FolderPathKeyboardKey(14, true) == L"'");
    assert(FolderPathKeyboardKey(19, false) == L"Caps");
    assert(FolderPathKeyboardDisplayKey(19, false, true) == L"Lower");
    assert(FolderPathKeyboardInput(0, false, false) == L"q");
    assert(FolderPathKeyboardInput(0, false, true) == L"Q");
    assert(FolderPathKeyboardInput(6, true, true) == L"&");
    assert(FolderPathKeyboardKey(48, false) == L"Symbols");
    assert(FolderPathKeyboardKey(48, true) == L"Letters");
    assert(FolderPathKeyboardKey(49, true) == L"Add");
    assert(FolderPathKeyboardKeyAt(FolderPathKeyX + 6 * (FolderPathKeyWidth + FolderPathKeyGapX) + 1,
                                  FolderPathKeyY + 1, true) == 6);
    assert(FolderPathKeyboardKeyAt(FolderPathKeyX + 10 * (FolderPathKeyWidth + FolderPathKeyGapX),
                                  FolderPathKeyY + 1, true) == FolderPathKeyboardKeyCount);

    for (const size_t count : {size_t{0}, size_t{1}, size_t{8}, size_t{9},
                               size_t{16}, size_t{17}, size_t{24}, size_t{25}}) {
        std::vector<bool> visited(count, false);
        for (size_t page = 0;;) {
            const auto range = GetFolderBrowserPageRange(count, page);
            for (size_t i = range.begin; i < range.end; ++i) visited[i] = true;
            if (!range.has_more) break;
            page = range.end;
        }
        assert(std::all_of(visited.begin(), visited.end(), [](bool value) { return value; }));
    }
    assert(FolderBrowserRowAt(108, 178, 2) == 0);
    assert(FolderBrowserRowAt(1172, 213, 2) == 0);
    assert(FolderBrowserRowAt(108, 214, 2) == FolderBrowserVisibleRows);
    assert(FolderBrowserRowAt(108, 217, 2) == 1);
    assert(FolderBrowserRowAt(108, 217, 1) == FolderBrowserVisibleRows);
}
