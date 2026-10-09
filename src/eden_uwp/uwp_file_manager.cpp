// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_file_manager.h"
#include "eden_uwp/uwp_async.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"

namespace EdenXbox {
namespace {
using namespace winrt::Windows::Storage;
using winrt::Windows::Storage::AccessCache::StorageApplicationPermissions;
constexpr unsigned PageSize = 64;
constexpr unsigned MaxFirmwareFiles = 4096;
std::mutex file_manager_mutex;
std::wstring BrokerPath(std::filesystem::path path) {
    return path.make_preferred().wstring();
}
void RequireInternalDestination(const std::filesystem::path& destination) {
    auto base = BrokerPath(std::filesystem::path{std::wstring{ApplicationData::Current().LocalFolder().Path()}});
    auto path = BrokerPath(destination.lexically_normal());
    const auto lower = [](wchar_t c) { return std::towlower(c); };
    std::transform(base.begin(), base.end(), base.begin(), lower);
    std::transform(path.begin(), path.end(), path.begin(), lower);
    if (!path.starts_with(base + L"\\")) throw std::runtime_error("Destino fuera del almacenamiento de la app.");
}

// Directory publication is confined to the app's internal keys/system-content folders.
// Keep the previous installation until all source files have been copied and checked.
bool RecoverDirectory(const std::filesystem::path& destination) {
    RequireInternalDestination(destination);
    const auto previous = destination.parent_path() / (destination.filename().wstring() + L".previous");
    if (std::filesystem::exists(previous)) {
        if (!std::filesystem::exists(destination)) {
            std::filesystem::rename(previous, destination);
            return true;
        }
        else std::filesystem::remove_all(previous);
    }
    return false;
}
bool IsImportStageName(std::wstring_view name, std::wstring_view destination_name) {
    std::wstring prefix{destination_name};
    prefix += L".import";
    if (name == prefix) return true;
    if (!name.starts_with(prefix) || name.size() <= prefix.size() + 3) return false;
    const auto suffix = name.substr(prefix.size());
    if (!suffix.starts_with(L" (") || suffix.back() != L')') return false;
    const auto number = suffix.substr(2, suffix.size() - 3);
    return !number.empty() && std::all_of(number.begin(), number.end(), [](wchar_t c) {
        return c >= L'0' && c <= L'9';
    });
}
void RemoveStaleImportStages(const std::filesystem::path& destination) {
    RequireInternalDestination(destination);
    const auto parent = destination.parent_path();
    std::error_code error;
    for (std::filesystem::directory_iterator it{parent, error}, end; !error && it != end; it.increment(error)) {
        std::error_code type_error;
        if (!it->is_directory(type_error) || type_error ||
            !IsImportStageName(it->path().filename().wstring(), destination.filename().wstring()))
            continue;
        std::error_code remove_error;
        std::filesystem::remove_all(it->path(), remove_error);
        if (remove_error)
            LOG_WARNING(Frontend, "File manager: stale import staging cleanup failed path={} error={}",
                        it->path().string(), remove_error.message());
    }
    if (error)
        LOG_WARNING(Frontend, "File manager: staging directory enumeration failed path={} error={}",
                    parent.string(), error.message());
}
void PublishDirectory(StorageFolder stage, const std::filesystem::path& destination) {
    const auto previous = destination.parent_path() / (destination.filename().wstring() + L".previous");
    RecoverDirectory(destination);
    const bool exists = std::filesystem::exists(destination);
    if (exists) std::filesystem::rename(destination, previous);
    try {
        stage.RenameAsync(destination.filename().wstring(), NameCollisionOption::FailIfExists).get();
    } catch (...) {
        if (exists) std::filesystem::rename(previous, destination);
        throw;
    }
    // Failed cleanup is retried by ReadFileSetupStatus on the next library startup.
    std::error_code error;
    if (exists) std::filesystem::remove_all(previous, error);
}
std::filesystem::path FirmwarePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system/Contents/registered";
}
bool ValidProductionKeys(const std::filesystem::path& file) {
    // Match Eden's text format, but reject a missing/zero/duplicated XTS header key
    // before replacing working data. Never log key material.
    std::ifstream input(file);
    std::string line;
    while (std::getline(input, line)) {
        line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }), line.end());
        std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!line.starts_with("header_key=")) continue;
        const auto key = std::string_view{line}.substr(11);
        return key.size() == 64 && std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isxdigit(c); }) &&
               key.substr(0, 32) != key.substr(32);
    }
    return false;
}
}

bool HasUsableHeaderKey() {
    auto& keys = Core::Crypto::KeyManager::Instance();
    if (!keys.HasKey(Core::Crypto::S256KeyType::Header)) return false;
    const auto key = keys.GetKey(Core::Crypto::S256KeyType::Header);
    return !std::equal(key.begin(), key.begin() + 16, key.begin() + 16);
}

FileSetupStatus ReadFileSetupStatus() {
    std::lock_guard lock{file_manager_mutex};
    const auto keys = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    const bool restored_keys = RecoverDirectory(keys);
    const auto firmware = FirmwarePath();
    RecoverDirectory(firmware);
    // Serialize with imports so only abandoned folders from a previous run are removed.
    RemoveStaleImportStages(keys);
    RemoveStaleImportStages(firmware);
    if (restored_keys) Core::Crypto::KeyManager::Instance().ReloadKeys();
    FileSetupStatus result;
    result.keys_ready = HasUsableHeaderKey();
    std::error_code error;
    for (std::filesystem::directory_iterator it{FirmwarePath(), error}, end; !error && it != end; it.increment(error)) {
        auto extension = it->path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t c) { return std::towlower(c); });
        if (it->is_regular_file(error) && extension == L".nca") ++result.firmware_files;
    }
    for (const auto& entry : StorageApplicationPermissions::FutureAccessList().Entries()) {
        const auto token = winrt::to_string(entry.Token);
        if (!token.starts_with("eden-games-")) continue;
        auto name = std::filesystem::path{std::wstring{entry.Metadata}}.filename().wstring();
        result.sources.push_back({token, name.empty() ? L"Carpeta USB" : name});
    }
    return result;
}
void ForgetGameFolder(std::string_view token) {
    if (!token.starts_with("eden-games-")) throw std::invalid_argument("Fuente de juegos no valida.");
    StorageApplicationPermissions::FutureAccessList().Remove(winrt::to_hstring(token));
}

std::string ImportSystemFiles(const StorageFolder& source, FileImport kind,
                             FileImportProgress& progress, std::stop_token stop) {
    std::lock_guard lock{file_manager_mutex};
    StorageFolder stage{nullptr};
    try {
        const bool keys = kind == FileImport::Keys;
        if (!keys && !HasUsableHeaderKey()) return "Primero importa tus claves de consola (prod.keys).";
        const auto destination = keys ? Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir) : FirmwarePath();
        RecoverDirectory(destination);
        std::filesystem::create_directories(destination.parent_path());
        auto parent = StorageFolder::GetFolderFromPathAsync(BrokerPath(destination.parent_path())).get();
        stage = parent.CreateFolderAsync(destination.filename().wstring() + L".import",
                                         CreationCollisionOption::GenerateUniqueName).get();
        std::vector<StorageFile> files;
        if (keys) {
            files.push_back(AwaitStorageOperation(source.GetFileAsync(L"prod.keys"), stop));
            if (auto title = AwaitStorageOperation(source.TryGetItemAsync(L"title.keys"), stop))
                files.push_back(title.as<StorageFile>());
            // Preserve unrelated/generated key files in a complete staged directory.
            if (std::filesystem::exists(destination)) {
                auto existing = AwaitStorageOperation(StorageFolder::GetFolderFromPathAsync(BrokerPath(destination)), stop);
                for (const auto& file : AwaitStorageOperation(existing.GetFilesAsync(), stop)) {
                    if (file.Name() != L"prod.keys" && (files.size() == 1 || file.Name() != L"title.keys"))
                        AwaitStorageOperation(file.CopyAsync(stage, file.Name(), NameCollisionOption::FailIfExists), stop);
                }
            }
        } else {
            for (unsigned start = 0;; start += PageSize) {
                auto page = AwaitStorageOperation(
                    source.GetFilesAsync(Search::CommonFileQuery::DefaultQuery, start, PageSize), stop);
                for (const auto& file : page) {
                    std::wstring extension{file.FileType()};
                    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return std::towlower(c); });
                    if (extension == L".nca") files.push_back(file);
                    if (files.size() > MaxFirmwareFiles) throw std::runtime_error("Demasiados archivos de firmware.");
                }
                if (page.Size() < PageSize) break;
                if (start >= 16384) throw std::runtime_error("Elige una carpeta que contenga solo tu firmware.");
                if (stop.stop_requested()) throw std::runtime_error("Importacion cancelada.");
            }
            if (files.empty()) throw std::runtime_error("La carpeta no contiene firmware .nca extraido.");
        }
        progress.total.store(static_cast<unsigned>(files.size()), std::memory_order_relaxed);
        bool system_version = false, mii_editor = false;
        FileSys::RealVfsFilesystem vfs;
        for (const auto& file : files) {
            if (stop.stop_requested()) throw std::runtime_error("Importacion cancelada; se conserva lo anterior.");
            const auto bytes = AwaitStorageOperation(file.GetBasicPropertiesAsync(), stop).Size();
            if (keys && bytes > 4 * 1024 * 1024) throw std::runtime_error("Archivo de claves demasiado grande.");
            auto copied = AwaitStorageOperation(file.CopyAsync(stage, file.Name(), NameCollisionOption::FailIfExists), stop);
            if (AwaitStorageOperation(copied.GetBasicPropertiesAsync(), stop).Size() != bytes)
                throw std::runtime_error("Copia incompleta.");
            if (!keys) {
                const auto path = std::filesystem::path{std::wstring{copied.Path()}};
                const auto u8path = path.u8string();
                auto raw = vfs.OpenFile(std::string{reinterpret_cast<const char*>(u8path.data()), u8path.size()}, FileSys::OpenMode::Read);
                FileSys::NCA nca{raw};
                if (nca.GetStatus() != Loader::ResultStatus::Success) throw std::runtime_error("Firmware no legible con estas claves; no se reemplaza lo instalado.");
                const auto id = nca.GetTitleId();
                if ((id >> 16) != (0x0100000000000000ULL >> 16)) throw std::runtime_error("La carpeta contiene un NCA de juego, no de firmware.");
                system_version |= id == 0x0100000000000809ULL && nca.GetType() == FileSys::NCAContentType::Data;
                mii_editor |= id == 0x0100000000001009ULL && nca.GetType() == FileSys::NCAContentType::Program;
            }
            progress.completed.fetch_add(1, std::memory_order_relaxed);
        }
        if (keys && !ValidProductionKeys(std::filesystem::path{std::wstring{stage.Path()}} / "prod.keys"))
            throw std::runtime_error("prod.keys no contiene una header_key valida.");
        if (!keys && (!system_version || !mii_editor)) throw std::runtime_error("Firmware incompleto: falta SystemVersion o MiiEdit.");
        if (stop.stop_requested()) throw std::runtime_error("Importacion cancelada; se conserva lo anterior.");
        PublishDirectory(stage, destination);
        stage = nullptr;
        if (keys) Core::Crypto::KeyManager::Instance().ReloadKeys();
        LOG_INFO(Frontend, "File manager: imported {} files ({})", files.size(), keys ? "keys" : "firmware");
        return {};
    } catch (const winrt::hresult_error& e) {
        LOG_ERROR(Frontend, "File manager: import failed HRESULT={:08X}", static_cast<unsigned>(e.code().value));
        if (stage) { try { stage.DeleteAsync().get(); } catch (...) {} }
        if (stop.stop_requested()) return "Importacion cancelada; se conserva lo anterior.";
        return "No se pudo importar. Comprueba la carpeta, el espacio libre y la conexion del USB.";
    } catch (const std::exception& e) {
        if (stage) { try { stage.DeleteAsync().get(); } catch (...) {} }
        return e.what();
    }
}

bool RunFileManagerGate(const std::function<void(std::string)>& diagnostic) {
    using namespace winrt::Windows::Storage;
    const auto old_keys = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    const auto old_nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    SCOPE_EXIT {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, old_keys);
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, old_nand);
        Core::Crypto::KeyManager::Instance().ReloadKeys();
    };
    std::string step = "prepare fixture";
    try {
        auto local = ApplicationData::Current().LocalFolder();
        auto fixture = local.CreateFolderAsync(L"file-manager-gate", CreationCollisionOption::GenerateUniqueName).get();
        // The opt-in gate leaves its tiny fixture for inspection; never changes live data.
        auto key_source = StorageFolder::GetFolderFromPathAsync(BrokerPath(old_keys)).get();
        auto firmware_source = fixture.CreateFolderAsync(L"source-firmware").get();
        FileSys::RealVfsFilesystem vfs;
        unsigned required = 0;
        for (const auto& file : std::filesystem::directory_iterator{FirmwarePath()}) {
            if (file.path().extension() != ".nca") continue;
            const auto path = file.path().u8string();
            FileSys::NCA nca{vfs.OpenFile(std::string{reinterpret_cast<const char*>(path.data()), path.size()}, FileSys::OpenMode::Read)};
            if (nca.GetStatus() != Loader::ResultStatus::Success) continue;
            if ((nca.GetTitleId() == 0x0100000000000809ULL && nca.GetType() == FileSys::NCAContentType::Data) ||
                (nca.GetTitleId() == 0x0100000000001009ULL && nca.GetType() == FileSys::NCAContentType::Program)) {
                auto source = StorageFile::GetFileFromPathAsync(BrokerPath(file.path())).get();
                source.CopyAsync(firmware_source).get();
                ++required;
            }
        }
        if (required < 2) throw std::runtime_error("Gate needs the user's installed firmware fixture.");
        const auto fixture_path = std::filesystem::path{std::wstring{fixture.Path()}};
        std::filesystem::create_directories(fixture_path / "keys");
        std::filesystem::create_directories(fixture_path / "nand");
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, fixture_path / "keys");
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, fixture_path / "nand");
        FileImportProgress progress;
        auto require = [](bool ok, const char* error) { if (!ok) throw std::runtime_error(error); };
        step = "keys import";
        const auto keys_error = ImportSystemFiles(key_source, FileImport::Keys, progress, {});
        if (!keys_error.empty()) throw std::runtime_error(keys_error);
        require(HasUsableHeaderKey(), "Imported keys not reloaded");
        step = "firmware import";
        const auto firmware_error = ImportSystemFiles(firmware_source, FileImport::Firmware, progress, {});
        if (!firmware_error.empty()) throw std::runtime_error(firmware_error);
        const auto before = ReadFileSetupStatus();
        require(before.keys_ready && before.firmware_files == required, "Setup status mismatch");

        step = "case-insensitive firmware status";
        const auto uppercase_nca = FirmwarePath() / "gate-case-check.NCA";
        std::ofstream{uppercase_nca, std::ios::binary}.put('\0');
        require(ReadFileSetupStatus().firmware_files == required + 1,
                "Uppercase NCA extension was not counted");
        std::filesystem::remove(uppercase_nca);
        require(ReadFileSetupStatus().firmware_files == required,
                "Firmware count did not recover after the extension check");

        step = "staging cleanup";
        const auto key_stage = fixture_path / "keys.import";
        const auto key_collision_stage = fixture_path / "keys.import (2)";
        const auto unrelated_key_folder = fixture_path / "keys.import-not-staging";
        const auto firmware_parent = FirmwarePath().parent_path();
        const auto firmware_stage = firmware_parent / "registered.import";
        const auto firmware_collision_stage = firmware_parent / "registered.import (3)";
        const auto unrelated_firmware_folder = firmware_parent / "registered.import-backup";
        for (const auto& path : {key_stage, key_collision_stage, unrelated_key_folder,
                                 firmware_stage, firmware_collision_stage, unrelated_firmware_folder})
            std::filesystem::create_directories(path);
        const auto after_cleanup = ReadFileSetupStatus();
        require(after_cleanup.keys_ready && after_cleanup.firmware_files == required,
                "Staging cleanup changed installed keys or firmware");
        require(!std::filesystem::exists(key_stage) && !std::filesystem::exists(key_collision_stage) &&
                !std::filesystem::exists(firmware_stage) && !std::filesystem::exists(firmware_collision_stage),
                "Recognized abandoned import staging was not removed");
        require(std::filesystem::exists(unrelated_key_folder) && std::filesystem::exists(unrelated_firmware_folder),
                "Staging cleanup removed a similarly named unrelated folder");
        ReadFileSetupStatus(); // Cleanup must be safe and idempotent.

        auto bad = fixture.CreateFolderAsync(L"invalid-keys").get();
        auto invalid = bad.CreateFileAsync(L"prod.keys").get();
        FileIO::WriteTextAsync(invalid, L"header_key=0000\n").get();
        require(!ImportSystemFiles(bad, FileImport::Keys, progress, {}).empty(), "Invalid keys accepted");
        require(ValidProductionKeys(fixture_path / "keys/prod.keys"), "Working keys replaced by invalid input");
        std::stop_source cancellation;
        cancellation.request_stop();
        require(!ImportSystemFiles(firmware_source, FileImport::Firmware, progress, cancellation.get_token()).empty(), "Cancelled import committed");
        require(ReadFileSetupStatus().firmware_files == required, "Cancellation changed installed firmware");
        const auto firmware = FirmwarePath();
        const auto previous = firmware.parent_path() / L"registered.previous";
        std::filesystem::rename(firmware, previous);
        require(ReadFileSetupStatus().firmware_files == required, "Interrupted publication recovery failed");
        diagnostic("File manager gate PASS: broker copies, NCA validation, key reload, invalid input, cancellation and interrupted publication recovery; live data preserved");
        return true;
    } catch (const std::exception& e) {
        diagnostic("File manager gate FAIL " + step + ": " + std::string{e.what()});
    } catch (const winrt::hresult_error& e) {
        diagnostic("File manager gate FAIL " + step + " HRESULT=" + std::to_string(e.code().value));
    }
    return false;
}
} // namespace EdenXbox
