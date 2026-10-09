# Preparing a community release

The Xbox binary is a signed UWP development package. Do not include private signing material,
Nintendo keys, firmware, or game dumps in source control or release assets.

## Prepare a draft

1. Run **Build Eden Xbox** on a reviewed source commit. The run must finish with both the UWP build
   and package/signing jobs successful. The package job validates the APPX signature and block map,
   manifest identity and architecture, Microsoft VCLibs dependency, shader/runtime files, checksums,
   and metadata.
2. Open **Actions → Prepare Xbox Release Draft → Run workflow**. Enter the successful build run ID
   and a new tag such as `v0.3.0-preview.1`.
3. The release workflow confirms the run belongs to this repository and `build-xbox.yml`, confirms
   the run succeeded and its package artifact is available, verifies the artifact checksums and
   source commit, then creates a **draft prerelease**. The bundle includes the signed APPX, public
   certificate, Microsoft VCLibs package, checksums, build metadata, bilingual user guides, and
   license. It contains no private key or user-supplied emulator data.
4. Review the draft tag, target commit, release notes, files, and checksum. Install the package on
   an Xbox in Developer Mode and perform a smoke test before publishing. For an external-storage
   release, test the Xbox folder picker with the storage types the release notes claim to support.
   A PC AppContainer test does not verify Xbox USB permissions, SMB/network visibility or
   authentication, or access to private app folders. Publishing is a separate maintainer action
   from the Releases page.

The workflow creates drafts only; it never publishes a release automatically. Use a new tag for
each package. Keep the signing identity stable so users can update without uninstalling and losing
their app data. The public `.cer` is included in the bundle; the private signing key and password
remain in GitHub Actions Secrets.

## User-facing package contents

The single ZIP asset is named `EdenXbox-<tag>.zip` and contains:

- `eden-xbox.appx`
- `eden-xbox.cer` (public development certificate)
- `Microsoft.VCLibs.x64.14.00.appx`
- `SHA256SUMS.txt` and `build-metadata.json`
- `README.md`, `README.en.md`, and `LICENSE.txt`
- `dist/qt_themes/default/icons/256x256/eden.png` for the guide's logo

The bilingual guides explain Dev Mode, local Device Portal installation, the app's exact LocalState
folders, firmware ZIP extraction during upload, and how users import their own keys, firmware, and
games. External game folders are selected through the Xbox system folder picker and persisted with
the permission Windows grants. The picker can access only locations it exposes and authorizes; do
not advertise arbitrary Xbox paths or SMB support as verified until tested on Xbox hardware.
