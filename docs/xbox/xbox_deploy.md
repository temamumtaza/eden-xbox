# Deploy Eden Xbox from macOS

The Windows Actions package is a signed development build for Xbox Series X|S Developer Mode. It contains no Nintendo keys, firmware, or games. Use [README-macos.md](../../README-macos.md) for the short workflow; this page covers the connection and troubleshooting details.

## Install or update

1. Download and unzip the eden-xbox-series-s artifact from a successful Build Eden Xbox run. It contains eden-xbox.appx, eden-xbox.cer, Microsoft.VCLibs.x64.14.00.appx, checksums, metadata, and the helper script.
2. Keep the Mac and Xbox on a trusted local network. Open Xbox Device Portal at https://<XBOX-IP>:11443. Do not forward this port or expose the portal to the public internet.
3. On first use, run the helper's trust command and confirm the displayed leaf-certificate SHA-256 fingerprint. The helper saves that exact certificate as a per-console trust anchor in the current macOS user's Application Support folder and requires the certificate to match the portal IP on later connections. Use replace-trust only after checking a changed certificate out of band.
4. Run the helper's deploy command with the artifact directory. It checks SHA256SUMS.txt, APPX identity/version/architecture, required D3D12 runtime files, and the build metadata before upload. It sends the app, VCLibs dependency, and public certificate to the Device Portal using verified HTTPS and the CSRF cookie-to-header flow. It polls deployment state and confirms the installed package version.
5. In the Device Portal Apps manager, set Eden to Game mode and launch it. Game mode matters for the emulator's memory reservation. The exact AppMemoryUsageLimit is recorded by the app and may vary by device mode and system state; use the value measured in the current console log rather than assuming a fixed limit.

The helper refuses a package whose version is not newer than the currently installed package. It never calls the uninstall API. A normal package update keeps the existing package identity and LocalState, so keys, firmware, games, and caches stored by the user remain on the console. Keep the old signed artifact until the update is accepted on-console.

The package was signed with this fork's persistent development certificate. Install the included public certificate if Device Portal requests it. The private signing key and password remain in GitHub Actions Secrets and are never included in the artifact.

## User data

After the app starts, import only keys and firmware you are authorized to use through Eden's UI. Add personally dumped games through the app's library or its LocalState games folder. The CI build and artifact do not contain any of those files. External storage and library behavior are documented in [xbox_rom_storage.md](xbox_rom_storage.md).

## Diagnostics

The app writes startup breadcrumbs to LocalState/eden_uwp_diag.txt. Eden's main log and graphics bug log are stored under LocalState/eden/log/eden_log.txt and LocalState/eden/log/eden_graphics_bugs.log. The helper's logs command downloads these files into a private local folder; analyze-logs prints summary counts and recorded memory measurements without printing raw log contents.

For early startup, check whether the diagnostic file recorded that the app entered its boot view and whether it returned an error. For renderer setup, inspect the local summary for D3D12 shader-path initialization and Render errors. A missing log can mean activation failed before Eden initialized. Do not share raw logs until checking for local paths, usernames, or other personal data.

## Deployment API and safeguards

The helper uses the documented Windows Device Portal package-install, state, installed-package, task-manager, and LocalAppData file endpoints. Its requests stay on the private LAN. On first use, confirm the Xbox's leaf-certificate SHA-256 fingerprint; each later HTTPS connection checks that exact certificate, its IP-address SAN, and its validity dates before sending a request. The helper also requires a session cookie and matching X-CSRF-Token header for writes. It does not store credentials, uninstall packages, or print session tokens.

Build/signature validation, deployment, startup, and game compatibility are separate checks. A successful Actions run validates the x64 APPX, package signature, certificate identity, Microsoft VCLibs signature, manifest capability/dependency, and required runtime DLL presence. It does not prove that the Xbox launched the app or that a game works. Record the actual console and Game mode result separately.
