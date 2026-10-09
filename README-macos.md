# Eden Xbox from macOS

This fork builds the Xbox UWP package on a hosted Windows runner. No Windows PC, local Windows VM, Nintendo keys, firmware, or game dump is required for the build.

## Build and download

1. Open the fork on GitHub, choose Actions, select Build Eden Xbox, and run it on the xbox branch. Leave Source ref blank for the latest commit on that branch. The workflow builds the UWP app and Mesa shader runtime, signs and validates the APPX, and publishes an artifact for 30 days.
2. Download the artifact named eden-xbox-series-s-… and unzip it. It contains the signed APPX, public certificate, Microsoft VCLibs dependency, checksums, build metadata, this guide, and the helper script.
3. In Terminal, validate the download. Replace the path with the unzipped artifact folder:

       python3 eden-xbox-macos.py verify-artifact --artifact-dir "$HOME/Downloads/eden-xbox-series-s-folder"

## Install and launch

1. Keep the Xbox and Mac on the same trusted local network. Open Xbox Device Portal at https://<XBOX-IP>:11443. Do not expose Device Portal to the public internet.
2. Pin its TLS certificate once. The helper prints the certificate fingerprint and asks you to type it back. You can instead supply a fingerprint you checked separately:

       python3 eden-xbox-macos.py trust --portal https://<XBOX-IP>:11443

3. Install the package and its dependency from the unzipped artifact folder:

       python3 eden-xbox-macos.py deploy --portal https://<XBOX-IP>:11443 --artifact-dir "$HOME/Downloads/eden-xbox-series-s-folder"

   The helper verifies the listed SHA-256 hashes, confirms the package identity and runtime files, and uses the Device Portal session cookie and CSRF header over certificate-verified HTTPS. It never uninstalls the existing package. If Device Portal authentication is enabled, it asks for credentials in Terminal and hides password input; it does not save them.
4. In Device Portal, set Eden to Game mode, then launch it. The first launch opens the library; this artifact includes no keys, firmware, or games.
5. Pull startup logs when needed:

       python3 eden-xbox-macos.py logs --portal https://<XBOX-IP>:11443
       python3 eden-xbox-macos.py analyze-logs "$HOME/Downloads/EdenXboxLogs/eden_uwp_diag.txt" "$HOME/Downloads/EdenXboxLogs/eden_log.txt" "$HOME/Downloads/EdenXboxLogs/eden_graphics_bugs.log"

The log summary prints counts and memory measurements, not raw log contents. See docs/xbox/xbox_deploy.md for troubleshooting and Game Mode details.

## Rebuild or roll back

To rebuild an earlier source revision, run the workflow with its full commit SHA in Source ref. Each run receives a higher APPX version, so an older build can update the installed package while preserving its LocalState. Keep a known-good artifact until the replacement has passed your console check. See docs/xbox/ci_macos_maintenance.md for the CI and upstream-sync design.
