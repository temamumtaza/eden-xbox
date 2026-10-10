// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_library.h"
#include "eden_uwp/folder_path.h"
#include "eden_uwp/stick_navigation.h"
#include "eden_uwp/uwp_library_canvas.h"
#include "eden_uwp/uwp_rom_storage.h"
#include "eden_uwp/storage_path.h"
#include "eden_uwp/uwp_file_manager.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Search.h>
#include "eden_uwp/game_library.h"
#include "eden_uwp/uwp_input.h"
#include "eden_uwp/uwp_library_metadata.h"
#include "common/logging.h"

namespace EdenXbox {
namespace {
using winrt::Windows::Gaming::Input::Gamepad;
using winrt::Windows::Gaming::Input::GamepadButtons;
using winrt::Windows::System::VirtualKey;
using winrt::Windows::Storage::StorageFolder;
using winrt::Windows::Storage::AccessCache::StorageApplicationPermissions;

struct FolderPathFailure { std::wstring message; };
struct FolderResolutionControl {
    std::mutex mutex;
    winrt::Windows::Foundation::IAsyncInfo current{nullptr};
    bool cancelled{};
    void Track(winrt::Windows::Foundation::IAsyncInfo operation) {
        bool should_cancel{};
        {
            std::lock_guard lock{mutex};
            current = operation;
            should_cancel = cancelled;
        }
        if (should_cancel && operation) {
            try { operation.Cancel(); } catch (...) {}
        }
    }
    void Clear() {
        std::lock_guard lock{mutex};
        current = nullptr;
    }
    void Cancel() {
        winrt::Windows::Foundation::IAsyncInfo operation{nullptr};
        {
            std::lock_guard lock{mutex};
            cancelled = true;
            operation = current;
        }
        if (operation) {
            try { operation.Cancel(); } catch (...) {}
        }
    }
    bool IsCancelled() {
        std::lock_guard lock{mutex};
        return cancelled;
    }
};
struct FolderPathResolution {
    StorageFolder folder{nullptr};
    std::wstring error;
    bool cancelled{};
};
struct FolderBrowserLoadResult {
    std::vector<FolderBrowserEntry> entries;
    std::wstring location;
    std::wstring error;
    size_t page{};
    bool at_roots{};
    bool has_more{};
    bool cancelled{};
};

template <typename TOperation>
auto AwaitFolderOperation(TOperation operation, const std::shared_ptr<FolderResolutionControl>& control) {
    control->Track(operation.template as<winrt::Windows::Foundation::IAsyncInfo>());
    try {
        auto result = operation.get();
        control->Clear();
        return result;
    } catch (...) {
        control->Clear();
        throw;
    }
}

std::wstring FolderDisplayName(const StorageFolder& folder, std::wstring_view fallback) {
    const auto name = folder.Name();
    if (!name.empty()) return std::wstring{name};
    const auto path = std::wstring{folder.Path()};
    const auto separator = path.find_last_of(L"\\/");
    if (separator != std::wstring::npos && separator + 1 < path.size()) return path.substr(separator + 1);
    return std::wstring{fallback};
}

FolderBrowserLoadResult LoadFolderBrowserPage(
    bool at_roots, const StorageFolder& current, size_t page, std::stop_token stop,
    const std::shared_ptr<FolderResolutionControl>& control) {
    FolderBrowserLoadResult result;
    result.at_roots = at_roots;
    result.page = page;
    if (stop.stop_requested() || control->IsCancelled()) { result.cancelled = true; return result; }
    if (!at_roots && !current) {
        result.error = L"No folder is available to browse.";
        return result;
    }

    if (at_roots) {
        struct RootCandidate {
            std::wstring label;
            StorageFolder folder{nullptr};
            winrt::hstring token;
        };
        std::vector<RootCandidate> roots;
        auto local = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
        roots.push_back({L"Eden internal storage", local, {}});
        try {
            auto volumes = AwaitFolderOperation(
                winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync(), control);
            for (const auto& volume : volumes) {
                if (stop.stop_requested() || control->IsCancelled()) { result.cancelled = true; return result; }
                roots.push_back({L"Removable drive: " + FolderDisplayName(volume, L"Drive"), volume, {}});
            }
        } catch (const winrt::hresult_error&) {
            if (stop.stop_requested() || control->IsCancelled()) { result.cancelled = true; return result; }
            // The internal folder and saved grants remain useful if removable storage is absent.
        }

        // FutureAccessList is platform-bounded. Resolve only the visible page so a
        // large saved grant list cannot stall navigation or allocate an unbounded view.
        const auto grants = StorageApplicationPermissions::FutureAccessList().Entries();
        for (const auto& entry : grants)
            roots.push_back({L"Saved access", nullptr, entry.Token});

        const auto page_range = GetFolderBrowserPageRange(roots.size(), page);
        for (size_t i = page_range.begin; i < page_range.end; ++i) {
            if (stop.stop_requested() || control->IsCancelled()) { result.cancelled = true; return result; }
            auto& candidate = roots[i];
            if (!candidate.folder) {
                try {
                    candidate.folder = AwaitFolderOperation(
                        StorageApplicationPermissions::FutureAccessList().GetFolderAsync(candidate.token), control);
                    candidate.label += L": " + FolderDisplayName(candidate.folder, L"Saved folder");
                } catch (const winrt::hresult_error&) {
                    if (stop.stop_requested() || control->IsCancelled()) { result.cancelled = true; return result; }
                    continue; // Stale grants do not prevent access to later entries.
                }
            }
            result.entries.push_back({std::move(candidate.label), std::move(candidate.folder)});
        }
        result.has_more = page_range.has_more;
        result.location = L"Locations available to Eden";
        return result;
    }

    result.location = std::wstring{current.Path()};
    try {
        const auto start = static_cast<uint32_t>(std::min<size_t>(page, UINT32_MAX - FolderBrowserPageSize - 1));
        auto folders = AwaitFolderOperation(current.GetFoldersAsync(
            winrt::Windows::Storage::Search::CommonFolderQuery::DefaultQuery,
            start, static_cast<uint32_t>(FolderBrowserPageSize + 1)), control);
        const auto visible = std::min<uint32_t>(folders.Size(), static_cast<uint32_t>(FolderBrowserPageSize));
        result.entries.reserve(visible);
        for (uint32_t i = 0; i < visible; ++i) {
            if (stop.stop_requested() || control->IsCancelled()) { result.cancelled = true; result.entries.clear(); return result; }
            const auto folder = folders.GetAt(i);
            result.entries.push_back({FolderDisplayName(folder, L"Folder"), folder});
        }
        result.has_more = folders.Size() > FolderBrowserPageSize;
    } catch (const winrt::hresult_error&) {
        if (stop.stop_requested() || control->IsCancelled()) result.cancelled = true;
        else result.error = L"Windows could not list this folder. Check permissions and make sure the drive is connected.";
    }
    return result;
}

bool PathStartsWithFolder(std::wstring_view path, std::wstring_view folder_path) {
    return IsSameOrDescendantFolderPath(path, folder_path, [](std::wstring_view a, std::wstring_view b) {
        return a.size() <= INT_MAX && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                   b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    });
}

StorageFolder NavigateFolderPath(StorageFolder folder, std::wstring_view path,
                                 std::stop_token stop,
                                 const std::shared_ptr<FolderResolutionControl>& control) {
    const std::wstring root_path{folder.Path()};
    if (!PathStartsWithFolder(path, root_path)) return nullptr;
    size_t offset = root_path.size();
    while (offset < path.size() && path[offset] == L'\\') ++offset;
    while (offset < path.size()) {
        if (stop.stop_requested() || control->IsCancelled()) return nullptr;
        const auto end = path.find(L'\\', offset);
        const auto length = (end == std::wstring_view::npos ? path.size() : end) - offset;
        const auto component = path.substr(offset, length);
        if (component == L"." || component == L".." || component.empty())
            throw FolderPathFailure{L"The path contains an invalid component."};
        folder = AwaitFolderOperation(folder.GetFolderAsync(winrt::hstring{component}), control);
        if (end == std::wstring_view::npos) break;
        offset = end + 1;
    }
    return folder;
}

StorageFolder ResolveFolderPath(std::wstring_view path, std::stop_token stop,
                                const std::shared_ptr<FolderResolutionControl>& control) {
    if (stop.stop_requested() || control->IsCancelled()) return nullptr;
    if (path.starts_with(L"\\\\"))
        return AwaitFolderOperation(StorageFolder::GetFolderFromPathAsync(winrt::hstring{path}), control);

    // LocalState is directly available to this app. Other drive paths are first
    // resolved through saved grants and RemovableDevices before the broker fallback.
    auto local = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
    if (PathStartsWithFolder(path, std::wstring{local.Path()}))
        return NavigateFolderPath(local, path, stop, control);

    // Use only folders for which this app has a persisted FutureAccessList grant.
    // A grant can point at a subfolder beneath a mounted volume, so check it before
    // the volume roots and navigate descendants through StorageFolder APIs.
    const auto grants = StorageApplicationPermissions::FutureAccessList().Entries();
    for (const auto& entry : grants) {
        if (stop.stop_requested() || control->IsCancelled()) return nullptr;
        try {
            auto granted = AwaitFolderOperation(
                StorageApplicationPermissions::FutureAccessList().GetFolderAsync(entry.Token), control);
            if (PathStartsWithFolder(path, std::wstring{granted.Path()}))
                return NavigateFolderPath(granted, path, stop, control);
        } catch (const winrt::hresult_error&) {
            // A stale grant (for example a removed USB) must not hide later roots.
        }
    }

    try {
        auto volumes = AwaitFolderOperation(
            winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync(), control);
        for (const auto& volume : volumes) {
            if (stop.stop_requested() || control->IsCancelled()) return nullptr;
            if (PathStartsWithFolder(path, std::wstring{volume.Path()}))
                return NavigateFolderPath(volume, path, stop, control);
        }
    } catch (const winrt::hresult_error&) {
        if (stop.stop_requested() || control->IsCancelled()) return nullptr;
        // A platform that denies removable-volume enumeration can still resolve
        // a drive path that Windows otherwise exposes to this app.
    }

    // Last, ask the WinRT storage broker directly. This does not bypass the
    // AppContainer: Windows still decides whether this absolute drive path is visible.
    return AwaitFolderOperation(StorageFolder::GetFolderFromPathAsync(winrt::hstring{path}), control);
}

std::wstring FolderPathError(const winrt::hresult_error& error, bool unc) {
    const HRESULT hr = error.code();
    const DWORD code = HRESULT_CODE(hr);
    if (hr == E_ACCESSDENIED || code == ERROR_ACCESS_DENIED)
        return L"Access denied. Windows does not allow the app to use this path; private Xbox paths remain sandboxed.";
    if (code == ERROR_PATH_NOT_FOUND || code == ERROR_FILE_NOT_FOUND || code == ERROR_NOT_FOUND)
        return L"Folder not found. Check the path and make sure the drive is connected.";
    if (unc)
        return L"Could not reach or open the network folder. Check SMB and Windows access. Credentials are not saved.";
    return L"Could not open the folder. Check that the path is absolute and Windows allows access.";
}

} // namespace

std::optional<std::string> ShowGameLibrary(void* core_window, unsigned width, unsigned height,
                                         const std::filesystem::path& root,
                                         const std::function<void()>& seed,
                                         std::string_view auto_pick, std::string_view initial_notice) {
    using namespace winrt::Windows::UI::Core;
    auto window = CoreWindow::GetForCurrentThread();
    auto canvas = std::make_unique<LibraryCanvas>(core_window, width, height);
    LibraryScan scan;
    size_t selected = 0;
    bool dirty = true, loading = true, settings = false;
    ConfigurationPanel configuration;
    bool return_to_configuration = false;
    unsigned picker_kind = 0; // 0 games, 1 keys, 2 firmware
    std::future<FileSetupStatus> setup_future;
    std::jthread setup_worker;
    FileImportProgress import_progress;
    std::future<std::string> import_future;
    std::jthread import_worker;
    std::future<FolderPathResolution> folder_path_future;
    std::jthread folder_path_worker;
    std::shared_ptr<FolderResolutionControl> folder_path_control;
    bool folder_path_cancelled = false;
    std::future<FolderBrowserLoadResult> folder_browser_future;
    std::jthread folder_browser_worker;
    std::shared_ptr<FolderResolutionControl> folder_browser_control;
    bool folder_browser_cancelled = false;
    StorageFolder browser_current{nullptr};
    std::vector<StorageFolder> browser_stack;
    bool added_folder_pending = false;
    std::chrono::steady_clock::time_point auto_pick_at{};
    unsigned setting_row = 0;
    ControllerPanel panel;
    panel.xbox = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
    ControllerOptions options = LoadControllerOptions();
    if (panel.xbox) options.controller_id.clear();
    panel.keyboard.bindings = LoadKeyboardBindings();
    auto capture_until = std::chrono::steady_clock::time_point{};
    std::wstring notice; // Library scan/metadata messages only.
    auto keyboard_notice_until = std::chrono::steady_clock::time_point{};
    auto controller_notice_until = std::chrono::steady_clock::time_point{};
    auto save_keyboard = [&] {
        const bool saved = SaveKeyboardBindings(panel.keyboard.bindings);
        panel.keyboard.notice = saved ? L"Keyboard settings saved" : L"Could not save keyboard settings";
        keyboard_notice_until = saved ? std::chrono::steady_clock::now() + std::chrono::seconds{3} :
                                       std::chrono::steady_clock::time_point::max();
        dirty = true;
    };
    auto begin_capture = [&] {
        panel.keyboard.notice.clear();
        panel.keyboard.capturing = true;
        capture_until = std::chrono::steady_clock::now() + std::chrono::seconds{4};
        dirty = true;
    };
    auto insert_path_text = [&](std::wstring_view value) {
        if (value.empty() || configuration.path_resolving) return;
        if (configuration.path_text.size() + value.size() > FolderPathInputLimit) {
            configuration.path_error = L"The path exceeds the 1,024-character limit.";
            dirty = true;
            return;
        }
        configuration.path_text.insert(configuration.path_cursor, value);
        configuration.path_cursor += value.size();
        configuration.path_error.clear();
        dirty = true;
    };
    auto erase_path_before_cursor = [&] {
        if (!configuration.path_cursor || configuration.path_resolving) return;
        size_t first = configuration.path_cursor - 1;
        if (first && configuration.path_text[first] >= 0xdc00 && configuration.path_text[first] <= 0xdfff &&
            configuration.path_text[first - 1] >= 0xd800 && configuration.path_text[first - 1] <= 0xdbff)
            --first;
        configuration.path_text.erase(first, configuration.path_cursor - first);
        configuration.path_cursor = first;
        configuration.path_error.clear();
        dirty = true;
    };
    auto erase_path_at_cursor = [&] {
        if (configuration.path_cursor >= configuration.path_text.size() || configuration.path_resolving) return;
        size_t last = configuration.path_cursor + 1;
        if (last < configuration.path_text.size() && configuration.path_text[configuration.path_cursor] >= 0xd800 &&
            configuration.path_text[configuration.path_cursor] <= 0xdbff &&
            configuration.path_text[last] >= 0xdc00 && configuration.path_text[last] <= 0xdfff)
            ++last;
        configuration.path_text.erase(configuration.path_cursor, last - configuration.path_cursor);
        configuration.path_error.clear();
        dirty = true;
    };
    auto move_path_cursor = [&](bool right) {
        if (configuration.path_resolving) return;
        if (right && configuration.path_cursor < configuration.path_text.size()) {
            if (configuration.path_text[configuration.path_cursor] >= 0xd800 &&
                configuration.path_text[configuration.path_cursor] <= 0xdbff &&
                configuration.path_cursor + 1 < configuration.path_text.size() &&
                configuration.path_text[configuration.path_cursor + 1] >= 0xdc00 &&
                configuration.path_text[configuration.path_cursor + 1] <= 0xdfff)
                configuration.path_cursor += 2;
            else ++configuration.path_cursor;
        } else if (!right && configuration.path_cursor) {
            --configuration.path_cursor;
            if (configuration.path_cursor && configuration.path_text[configuration.path_cursor] >= 0xdc00 &&
                configuration.path_text[configuration.path_cursor] <= 0xdfff &&
                configuration.path_text[configuration.path_cursor - 1] >= 0xd800 &&
                configuration.path_text[configuration.path_cursor - 1] <= 0xdbff)
                --configuration.path_cursor;
        }
        dirty = true;
    };
    bool device_changed = false;
    size_t retained_page = SIZE_MAX;
    bool metadata_dirty = true;
    unsigned actions = 0;
    enum : unsigned { Up = 1, Down = 2, Play = 4, Quit = 8, Swap = 16, Zone = 32,
                      Refresh = 64, Panel = 128, Left = 256, Right = 512, AddFolder = 1024 };
    using winrt::Windows::Storage::StorageFolder;
    winrt::Windows::Foundation::IAsyncOperation<StorageFolder> picker{nullptr};
    std::future<std::string> validation;
    std::jthread validation_worker;
    std::string selected_path;
    bool resize = false, window_closed = false;
    auto size_changed = window.SizeChanged(winrt::auto_revoke, [&](auto&&, WindowSizeChangedEventArgs const& e) {
        const auto scale = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        width = std::max(1U, static_cast<unsigned>(e.Size().Width * scale));
        height = std::max(1U, static_cast<unsigned>(e.Size().Height * scale));
        resize = true;
    });
    auto closed = window.Closed(winrt::auto_revoke, [&](auto&&, CoreWindowEventArgs const&) { window_closed = true; });
    auto visible = window.VisibilityChanged(winrt::auto_revoke, [&](auto&&, VisibilityChangedEventArgs const&) { dirty = true; });
    auto activation = window.Activated(winrt::auto_revoke, [&](auto&&, WindowActivatedEventArgs const& e) {
        if (e.WindowActivationState() == CoreWindowActivationState::Deactivated) {
            panel.keyboard.capturing = false; dirty = true;
        }
    });
    auto keyboard = window.KeyDown(winrt::auto_revoke, [&](auto&&, KeyEventArgs const& e) {
        if (e.KeyStatus().WasKeyDown) return;
        if (configuration.path_entry_open) {
            const auto key = e.VirtualKey();
            if (key == VirtualKey::Escape) { actions |= Quit; e.Handled(true); }
            else if (key == VirtualKey::Enter) {
                configuration.path_key = FolderPathKeyboardKeyCount - 1;
                actions |= Play; e.Handled(true);
            }
            else if (key == VirtualKey::Back) { erase_path_before_cursor(); e.Handled(true); }
            else if (key == VirtualKey::Delete) { erase_path_at_cursor(); e.Handled(true); }
            else if (key == VirtualKey::Left) { move_path_cursor(false); e.Handled(true); }
            else if (key == VirtualKey::Right) { move_path_cursor(true); e.Handled(true); }
            else if (key == VirtualKey::Home) { configuration.path_cursor = 0; dirty = true; e.Handled(true); }
            else if (key == VirtualKey::End) {
                configuration.path_cursor = configuration.path_text.size(); dirty = true; e.Handled(true);
            } else if (configuration.path_resolving) e.Handled(true);
            // Let printable key events reach CharacterReceived for Unicode input.
            return;
        }
        if (configuration.browser_open) {
            switch (e.VirtualKey()) {
            case VirtualKey::Up: actions |= Up; break;
            case VirtualKey::Down: actions |= Down; break;
            case VirtualKey::Left: actions |= Left; break;
            case VirtualKey::Right: actions |= Right; break;
            case VirtualKey::Enter: actions |= Play; break;
            case VirtualKey::Escape: actions |= Quit; break;
            default: break;
            }
            e.Handled(true);
            return;
        }
        auto& editor = panel.keyboard;
        if (editor.open) {
            const auto key = e.VirtualKey();
            if (editor.capturing) {
                if (key != VirtualKey::Escape) {
                    if (AssignKeyboardKey(editor.bindings, editor.selected, static_cast<unsigned>(key))) save_keyboard();
                    else {
                        editor.notice = L"That key is not supported";
                        keyboard_notice_until = std::chrono::steady_clock::time_point::max();
                    }
                }
                editor.capturing = false;
                dirty = true;
            } else if (key == VirtualKey::Escape) { editor.open = false; dirty = true; }
            else if (key == VirtualKey::Tab) {
                editor.page = (editor.page + 1) % 3; editor.selected = KeyboardPageActions(editor.page).front(); dirty = true;
            } else if (key == VirtualKey::Enter) begin_capture();
            else if (key == VirtualKey::Delete) {
                AssignKeyboardKey(editor.bindings, editor.selected, 0); save_keyboard();
            } else if (key == VirtualKey::Up || key == VirtualKey::Down ||
                       key == VirtualKey::Left || key == VirtualKey::Right) {
                const auto items = KeyboardPageActions(editor.page);
                const size_t count = items.size();
                const size_t index = static_cast<size_t>(std::find(items.begin(), items.end(), editor.selected) - items.begin());
                const size_t step = key == VirtualKey::Up || key == VirtualKey::Down ? 2 : 1;
                const bool reverse = key == VirtualKey::Up || key == VirtualKey::Left;
                editor.selected = items[(index + (reverse ? count - step : step)) % count];
                dirty = true;
            }
            e.Handled(true);
            return;
        }
        switch (e.VirtualKey()) {
        case VirtualKey::Up: actions |= Up; break;
        case VirtualKey::Down: actions |= Down; break;
        case VirtualKey::Left: actions |= Left; break;
        case VirtualKey::Right: actions |= Right; break;
        case VirtualKey::F1: actions |= Panel; break;
        case VirtualKey::Escape: actions |= Quit; break;
        case VirtualKey::Enter: actions |= Play; break;
        case VirtualKey::X: actions |= Swap; break;
        case VirtualKey::Y: actions |= Zone; break;
        case VirtualKey::R: actions |= Refresh; break;
        case VirtualKey::O: actions |= AddFolder; break;
        default: break;
        }
        e.Handled(true);
    });
    auto character_received = window.CharacterReceived(winrt::auto_revoke, [&](auto&&, CharacterReceivedEventArgs const& e) {
        if (!configuration.path_entry_open || configuration.path_resolving) return;
        const auto codepoint = static_cast<uint32_t>(e.KeyCode());
        if (codepoint >= 0x20 && codepoint <= 0x10ffff && codepoint != 0x7f) {
            if (codepoint <= 0xffff) {
                const wchar_t character = static_cast<wchar_t>(codepoint);
                insert_path_text(std::wstring_view{&character, 1});
            } else {
                const uint32_t adjusted = codepoint - 0x10000;
                const wchar_t pair[]{static_cast<wchar_t>(0xd800 + (adjusted >> 10)),
                                     static_cast<wchar_t>(0xdc00 + (adjusted & 0x3ff))};
                insert_path_text(std::wstring_view{pair, 2});
            }
        }
        e.Handled(true);
    });
    auto pointer = window.PointerPressed(winrt::auto_revoke, [&](auto&&, PointerEventArgs const& e) {
        const auto p = e.CurrentPoint().Position();
        const auto dpi = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        const auto scale = std::max(0.01f, std::min(width / 1280.0f, height / 720.0f));
        const float x = static_cast<float>((p.X * dpi - (width - 1280 * scale) / 2) / scale);
        const float y = static_cast<float>((p.Y * dpi - (height - 720 * scale) / 2) / scale);
        if (configuration.browser_open) {
            if (y >= 604 && y < 652) {
                if (x < 330) actions |= Play;
                else if (x < 610) actions |= Left;
                else if (x < 930) actions |= Right;
                else actions |= Quit;
            } else if (!configuration.browser_loading) {
                const size_t count = configuration.browser_at_roots ? configuration.browser_entries.size() :
                                     2 + configuration.browser_entries.size();
                const auto row = FolderBrowserRowAt(x, y, count);
                if (row < FolderBrowserVisibleRows) {
                    configuration.browser_selected = row; actions |= Play; dirty = true;
                }
            }
            e.Handled(true);
            return;
        }
        if (configuration.path_entry_open) {
            if (x >= 980 && y >= 604 && y < 652) actions |= Quit;
            else if (!configuration.path_resolving && x >= 100 && x < 330 && y >= 604 && y < 652) {
                configuration.path_key = FolderPathKeyboardKeyCount - 1;
                actions |= Play;
            } else if (!configuration.path_resolving && x >= 108 && x <= 1172 && y >= 172 && y < 226) {
                configuration.path_cursor = configuration.path_text.size();
                dirty = true;
            } else if (!configuration.path_resolving) {
                const auto key = FolderPathKeyboardKeyAt(x, y, configuration.path_symbols);
                if (key < FolderPathKeyboardKeyCount) {
                    configuration.path_key = key;
                    actions |= Play;
                }
            }
            e.Handled(true);
            return;
        }
        if (configuration.open && !settings) {
            const auto layout = GetConfigurationLayout(configuration);
            if (y >= layout.footer - 7 && y < layout.footer + 40 &&
                x >= layout.x + layout.width - 230 && x < layout.x + layout.width) actions |= Quit;
            else if (!configuration.busy && x >= layout.x && x < layout.x + layout.width && y >= layout.top) {
                const auto row = static_cast<size_t>((y - layout.top) / layout.stride);
                const auto choice = ConfigurationFirstRow(configuration) + row;
                if (row < 5 && y < layout.top + row * layout.stride + layout.row_height &&
                    choice < ConfigurationRowCount(configuration)) {
                    configuration.selected = choice; actions |= Play; dirty = true;
                }
            }
            e.Handled(true);
            return;
        }
        if (panel.keyboard.open) {
            auto& editor = panel.keyboard;
            if (Contains({1120, 112, 48, 40}, x, y)) {
                editor.open = editor.capturing = false; dirty = true;
            } else if (!editor.capturing) {
                if (Contains({635, 203, 514, 36}, x, y)) {
                    editor.page = std::min(2U, static_cast<unsigned>((x - 635) / 174));
                    editor.selected = KeyboardPageActions(editor.page).front(); dirty = true;
                } else if (Contains({635, 603, 155, 36}, x, y)) {
                    AssignKeyboardKey(editor.bindings, editor.selected, 0); save_keyboard();
                } else if (Contains({806, 603, 170, 36}, x, y)) {
                    editor.bindings = DefaultKeyboardBindings; save_keyboard();
                } else {
                    for (size_t i = 0; i < KeyboardButtonCount; ++i) {
                        if (Contains(KeyboardDiagramTile(i), x, y)) {
                            editor.page = KeyboardPageFor(i); editor.selected = i; begin_capture(); break;
                        }
                    }
                    for (const size_t i : KeyboardPageActions(editor.page)) {
                        if (!editor.capturing && Contains(KeyboardTileFor(i), x, y)) {
                            editor.selected = i; begin_capture(); break;
                        }
                    }
                }
            }
            e.Handled(true); return;
        }
        if (settings) {
            if (panel.expanded) {
                const size_t first = ControllerChoiceFirst(panel);
                if (x >= 648 && x < 1178 && y >= ControllerChoiceTop) {
                    const size_t i = first + static_cast<size_t>((y - ControllerChoiceTop) / ControllerChoiceHeight);
                    if (i < panel.devices.size() + 2 && i < first + ControllerVisibleChoices) {
                        panel.choice = i;
                        actions |= Play;
                    } else actions |= Quit;
                } else actions |= Quit;
            } else if (!panel.xbox && Contains({648, 510, 530, 34}, x, y)) {
                panel.keyboard.open = true; panel.keyboard.capturing = false; dirty = true;
            } else if (x < 620 || x > 1206 || y < 133 || y > 620) actions |= Panel;
            else if (x >= 648 && x < 1178 && y >= ControllerRowTop &&
                     y < ControllerRowTop + (panel.xbox ? 3 : 4) * ControllerRowHeight) {
                setting_row = static_cast<unsigned>((y - ControllerRowTop) / ControllerRowHeight);
                actions |= Play;
            }
        } else if (x >= 670 && x < 895 && y >= 43 && y < 87) actions |= AddFolder;
        else if (x >= 1080 && y >= 43 && y < 87) actions |= Panel;
        else if (!loading && x >= 320 && x < 504 && y >= 334 && y < 380) actions |= Play;
        else if (!loading && x >= 58 && x < 1226 && y >= 444 && y < 635) {
            const size_t index = selected / 5 * 5 + static_cast<size_t>((x - 58) / 232);
            if (index < scan.entries.size()) { selected = index; dirty = true; }
        }
        e.Handled(true);
    });
    auto wheel = window.PointerWheelChanged(winrt::auto_revoke, [&](auto&&, PointerEventArgs const& e) {
        if (settings && panel.expanded && !panel.keyboard.open) {
            const int delta = e.CurrentPoint().Properties().MouseWheelDelta();
            const size_t count = panel.devices.size() + 2;
            if (delta > 0 && panel.choice) --panel.choice;
            if (delta < 0 && panel.choice + 1 < count) ++panel.choice;
            dirty = true;
            e.Handled(true);
        }
    });
    std::future<LibraryScan> future;
    std::jthread scanner;
    struct MetadataBatch { std::vector<LibraryEntry> entries; unsigned generation{}; };
    std::future<MetadataBatch> metadata;
    std::jthread metadata_worker;
    unsigned generation = 0;
    bool logging_ready = false;
    auto refresh_setup = [&] {
        if (setup_future.valid()) return;
        std::packaged_task<FileSetupStatus()> task{[] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
            return ReadFileSetupStatus();
        }};
        setup_future = task.get_future();
        setup_worker = std::jthread(std::move(task));
    };
    auto start_scan = [&](StorageFolder added = nullptr) {
        added_folder_pending = static_cast<bool>(added);
        loading = dirty = true;
        ++generation;
        metadata_dirty = true;
        retained_page = SIZE_MAX;
        metadata_worker.request_stop();
        canvas->InvalidateCovers();
        std::packaged_task<LibraryScan(std::stop_token)> task{[&, added](std::stop_token stop) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
            seed();
            // Recover interrupted publication before a metadata worker constructs NCA readers.
            ReadFileSetupStatus();
            std::error_code ec;
            std::filesystem::create_directories(root, ec);
            auto result = ScanGameLibrary(root, stop);
            std::vector<std::wstring> internal_game_paths;
            internal_game_paths.reserve(result.entries.size());
            for (auto& entry : result.entries) {
                const auto file_path = root / entry.relative_path;
                const auto path = file_path.u8string();
                entry.launch_path.assign(reinterpret_cast<const char*>(path.data()), path.size());
                internal_game_paths.push_back(file_path.wstring());
                entry.source_name = L"Interna";
            }
            if (added) {
                try {
                    RememberGameFolder(added, stop);
                } catch (const winrt::hresult_error& e) {
                    result.error = "Could not save the external folder: " + winrt::to_string(e.message());
                } catch (const std::exception& e) {
                    result.error = e.what();
                }
            }
            try {
                auto external = ScanExternalGameFolders(stop, internal_game_paths);
                if (!external.error.empty()) {
                    if (!result.error.empty()) result.error += " | ";
                    result.error += external.error;
                }
                result.limited |= external.limited;
                result.entries.insert(result.entries.end(),
                    std::make_move_iterator(external.entries.begin()), std::make_move_iterator(external.entries.end()));
            } catch (const winrt::hresult_error& e) {
                if (!result.error.empty()) result.error += " | ";
                result.error += "Could not restore an external folder: " + winrt::to_string(e.message());
            } catch (const std::exception& e) {
                if (!result.error.empty()) result.error += " | ";
                result.error += e.what();
            }
            if (!result.entries.empty() && result.error == "The games folder is empty or does not exist.") result.error.clear();
            std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
                return a.launch_path < b.launch_path;
            });
            return result;
        }};
        future = task.get_future();
        scanner = std::jthread(std::move(task));
    };
    auto begin_import = [&](StorageFolder folder, unsigned kind) {
        configuration.busy = true;
        configuration.notice.clear();
        import_progress.completed = 0;
        import_progress.total = 0;
        metadata_worker.request_stop();
        std::packaged_task<std::string(std::stop_token)> task{
            [folder, kind, old_metadata = std::move(metadata_worker), &import_progress](std::stop_token stop) mutable {
                if (old_metadata.joinable()) old_metadata.join();
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                return ImportSystemFiles(folder, kind == 1 ? FileImport::Keys : FileImport::Firmware,
                                         import_progress, stop);
            }};
        import_future = task.get_future();
        import_worker = std::jthread(std::move(task));
        dirty = true;
    };
    auto start_folder_browser_load = [&](bool at_roots, StorageFolder current, size_t page) {
        if (folder_browser_future.valid()) return false;
        configuration.browser_loading = true;
        configuration.browser_at_roots = at_roots;
        configuration.browser_page = page;
        configuration.browser_selected = 0;
        configuration.browser_entries.clear();
        configuration.browser_location.clear();
        configuration.browser_error.clear();
        folder_browser_cancelled = false;
        auto control = std::make_shared<FolderResolutionControl>();
        folder_browser_control = control;
        std::packaged_task<FolderBrowserLoadResult(std::stop_token, bool, StorageFolder, size_t,
                                                    std::shared_ptr<FolderResolutionControl>)> task{
            [](std::stop_token stop, bool roots, StorageFolder folder, size_t first,
               std::shared_ptr<FolderResolutionControl> operation_control) {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                try { return LoadFolderBrowserPage(roots, folder, first, stop, operation_control); }
                catch (const winrt::hresult_error& error) {
                    FolderBrowserLoadResult result;
                    result.at_roots = roots;
                    result.page = first;
                    result.cancelled = stop.stop_requested() || operation_control->IsCancelled();
                    if (!result.cancelled) result.error = L"Could not list this location: " + std::wstring{error.message()};
                    return result;
                } catch (...) {
                    FolderBrowserLoadResult result;
                    result.at_roots = roots;
                    result.page = first;
                    result.cancelled = stop.stop_requested() || operation_control->IsCancelled();
                    if (!result.cancelled) result.error = L"Could not list this location.";
                    return result;
                }
            }};
        folder_browser_future = task.get_future();
        folder_browser_worker = std::jthread(std::move(task), at_roots, std::move(current), page, std::move(control));
        dirty = true;
        return true;
    };
    auto open_folder_browser = [&] {
        if (folder_browser_future.valid()) {
            configuration.notice = L"Wait for the previous check to finish.";
            dirty = true;
            return;
        }
        configuration.browser_open = true;
        configuration.browser_at_roots = true;
        configuration.browser_page = 0;
        configuration.browser_selected = 0;
        configuration.browser_error.clear();
        configuration.browser_location.clear();
        browser_current = nullptr;
        browser_stack.clear();
        start_folder_browser_load(true, nullptr, 0);
    };
    auto use_browser_folder = [&](StorageFolder folder) {
        const auto purpose = configuration.path_purpose;
        configuration.browser_open = false;
        configuration.browser_loading = false;
        configuration.browser_entries.clear();
        configuration.browser_error.clear();
        browser_current = nullptr;
        browser_stack.clear();
        if (purpose == FolderPathPurpose::Games) {
            configuration.notice = L"Folder selected. Adding games to the library...";
            start_scan(std::move(folder));
        } else {
            configuration.notice = L"Folder selected. Importing files...";
            begin_import(std::move(folder), purpose == FolderPathPurpose::Keys ? 1U : 2U);
        }
        dirty = true;
    };
    auto go_up_in_folder_browser = [&] {
        if (browser_stack.empty()) {
            browser_current = nullptr;
            start_folder_browser_load(true, nullptr, 0);
        } else {
            browser_current = browser_stack.back();
            browser_stack.pop_back();
            start_folder_browser_load(false, browser_current, 0);
        }
    };
    auto activate_folder_browser_selection = [&] {
        if (configuration.browser_loading) return;
        const size_t selected_row = configuration.browser_selected;
        if (configuration.browser_at_roots) {
            if (selected_row >= configuration.browser_entries.size()) return;
            browser_current = configuration.browser_entries[selected_row].folder;
            browser_stack.clear();
            start_folder_browser_load(false, browser_current, 0);
            return;
        }
        if (selected_row == 0) {
            use_browser_folder(browser_current);
        } else if (selected_row == 1) {
            go_up_in_folder_browser();
        } else {
            const size_t child = selected_row - 2;
            if (child >= configuration.browser_entries.size()) return;
            browser_stack.push_back(browser_current);
            browser_current = configuration.browser_entries[child].folder;
            start_folder_browser_load(false, browser_current, 0);
        }
    };
    auto page_folder_browser = [&](bool next) {
        if (configuration.browser_loading) return;
        const size_t page = configuration.browser_page;
        if (next) {
            if (!configuration.browser_has_more) return;
            start_folder_browser_load(configuration.browser_at_roots, browser_current,
                                      page + FolderBrowserPageSize);
        } else if (page >= FolderBrowserPageSize) {
            start_folder_browser_load(configuration.browser_at_roots, browser_current,
                                      page - FolderBrowserPageSize);
        }
    };
    auto begin_folder_path_resolution = [&] {
        auto path = NormalizeExternalFolderPath(configuration.path_text);
        if (path.empty()) {
            configuration.path_error = L"Enter a folder path.";
            dirty = true;
            return;
        }
        if (path.size() > FolderPathInputLimit) {
            configuration.path_error = L"The path exceeds the 1,024-character limit.";
            dirty = true;
            return;
        }
        if (!IsAllowedAbsoluteFolderPath(path)) {
            configuration.path_error = L"Enter an absolute path such as D:\\Games or \\\\server\\folder.";
            dirty = true;
            return;
        }
        if (folder_path_future.valid()) {
            configuration.path_error = L"The previous check is still running. Wait for it or cancel it.";
            dirty = true;
            return;
        }
        configuration.path_text = path;
        configuration.path_cursor = path.size();
        configuration.path_error.clear();
        configuration.path_resolving = true;
        folder_path_cancelled = false;
        const bool unc = path.starts_with(L"\\\\");
        auto control = std::make_shared<FolderResolutionControl>();
        folder_path_control = control;
        std::packaged_task<FolderPathResolution(std::stop_token, std::wstring, bool,
                                                 std::shared_ptr<FolderResolutionControl>)> task{
            [](std::stop_token stop, std::wstring requested_path, bool is_unc,
               std::shared_ptr<FolderResolutionControl> operation_control) {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                FolderPathResolution result;
                try {
                    result.folder = ResolveFolderPath(requested_path, stop, operation_control);
                    result.cancelled = stop.stop_requested() || operation_control->IsCancelled() || !result.folder;
                    if (result.cancelled) result.folder = nullptr;
                } catch (const FolderPathFailure& error) {
                    result.error = error.message;
                } catch (const winrt::hresult_error& error) {
                    result.cancelled = stop.stop_requested() || operation_control->IsCancelled();
                    if (!result.cancelled) result.error = FolderPathError(error, is_unc);
                } catch (...) {
                    result.cancelled = stop.stop_requested() || operation_control->IsCancelled();
                    if (!result.cancelled) result.error = L"Could not open the folder. Check the path and permissions.";
                }
                return result;
            }};
        folder_path_future = task.get_future();
        folder_path_worker = std::jthread(std::move(task), std::move(path), unc, std::move(control));
        dirty = true;
    };
    auto activate_folder_path_key = [&] {
        if (configuration.path_resolving || configuration.path_key >= FolderPathKeyboardKeyCount) return;
        const auto label = FolderPathKeyboardKey(configuration.path_key, configuration.path_symbols);
        if (label.empty()) return;
        if (label == L"Add") { begin_folder_path_resolution(); return; }
        if (label == L"Backspace") { erase_path_before_cursor(); return; }
        if (label == L"Left") { move_path_cursor(false); return; }
        if (label == L"Right") { move_path_cursor(true); return; }
        if (label == L"Symbols") { configuration.path_symbols = true; dirty = true; return; }
        if (label == L"Letters") { configuration.path_symbols = false; dirty = true; return; }
        if (label == L"Caps") { configuration.path_uppercase = !configuration.path_uppercase; dirty = true; return; }
        if (label == L"Clear") {
            configuration.path_text.clear(); configuration.path_cursor = 0;
            configuration.path_error.clear(); dirty = true; return;
        }
        if (label == L"Space") { insert_path_text(L" "); return; }
        insert_path_text(FolderPathKeyboardInput(configuration.path_key, configuration.path_symbols,
                                                  configuration.path_uppercase));
    };
    auto move_folder_path_key = [&](int columns, int rows) {
        const auto& layout = configuration.path_symbols ? FolderPathSymbolsKeyboard : FolderPathLettersKeyboard;
        const auto current_row = static_cast<int>(configuration.path_key / FolderPathKeyboardColumns);
        const auto current_column = static_cast<int>(configuration.path_key % FolderPathKeyboardColumns);
        const int target_row = std::clamp(current_row + rows, 0, static_cast<int>(FolderPathKeyboardRows) - 1);
        const int target_column = std::clamp(current_column + columns, 0, static_cast<int>(FolderPathKeyboardColumns) - 1);
        if (rows == 0) {
            for (int step = 0; step < static_cast<int>(FolderPathKeyboardColumns); ++step) {
                const int candidate = target_column + (columns < 0 ? -step : step);
                if (candidate < 0 || candidate >= static_cast<int>(FolderPathKeyboardColumns)) continue;
                if (!layout[target_row][candidate].empty()) {
                    configuration.path_key = static_cast<size_t>(target_row * FolderPathKeyboardColumns + candidate);
                    dirty = true;
                    return;
                }
            }
        } else {
            for (int distance = 0; distance < static_cast<int>(FolderPathKeyboardColumns); ++distance) {
                const int left = target_column - distance;
                const int right = target_column + distance;
                if (left >= 0 && !layout[target_row][left].empty()) {
                    configuration.path_key = static_cast<size_t>(target_row * FolderPathKeyboardColumns + left);
                    dirty = true;
                    return;
                }
                if (right < static_cast<int>(FolderPathKeyboardColumns) && !layout[target_row][right].empty()) {
                    configuration.path_key = static_cast<size_t>(target_row * FolderPathKeyboardColumns + right);
                    dirty = true;
                    return;
                }
            }
        }
    };
    start_scan();
    GamepadButtons previous{};
    StickNavigation stick_navigation;
    unsigned navigation_context = 0;
    std::optional<ControllerDevice> pad;
    auto next_pad_check = std::chrono::steady_clock::now();
    while (true) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        const auto now = std::chrono::steady_clock::now();
        if (!panel.keyboard.notice.empty() && now >= keyboard_notice_until) {
            panel.keyboard.notice.clear(); dirty = true;
        }
        if (!panel.notice.empty() && now >= controller_notice_until) {
            panel.notice.clear(); dirty = true;
        }
        if (panel.keyboard.capturing && now >= capture_until) {
            panel.keyboard.capturing = false; dirty = true;
        }
        if (now >= next_pad_check) {
            RefreshProControllers();
            auto devices = EnumerateControllers();
            if (!SameControllerList(panel.devices, devices)) {
                panel.devices = std::move(devices);
                panel.choice = std::min(panel.choice, panel.devices.size() + 1);
                // An open list must not apply a stale row after hotplug/reorder.
                if (panel.expanded) {
                    panel.expanded = false;
                    actions &= ~Play;
                }
                dirty = true;
            }
            const auto index = SelectController(panel.devices, options.controller_id);
            const auto next = index ? std::optional<ControllerDevice>{panel.devices[*index]} : std::nullopt;
            if (!pad || !next || pad->id != next->id) {
                previous = {}; stick_navigation.Reset();
            }
            pad = next;
            next_pad_check = now + std::chrono::milliseconds{500};
        }
        const unsigned context = configuration.browser_open ? 6 : configuration.path_entry_open ? 5 : panel.keyboard.open ? 3 :
                                 panel.expanded ? 2 : settings ? 1 : configuration.open ? 4 : 0;
        if (context != navigation_context) {
            stick_navigation.Reset(); navigation_context = context;
        }
        if (pad) {
            try {
                const auto reading = ReadController(*pad).gamepad;
                const auto buttons = reading.Buttons;
                const auto edges = buttons & ~previous;
                previous = buttons;
                auto hit = [&](GamepadButtons b) { return (edges & b) == b; };
                // Back must remain available while editing/capturing keyboard input.
                // Track all edges, but suppress other pad actions inside that modal.
                if (hit(GamepadButtons::B)) actions |= Quit;
                if (!panel.keyboard.open) {
                    // The d-pad takes precedence; do not combine opposite directions.
                    constexpr auto dpad = GamepadButtons::DPadUp | GamepadButtons::DPadDown |
                                          GamepadButtons::DPadLeft | GamepadButtons::DPadRight;
                    if ((buttons & dpad) != GamepadButtons::None) stick_navigation.Reset();
                    else {
                        switch (stick_navigation.Update(reading.LeftThumbstickX, reading.LeftThumbstickY, now)) {
                        case StickDirection::Up: actions |= Up; break;
                        case StickDirection::Down: actions |= Down; break;
                        case StickDirection::Left: actions |= Left; break;
                        case StickDirection::Right: actions |= Right; break;
                        case StickDirection::None: break;
                        }
                    }
                    if (hit(GamepadButtons::DPadUp)) actions |= Up;
                    if (hit(GamepadButtons::DPadDown)) actions |= Down;
                    if (hit(GamepadButtons::DPadLeft)) actions |= Left;
                    if (hit(GamepadButtons::DPadRight)) actions |= Right;
                    if (hit(GamepadButtons::A)) actions |= Play;
                    if (!configuration.path_entry_open && !configuration.browser_open) {
                        if (hit(GamepadButtons::X)) {
                            if (settings) actions |= Swap;
                            else if (!configuration.open) actions |= AddFolder;
                        }
                        if (hit(GamepadButtons::Y)) actions |= Zone;
                        if (hit(GamepadButtons::Menu)) actions |= Refresh;
                        if (hit(GamepadButtons::View)) actions |= Panel;
                    }
                }
            } catch (...) { pad.reset(); previous = {}; stick_navigation.Reset(); }
        }
        if (window_closed || QuitRequested()) {
            if (folder_path_control) {
                folder_path_control->Cancel();
                folder_path_worker.request_stop();
            }
            if (folder_browser_control) {
                folder_browser_control->Cancel();
                folder_browser_worker.request_stop();
            }
            return std::nullopt;
        }
        if (picker) {
            actions = 0; // The system picker owns input until it returns.
            if (picker.Status() != winrt::Windows::Foundation::AsyncStatus::Started) {
                try {
                    auto folder = picker.GetResults();
                    if (folder && picker_kind == 0) start_scan(folder);
                    else if (folder) begin_import(folder, picker_kind);
                    else { configuration.notice = L"Selection cancelled."; dirty = true; }
                } catch (const winrt::hresult_error& e) {
                    configuration.notice = L"Could not select the folder: " + std::wstring{e.message()}; dirty = true;
                }
                picker = nullptr;
                previous = {};
                stick_navigation.Reset();
            }
        }
        if (validation.valid()) {
            actions = 0;
            if (validation.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
                try {
                    const auto error = validation.get();
                    if (error.empty()) return selected_path;
                    notice = winrt::to_hstring(error).c_str();
                } catch (const std::exception& e) { notice = winrt::to_hstring(e.what()).c_str(); }
                dirty = true;
            }
        }
        if (setup_future.valid() && setup_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try { configuration.status = setup_future.get(); }
            catch (...) { configuration.notice = L"Could not check file setup."; }
            configuration.selected = std::min(configuration.selected, ConfigurationRowCount(configuration) - 1);
            dirty = true;
        }
        if (configuration.busy) {
            if (actions & Quit) { import_worker.request_stop(); configuration.notice = L"Cancelling import..."; }
            actions = 0;
            const auto done = import_progress.completed.load(std::memory_order_relaxed);
            const auto total = import_progress.total.load(std::memory_order_relaxed);
            if (done != configuration.completed || total != configuration.total) {
                configuration.completed = done; configuration.total = total; dirty = true;
            }
            if (import_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
                try {
                    const auto error = import_future.get();
                    configuration.notice = error.empty() ? L"Import complete. Files are ready in internal storage." : std::wstring{winrt::to_hstring(error)};
                } catch (...) { configuration.notice = L"Import failed; the previous installation was kept."; }
                configuration.busy = false;
                // The old metadata worker has joined; discard results before rescanning.
                metadata = {};
                refresh_setup();
                start_scan();
                dirty = true;
            }
        }
        if (actions & Quit) {
            if (configuration.browser_open) {
                if (configuration.browser_loading && folder_browser_future.valid()) {
                    folder_browser_cancelled = true;
                    if (folder_browser_control) folder_browser_control->Cancel();
                    folder_browser_worker.request_stop();
                    configuration.browser_open = false;
                    configuration.browser_loading = false;
                    configuration.browser_entries.clear();
                    browser_current = nullptr;
                    browser_stack.clear();
                } else if (!configuration.browser_at_roots) {
                    go_up_in_folder_browser();
                } else {
                    configuration.browser_open = false;
                    configuration.browser_entries.clear();
                    browser_current = nullptr;
                    browser_stack.clear();
                }
                dirty = true; actions &= ~Quit;
            }
            else if (configuration.path_entry_open) {
                if (configuration.path_resolving && folder_path_future.valid()) {
                    folder_path_cancelled = true;
                    if (folder_path_control) folder_path_control->Cancel();
                    folder_path_worker.request_stop();
                }
                configuration.path_entry_open = false;
                configuration.path_resolving = false;
                configuration.path_text.clear();
                configuration.path_error.clear();
                dirty = true; actions &= ~Quit;
            }
            else if (panel.keyboard.open) { panel.keyboard.open = panel.keyboard.capturing = false; dirty = true; actions &= ~Quit; }
            else if (panel.expanded) { panel.expanded = false; dirty = true; actions &= ~Quit; }
            else if (settings) { settings = false; configuration.open = return_to_configuration; return_to_configuration = false; dirty = true; actions &= ~Quit; }
            else if (configuration.open) {
                if (configuration.files) { configuration.files = false; configuration.selected = 0; }
                else configuration.open = false;
                dirty = true; actions &= ~Quit;
            }
            else return std::nullopt;
        }
        if (folder_path_future.valid() && folder_path_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            FolderPathResolution result;
            try { result = folder_path_future.get(); }
            catch (...) { result.error = L"Could not check the folder."; }
            if (folder_path_worker.joinable()) folder_path_worker.join();
            folder_path_control.reset();
            if (!folder_path_cancelled && configuration.path_entry_open && configuration.path_resolving) {
                const auto purpose = configuration.path_purpose;
                configuration.path_resolving = false;
                if (result.folder) {
                    configuration.path_entry_open = false;
                    configuration.path_text.clear();
                    configuration.path_error.clear();
                    if (purpose == FolderPathPurpose::Games) {
                        configuration.notice = L"Folder is accessible. Adding games to the library...";
                        start_scan(result.folder);
                    } else {
                        configuration.notice = L"Folder is accessible. Importing files...";
                        begin_import(result.folder, purpose == FolderPathPurpose::Keys ? 1U : 2U);
                    }
                } else if (result.cancelled) {
                    configuration.path_entry_open = false;
                    configuration.path_text.clear();
                } else configuration.path_error = result.error.empty() ? L"Could not open the folder." : result.error;
                dirty = true;
            }
            folder_path_cancelled = false;
        }
        if (folder_browser_future.valid() &&
            folder_browser_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            FolderBrowserLoadResult result;
            try { result = folder_browser_future.get(); }
            catch (...) { result.error = L"Could not list this location."; }
            if (folder_browser_worker.joinable()) folder_browser_worker.join();
            folder_browser_control.reset();
            if (!folder_browser_cancelled && configuration.browser_open) {
                configuration.browser_loading = false;
                configuration.browser_at_roots = result.at_roots;
                configuration.browser_page = result.page;
                configuration.browser_entries = std::move(result.entries);
                configuration.browser_location = std::move(result.location);
                configuration.browser_error = std::move(result.error);
                configuration.browser_has_more = result.has_more;
                if (result.cancelled) configuration.browser_error.clear();
                const size_t row_count = configuration.browser_at_roots ? configuration.browser_entries.size() :
                                         2 + configuration.browser_entries.size();
                configuration.browser_selected = row_count ? std::min(configuration.browser_selected, row_count - 1) : 0;
                dirty = true;
            }
            folder_browser_cancelled = false;
        }
        if (configuration.browser_open) {
            if (configuration.browser_loading) {
                actions = 0;
            } else {
                const size_t count = configuration.browser_at_roots ? configuration.browser_entries.size() :
                                     2 + configuration.browser_entries.size();
                if (count && (actions & Up))
                    configuration.browser_selected = (configuration.browser_selected + count - 1) % count;
                if (count && (actions & Down))
                    configuration.browser_selected = (configuration.browser_selected + 1) % count;
                if (actions & Left) page_folder_browser(false);
                if (actions & Right) page_folder_browser(true);
                if (actions & Play) activate_folder_browser_selection();
                if (actions & (Up | Down)) dirty = true;
                actions = 0;
            }
        }
        if (configuration.path_entry_open) {
            if (!configuration.path_resolving) {
                if (actions & Up) move_folder_path_key(0, -1);
                else if (actions & Down) move_folder_path_key(0, 1);
                else if (actions & Left) move_folder_path_key(-1, 0);
                else if (actions & Right) move_folder_path_key(1, 0);
                if (actions & Play) activate_folder_path_key();
            }
            actions = 0;
        }
        if (actions & Panel && !configuration.open) { settings = !settings; panel.keyboard.open = panel.keyboard.capturing = false; panel.expanded = false; dirty = true; }
        if (actions & AddFolder && !settings && !picker && !configuration.busy && !validation.valid()) {
            configuration.open = true; configuration.files = false; configuration.selected = 0;
            configuration.notice.clear(); refresh_setup(); dirty = true;
        }
        if (configuration.open && !settings && !picker && !configuration.busy &&
            !configuration.path_entry_open && !configuration.browser_open) {
            const auto count = ConfigurationRowCount(configuration);
            if (actions & Up) configuration.selected = (configuration.selected + count - 1) % count;
            if (actions & Down) configuration.selected = (configuration.selected + 1) % count;
            if (actions & (Up | Down)) dirty = true;
            if (actions & Play) {
                if (!configuration.files) {
                    if (configuration.selected == 0) { configuration.files = true; configuration.selected = 0; }
                    else { settings = true; return_to_configuration = true; }
                } else if (loading || setup_future.valid()) configuration.notice = L"Wait for the file check to finish.";
                else if (configuration.selected >= 6) {
                    try {
                        ForgetGameFolder(configuration.status.sources[configuration.selected - 6].token);
                        configuration.notice = L"External folder removed from the library. Your games remain where they are.";
                        configuration.selected = 0; refresh_setup(); start_scan();
                    } catch (...) { configuration.notice = L"Could not remove the folder."; }
                } else if (configuration.selected == 1 || configuration.selected == 3 || configuration.selected == 5) {
                    if (folder_path_future.valid()) {
                        configuration.notice = L"The previous check is still running; wait for it to finish.";
                    } else {
                        configuration.path_entry_open = true;
                        configuration.path_resolving = false;
                        configuration.path_purpose = configuration.selected == 1 ? FolderPathPurpose::Games :
                            configuration.selected == 3 ? FolderPathPurpose::Keys : FolderPathPurpose::Firmware;
                        configuration.path_text.clear();
                        configuration.path_cursor = 0;
                        configuration.path_key = 0;
                        configuration.path_symbols = false;
                        configuration.path_uppercase = false;
                        configuration.path_error.clear();
                        configuration.notice.clear();
                    }
                } else {
                    picker_kind = static_cast<unsigned>(configuration.selected / 2);
                    configuration.path_purpose = picker_kind == 0 ? FolderPathPurpose::Games :
                        picker_kind == 1 ? FolderPathPurpose::Keys : FolderPathPurpose::Firmware;
                    if (panel.xbox) {
                        open_folder_browser();
                    } else {
                        try {
                            winrt::Windows::Storage::Pickers::FolderPicker folder_picker;
                            folder_picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                            folder_picker.FileTypeFilter().Append(L"*");
                            picker = folder_picker.PickSingleFolderAsync();
                            configuration.notice = picker_kind == 0 ? L"Choose the game folder." : picker_kind == 1 ?
                                L"Choose the folder with prod.keys and, optionally, title.keys." : L"Choose the folder with extracted firmware (.nca).";
                        } catch (...) { configuration.notice = L"Could not open the folder picker."; }
                    }
                }
                dirty = true;
            }
            actions = 0;
        }
        if (!settings) panel.notice.clear();
        if (!panel.keyboard.open) panel.keyboard.notice.clear();
        if (loading && future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try { scan = future.get(); }
            catch (const std::exception& e) { scan.error = e.what(); }
            catch (const winrt::hresult_error& e) { scan.error = winrt::to_string(e.message()); }
            loading = false;
            if (!configuration.busy) refresh_setup();
            selected = std::min(selected, scan.entries.empty() ? 0 : scan.entries.size() - 1);
            notice = winrt::to_hstring(scan.error).c_str();
            if (added_folder_pending) {
                configuration.notice = scan.error.empty() ? L"External folder added to the library." :
                    L"Could not finish registering or scanning: " + std::wstring{winrt::to_hstring(scan.error)};
                added_folder_pending = false;
            }
            if (notice.empty() && !initial_notice.empty()) notice = winrt::to_hstring(initial_notice).c_str();
            initial_notice = {};
            if (scan.limited) {
                notice = L"Scan incomplete: a source, entry, or depth limit was reached; some games may be missing.";
                if (!scan.error.empty()) {
                    notice += L" ";
                    notice += winrt::to_hstring(scan.error).c_str();
                }
            }
            dirty = true;
        }
        if (!auto_pick.empty() && !configuration.open && !loading && !picker && !validation.valid()) {
            if (auto_pick_at == std::chrono::steady_clock::time_point{})
                auto_pick_at = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            if (std::chrono::steady_clock::now() >= auto_pick_at) {
                for (size_t i = 0; i < scan.entries.size(); ++i) {
                    const auto path = scan.entries[i].relative_path.u8string();
                    if (scan.entries[i].launch_path == auto_pick ||
                        (!IsStoragePath(scan.entries[i].launch_path) &&
                         std::string_view{reinterpret_cast<const char*>(path.data()), path.size()} == auto_pick)) {
                        selected = i; settings = false; actions |= Play;
                    }
                }
            }
        }
        if (actions & Play && !configuration.open && !settings && !panel.keyboard.open && !loading && !scan.entries.empty()) {
            selected_path = scan.entries[selected].launch_path;
            notice = L"Checking access to the game...";
            dirty = true;
            std::packaged_task<std::string()> task{[path = selected_path] {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                if (!HasUsableHeaderKey() && !IsHomebrewNroPath(std::filesystem::path{path}))
                    return std::string{"Your keys are missing. Open Settings > File Manager > Import console keys."};
                return IsStoragePath(path) ? CheckExternalGame(path) : std::string{};
            }};
            validation = task.get_future();
            validation_worker = std::jthread(std::move(task));
            actions = 0;
        }
        if (settings) {
            if (panel.keyboard.open) { actions &= ~(Swap | Zone); }
            else if (panel.expanded) {
                const size_t count = panel.devices.size() + 2;
                if (actions & Up) panel.choice = (panel.choice + count - 1) % count;
                if (actions & Down) panel.choice = (panel.choice + 1) % count;
                if (actions & (Up | Down)) dirty = true;
                if (actions & Play) {
                    if (panel.choice < 2 || (bool(panel.devices[panel.choice - 2]) &&
                        !panel.devices[panel.choice - 2].id.starts_with(L"session:"))) {
                        options.controller_id = panel.choice == 0 ? L"" : panel.choice == 1 ?
                            std::wstring{KeyboardControllerId} : panel.devices[panel.choice - 2].id;
                        device_changed = true;
                        panel.expanded = false;
                        next_pad_check = now;
                    }
                    dirty = true;
                }
                actions &= ~(Swap | Zone);
            } else {
                // Rows: [device (PC)], controller type, A/B/X/Y, deadzone, [keyboard (PC)].
                const unsigned offset = panel.xbox ? 0 : 1;
                const unsigned rows = panel.xbox ? 3 : 5;
                if (actions & Up) setting_row = (setting_row + rows - 1) % rows;
                if (actions & Down) setting_row = (setting_row + 1) % rows;
                if (actions & (Up | Down)) dirty = true;
                if (actions & (Play | Left | Right)) {
                    if (!panel.xbox && setting_row == 4) {
                        panel.keyboard.open = true; panel.keyboard.capturing = false; dirty = true;
                    } else if (!panel.xbox && setting_row == 0) {
                        panel.expanded = true;
                        panel.choice = options.controller_id == KeyboardControllerId ? 1 : 0;
                        for (size_t i = 0; i < panel.devices.size(); ++i)
                            if (panel.devices[i].id == options.controller_id) panel.choice = i + 2;
                        dirty = true;
                    } else if (setting_row == offset) {
                        options.style = StepConsoleControllerStyle(options.style, (actions & Left) != 0);
                        device_changed = true; dirty = true;
                    } else actions |= setting_row == offset + 1 ? Swap : Zone;
                }
            }
        } else {
            actions &= ~(Swap | Zone);
            if (actions & Left && selected) { --selected; dirty = true; }
            if (actions & Right && selected + 1 < scan.entries.size()) { ++selected; dirty = true; }
            if (actions & Up && selected >= 5) { selected -= 5; dirty = true; }
            if (actions & Down && selected + 5 < scan.entries.size()) { selected += 5; dirty = true; }
        }
        if (actions & Swap) { options.swap_face_buttons = !options.swap_face_buttons; dirty = true; }
        if (actions & Zone) {
            options.deadzone = options.deadzone < 0.10f ? 0.12f : options.deadzone < 0.15f ? 0.18f : 0.08f;
            dirty = true;
        }
        if (device_changed || actions & (Swap | Zone)) {
            device_changed = false;
            const bool saved = SaveControllerOptions(options);
            panel.notice = saved ? L"Settings saved" : L"Could not save controller settings";
            controller_notice_until = saved ? now + std::chrono::seconds{3} :
                                             std::chrono::steady_clock::time_point::max();
        }
        if (actions & Refresh && !loading && !picker && !validation.valid()) start_scan();
        if (!configuration.busy && metadata.valid() && metadata.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try {
                auto batch = metadata.get();
                if (batch.generation == generation) {
                    for (auto& entry : batch.entries) {
                        auto found = std::find_if(scan.entries.begin(), scan.entries.end(), [&](const auto& e) {
                            return e.launch_path == entry.launch_path;
                        });
                        if (found != scan.entries.end()) *found = std::move(entry);
                    }
                    dirty = true;
                    metadata_dirty = true;
                }
            } catch (const std::exception&) { notice = L"Could not read metadata for some games."; }
        }
        const size_t page = selected / 5 * 5;
        if (!configuration.open && !loading && !metadata.valid() && !scan.entries.empty() &&
            (metadata_dirty || retained_page != page)) {
            metadata_dirty = false;
            std::vector<LibraryEntry> requested;
            const size_t first = selected / 5 * 5;
            if (retained_page != SIZE_MAX && retained_page != first) {
                for (size_t i = retained_page; i < std::min(retained_page + 5, scan.entries.size()); ++i) {
                    auto& entry = scan.entries[i];
                    if (!entry.icon.empty()) {
                        std::vector<unsigned char>{}.swap(entry.icon);
                        entry.metadata_loaded = false;
                    }
                }
            }
            retained_page = first;
            for (size_t i = first; i < std::min(first + 5, scan.entries.size()); ++i) {
                auto& entry = scan.entries[i];
                if (!entry.metadata_loaded) {
                    requested.push_back(entry);
                    entry.metadata_loaded = true;
                }
            }
            if (!requested.empty()) {
                if (!logging_ready) { Common::Log::Initialize(); logging_ready = true; }
                std::packaged_task<MetadataBatch(std::stop_token)> task{
                    [root, requested = std::move(requested), generation](std::stop_token stop) mutable {
                        winrt::init_apartment(winrt::apartment_type::multi_threaded);
                        struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                        ReadLibraryMetadata(root, requested, stop);
                        return MetadataBatch{std::move(requested), generation};
                    }};
                metadata = task.get_future();
                metadata_worker = std::jthread(std::move(task));
            }
        }
        actions = 0;
        if (resize) {
            canvas.reset();
            canvas = std::make_unique<LibraryCanvas>(core_window, width, height);
            resize = false;
            dirty = true;
        }
        if (dirty) { canvas->Draw(scan, selected, loading, settings, setting_row, options, notice, panel, configuration); dirty = false; }
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }
}
} // namespace EdenXbox
