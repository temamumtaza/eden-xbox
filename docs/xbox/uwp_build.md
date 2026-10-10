# Building Eden for Xbox UWP

This branch builds the current Eden UWP frontend and its native Direct3D 12 renderer for Xbox Series X|S Developer Mode. The earlier Phase 2 headless-boot description is historical and no longer describes the package produced here.

## Build from macOS

Use GitHub Actions; no local Windows installation or VM is needed. Open Actions in the personal fork, choose Build Eden Xbox, and run it on the xbox branch. Leave Source ref blank for the current branch tip, or provide a commit SHA from this fork to reproduce or roll back source. The workflow downloads dependencies, configures the UWP toolchain, builds Eden and Mesa's SPIR-V-to-DXIL runtime, signs and validates the APPX, and publishes a 30-day install artifact.

The workflow caches CPM dependencies, the UWP Ninja tree, and Mesa's build tree. It first restores
a cache with the same configuration fingerprint, then falls back to the older SDK 26100 cache
format. When that legacy cache is selected, the workflow preserves CPM and the UWP tree but wipes
Mesa's build tree once because its configuration cannot be verified. On a matching v2 cache,
CMake and Meson reconfigure their existing trees and Ninja rebuilds changed files, including the
linked shader wrapper. The restore action exposes the matched cache key for this check ([restore
outputs](https://github.com/actions/cache/blob/v6.1.0/restore/action.yml)). Use `clean_build` only
when you need a clean configuration; it skips the caches and configures Mesa from scratch. Meson
documents the difference between [reconfiguring and wiping a build directory](https://mesonbuild.com/Configuring-a-build-directory.html).

The short install steps are in [README-macos.md](../../README-macos.md); signing, cache, rollback, and scheduled upstream maintenance are in [ci_macos_maintenance.md](ci_macos_maintenance.md).

## Capture one D3D12 guest shader

The `workflow_dispatch` input `dump_shader_hash` accepts one 16-digit VS or PS hash from a D3D12 pipeline log. When reusing a successful unsigned build, set `unsigned_run_id` to that build and set `package_version_override` to a version newer than the Xbox's installed package; the package job then reuses the executable and translator without running the native build. It adds `dump_shader=<hash>` to the packaged `boot.cfg`, and the package metadata records the selected hash. Leaving the input blank preserves the normal package.

When Eden builds a pipeline containing that shader, it writes `shader_<hash>_<stage>.ir.txt` and `shader_<hash>_<stage>.spv` under `LocalState/eden/log`. Capture one hash per run; use the guest shader hash shown after `VS` or `PS`, not the D3D12 pipeline key. These files contain shader code derived from the selected title; keep them private and out of source control. The standard `eden-xbox-macos.py logs` command collects the regular log files; use `tools/xbox/download-shader-dump.py --portal https://<xbox-ip>:11443 --shader-hash <16-hex-hash>` to fetch the matching IR/SPIR-V files over the same pinned TLS connection. The downloader checks all five possible stage suffixes and writes only into a private local Downloads subfolder.

## Windows toolchain used by CI

The hosted Windows 2022 runner uses Visual Studio 2022 with the v143 x64 UWP tools, Windows SDK 10.0.26100.0, Store CRT, CMake 3.31 or newer, Ninja, native Strawberry Perl, NASM, Python 3, and glslangValidator 16.6.0. SDK 26100 supplies the D3D12 feature declarations used by the current renderer; its runtime feature queries fall back when the Xbox OS does not expose an optional feature. The workflow checks the selected SDK and Store CRT before configuration. The SDK supplies MakeAppx, SignTool, and dxil.dll; the UWP Extension SDK supplies Microsoft.VCLibs.x64.14.00.appx.

The source CMakeLists requires CMake 3.31. The uwp-x64 preset selects WindowsStore, Release, UWP AppContainer Dynarmic settings, and the Xbox-specific frontend options. It does not use the old null-renderer-only build description.

## Optional local Windows build

A local Windows build is useful for development but is not required for release. From cmd.exe at the repository root, set the requested SDK and invoke the checked environment wrapper:

    set EDEN_WINDOWS_SDK_VERSION=10.0.26100.0
    tools\xbox\build-env.bat cmake --preset uwp-x64 -DCMAKE_SYSTEM_VERSION=10.0.26100.0
    tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp

The wrapper selects Visual Studio's UWP x64 Store CRT, applies the selected SDK, and checks that the environment did not silently fall back to desktop CRT. Use cmd.exe or PowerShell, not Git Bash. If the project root is a clean checkout without an SDK pin, set EDEN_WINDOWS_SDK_VERSION before invoking it.

Build Mesa's shader translator outside the repository with PowerShell:

    powershell -ExecutionPolicy Bypass -File tools\xbox\build-spirv-to-dxil.ps1

The script pins the Mesa archive hash and its Python build tools. Mesa and its DLL stay in the sibling mesa-build directory and are never committed. The strict CI package fails if either spirv_to_dxil.dll or the matching Windows SDK dxil.dll is absent.

## Output and runtime requirements

The signed install artifact contains eden-xbox.appx, eden-xbox.cer, Microsoft.VCLibs.x64.14.00.appx, SHA256SUMS.txt, and build-metadata.json. The APPX contains the app, the Mesa translation DLL, dxil.dll, and the runtime DLLs emitted next to eden-uwp.exe. The manifest targets x64 Windows.Universal, declares codeGeneration and Microsoft.VCLibs.140.00, and keeps the publisher aligned with the development signing certificate.

The CI validator uses the pinned Windows SDK's MakeAppx and SignTool, checks the signed package and Microsoft-signed VCLibs, inspects the package manifest and x64 PE files, and rejects packaged private keys, firmware, and game dumps. A passing build does not establish console startup or game compatibility; those need a Series S/X test in the actual Game mode configuration.
