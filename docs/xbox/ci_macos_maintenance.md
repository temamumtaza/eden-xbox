# macOS-to-Xbox build and release maintenance

## Build flow

The Build Eden Xbox workflow runs on a hosted Windows 2022 x64 runner. It checks out the selected fork revision and recursive submodules, pins Windows SDK 10.0.26100.0, verifies the UWP Store CRT, and installs the required Visual Studio UWP component, Strawberry Perl, NASM, and checksum-pinned glslangValidator. SDK 26100 supplies the D3D12 feature declarations used by the current renderer; runtime capability checks retain fallback behavior on older Xbox OS versions. CMake 3.31 or later and Ninja come from the Visual Studio installation.

The build reuses branch-scoped CPM, Ninja, and Mesa caches. Cache keys include the dependency/build-script inputs and source SHA; clean_build disables cache restore. Mesa sources and build outputs remain under the runner temporary directory. The script verifies the Mesa source archive before extraction and installs pinned Python build packages. spirv_to_dxil.dll is only staged inside the signed APPX; it is not committed or uploaded as a standalone binary.

The workflow configures with the uwp-x64 preset, builds eden-uwp, and creates an allowlisted unsigned payload. A separate job imports the persistent development certificate from GitHub Actions Secrets, packages the frontend in library mode, requires spirv_to_dxil.dll, dxil.dll, and Microsoft.VCLibs, signs with SHA-256 and an RFC 3161 timestamp, then temporarily adds the pinned public signer to the ephemeral runner's current-user TrustedPeople store so SignTool can validate the package. It also validates the manifest, architecture, dependency signature, and runtime files. The final artifact includes the APPX, public .cer, VCLibs APPX, SHA256SUMS.txt, build-metadata.json, README-macos.md, and the macOS helper. It contains no private key, Nintendo keys, firmware, or game dump.

To rerun signing after a build job succeeds but package signing fails, dispatch the workflow with Unsigned run ID set to the earlier run ID. The workflow checks that the selected run is from the same repository, workflow, and branch; confirms its UWP build job succeeded and its unsigned artifact is still available; then downloads that artifact. New build payloads carry their source commit, source run ID and number, and package version. For older push-run payloads without this metadata, the workflow derives the source commit and package version from the verified run record. It rejects older manual-dispatch payloads that do not carry provenance metadata.

## Signing identity

The development signer must keep the Publisher subject CN=EdenXboxDev and the same private key across updates. The bootstrap script creates a random password and sends the encrypted PFX, password, and public thumbprint directly to repository Actions Secrets. It refuses to replace an existing secret set, so it cannot silently rotate the identity used by an installed package. Never download or print the private PFX or password. The public certificate is included for Device Portal installation.

To provision this personal fork, run tools/xbox/bootstrap-signing-macos.sh from macOS after authenticating GitHub CLI to the fork. If reusing the script for a different repository, change its repository setting first. Secret values go directly from local temporary files to GitHub; the temporary directory is removed on exit.

## Versions, updates, and rollback

Each Actions run gets a monotonically increasing four-part APPX version in the 0.3 series. The repository manifest remains unchanged; the packaging script edits only its staged copy. Device Portal updates use the same package identity and publisher and do not uninstall the installed app. Windows package updates preserve LocalState.

To roll back a regression, rebuild the last known-good source commit through the current workflow. The new workflow run assigns it a version higher than the installed package, allowing it to replace the newer binary without deleting local data. Keep the prior signed artifact until the replacement launches and diagnostics are reviewed. Do not use the Device Portal uninstall endpoint for rollback.

## Upstream updates

The scheduled Sync upstream Xbox workflow compares JulianDr14/eden-xbox xbox with the fork's xbox branch. If upstream has new commits, it creates an automation/upstream-xbox-* branch in this fork and merges the upstream commit. CI runs the normal Windows build, package, and verification sequence for that branch. Only a successful package build opens a draft pull request against this fork's xbox branch. A merge conflict or failed build leaves the update branch available for inspection; it never opens a pull request against Eden or the upstream repository.

The sync workflow preserves this fork's workflow definitions on automated branches so new upstream workflow files do not gain access to signing secrets or GitHub write tokens. Review source changes and the build metadata before merging the draft PR.

## Console acceptance and memory

A successful Windows workflow proves compilation, package structure, signing, and dependency validation. It does not prove launch, Game Mode, Series S stability, game compatibility, or performance. Confirm those on the actual console. The app writes AppMemoryUsage and AppMemoryUsageLimit measurements into its diagnostics; use the measured limit from that device and mode rather than assuming a fixed Game Mode budget. The pipeline does not add expandedResources or claim Series S performance from Series X results.
