// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stop_token>
#include <winrt/Windows.Foundation.h>

namespace EdenXbox {
// Propagate worker cancellation into the active WinRT request. Checking a stop
// token only between .get() calls cannot interrupt a stalled USB/SMB operation.
template <typename TOperation>
auto AwaitStorageOperation(TOperation operation, std::stop_token stop) {
    auto info = operation.template as<winrt::Windows::Foundation::IAsyncInfo>();
    const auto cancel = [info]() noexcept {
        try { info.Cancel(); } catch (...) {}
    };
    std::stop_callback cancellation{stop, cancel};
    return operation.get();
}
} // namespace EdenXbox
