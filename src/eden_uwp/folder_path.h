// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace EdenXbox {
inline std::wstring NormalizeExternalFolderPath(std::wstring path) {
    const auto first = path.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = path.find_last_not_of(L" \t\r\n");
    path = path.substr(first, last - first + 1);
    for (auto& character : path)
        if (character == L'/') character = L'\\';
    return path;
}

inline bool HasSafeFolderComponents(std::wstring_view path, std::size_t offset) {
    while (offset < path.size()) {
        if (path[offset] == L'\\') return false;
        const auto end = path.find(L'\\', offset);
        const auto component = path.substr(offset, end == std::wstring_view::npos ? path.size() - offset : end - offset);
        if (component == L"." || component == L"..") return false;
        if (end == std::wstring_view::npos || end + 1 == path.size()) return true;
        offset = end + 1;
    }
    return true;
}

inline bool IsAllowedAbsoluteFolderPath(std::wstring_view path) {
    if (path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
                             (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\')
        return HasSafeFolderComponents(path, 3);

    // Accept ordinary UNC share paths only. Device/extended namespaces (\\.\ and \\?\)
    // bypass the drive-volume broker and are not part of this app's storage flow.
    if (path.size() < 5 || path[0] != L'\\' || path[1] != L'\\' ||
        path[2] == L'.' || path[2] == L'?') return false;
    const auto server_end = path.find(L'\\', 2);
    if (server_end == std::wstring_view::npos || server_end == 2) return false;
    const auto share_end = path.find(L'\\', server_end + 1);
    const auto share_length = (share_end == std::wstring_view::npos ? path.size() : share_end) - server_end - 1;
    if (!share_length) return false;
    const auto server = path.substr(2, server_end - 2);
    const auto share = path.substr(server_end + 1, share_length);
    if (server == L"." || server == L".." || share == L"." || share == L"..") return false;
    return share_end == std::wstring_view::npos || HasSafeFolderComponents(path, share_end + 1);
}

template <typename CaseInsensitiveEqual>
inline bool IsSameOrDescendantFolderPath(std::wstring_view path, std::wstring_view folder,
                                         CaseInsensitiveEqual equal) {
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/'))
        folder.remove_suffix(1);
    if (folder.empty() || path.size() < folder.size() || !equal(path.substr(0, folder.size()), folder))
        return false;
    return path.size() == folder.size() || folder.back() == L'\\' || folder.back() == L'/' ||
           path[folder.size()] == L'\\' || path[folder.size()] == L'/';
}

inline constexpr size_t FolderPathInputLimit = 1024;
using FolderPathKeyboardLayout = std::array<std::array<std::wstring_view, 10>, 5>;
inline constexpr FolderPathKeyboardLayout FolderPathLettersKeyboard{{
    {L"q", L"w", L"e", L"r", L"t", L"y", L"u", L"i", L"o", L"p"},
    {L"a", L"s", L"d", L"f", L"g", L"h", L"j", L"k", L"l", L"Caps"},
    {L"z", L"x", L"c", L"v", L"b", L"n", L"m", L"_", L"-", L""},
    {L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9"},
    {L"\\", L"/", L":", L".", L"Space", L"Backspace", L"Left", L"Right", L"Symbols", L"Add"},
}};
inline constexpr FolderPathKeyboardLayout FolderPathSymbolsKeyboard{{
    {L"!", L"@", L"#", L"$", L"%", L"^", L"&", L"*", L"(", L")"},
    {L"[", L"]", L"{", L"}", L"'", L"+", L"=", L";", L",", L"Clear"},
    {L"_", L"-", L"\\", L"/", L":", L".", L" ", L"<", L">", L"~"},
    {L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9"},
    {L"\\", L"/", L":", L".", L"Space", L"Backspace", L"Left", L"Right", L"Letters", L"Add"},
}};
inline constexpr size_t FolderPathKeyboardRows = FolderPathLettersKeyboard.size();
inline constexpr size_t FolderPathKeyboardColumns = FolderPathLettersKeyboard.front().size();
inline constexpr size_t FolderPathKeyboardKeyCount = FolderPathKeyboardRows * FolderPathKeyboardColumns;
inline constexpr std::wstring_view FolderPathKeyboardKey(size_t key, bool symbols) {
    if (key >= FolderPathKeyboardKeyCount) return {};
    const auto& layout = symbols ? FolderPathSymbolsKeyboard : FolderPathLettersKeyboard;
    return layout[key / FolderPathKeyboardColumns][key % FolderPathKeyboardColumns];
}
inline std::wstring FolderPathKeyboardDisplayKey(size_t key, bool symbols, bool uppercase) {
    const auto label = FolderPathKeyboardKey(key, symbols);
    if (label == L"Caps" && uppercase) return L"Lower";
    if (!symbols && uppercase && label.size() == 1 && label[0] >= L'a' && label[0] <= L'z')
        return std::wstring{static_cast<wchar_t>(label[0] - (L'a' - L'A'))};
    return std::wstring{label};
}
inline std::wstring FolderPathKeyboardInput(size_t key, bool symbols, bool uppercase) {
    const auto label = FolderPathKeyboardKey(key, symbols);
    if (!symbols && uppercase && label.size() == 1 && label[0] >= L'a' && label[0] <= L'z')
        return std::wstring{static_cast<wchar_t>(label[0] - (L'a' - L'A'))};
    return std::wstring{label};
}
inline constexpr float FolderPathKeyX = 110;
inline constexpr float FolderPathKeyY = 292;
inline constexpr float FolderPathKeyWidth = 100;
inline constexpr float FolderPathKeyHeight = 40;
inline constexpr float FolderPathKeyGapX = 6;
inline constexpr float FolderPathKeyGapY = 6;
inline size_t FolderPathKeyboardKeyAt(float x, float y, bool symbols = false) {
    if (x < FolderPathKeyX || y < FolderPathKeyY) return FolderPathKeyboardKeyCount;
    const auto column = static_cast<size_t>((x - FolderPathKeyX) / (FolderPathKeyWidth + FolderPathKeyGapX));
    const auto row = static_cast<size_t>((y - FolderPathKeyY) / (FolderPathKeyHeight + FolderPathKeyGapY));
    if (column >= FolderPathKeyboardColumns || row >= FolderPathKeyboardRows) return FolderPathKeyboardKeyCount;
    const float key_x = FolderPathKeyX + column * (FolderPathKeyWidth + FolderPathKeyGapX);
    const float key_y = FolderPathKeyY + row * (FolderPathKeyHeight + FolderPathKeyGapY);
    if (x >= key_x + FolderPathKeyWidth || y >= key_y + FolderPathKeyHeight ||
        FolderPathKeyboardKey(row * FolderPathKeyboardColumns + column, symbols).empty())
        return FolderPathKeyboardKeyCount;
    return row * FolderPathKeyboardColumns + column;
}
inline constexpr size_t FolderBrowserPageSize = 8;
inline constexpr float FolderBrowserListX = 108;
inline constexpr float FolderBrowserListRight = 1172;
inline constexpr float FolderBrowserListTop = 178;
inline constexpr float FolderBrowserRowHeight = 36;
inline constexpr float FolderBrowserRowStride = 39;
inline constexpr size_t FolderBrowserVisibleRows = 10;

struct FolderBrowserPageRange {
    size_t begin{};
    size_t end{};
    bool has_more{};
};

inline constexpr FolderBrowserPageRange GetFolderBrowserPageRange(size_t count, size_t page) {
    if (page >= count) return {count, count, false};
    const size_t remaining = count - page;
    const size_t end = remaining > FolderBrowserPageSize ? page + FolderBrowserPageSize : count;
    return {page, end, end < count};
}

inline constexpr size_t FolderBrowserRowAt(float x, float y, size_t row_count) {
    if (x < FolderBrowserListX || x > FolderBrowserListRight || y < FolderBrowserListTop)
        return FolderBrowserVisibleRows;
    const auto row = static_cast<size_t>((y - FolderBrowserListTop) / FolderBrowserRowStride);
    if (row >= FolderBrowserVisibleRows || row >= row_count ||
        y >= FolderBrowserListTop + row * FolderBrowserRowStride + FolderBrowserRowHeight)
        return FolderBrowserVisibleRows;
    return row;
}
} // namespace EdenXbox
