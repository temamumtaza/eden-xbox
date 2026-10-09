<!--
SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

**Language:** **English** · [Bahasa Indonesia](README.md)

<p align="center">
  <img src="dist/qt_themes/default/icons/256x256/eden.png" alt="Eden logo" width="112">
</p>

<h1 align="center">Eden for Xbox</h1>

<p align="center">A beginner guide to download, install, and set up Eden on Xbox Series X|S in Developer Mode.</p>

> **Community preview.** Eden for Xbox is still in development. Testing reported for this release includes one *Wild West Crops* smoke test running on an Xbox Series S. That result does not guarantee compatibility or stability for other games.

The current app interface is still in Spanish. This guide shows the Spanish menu labels as they appear on Xbox.

## Source repositories

This repository is an unofficial community fork of [JulianDr14/eden-xbox](https://github.com/JulianDr14/eden-xbox), the Xbox port source repository. The main Eden project and upstream source are [eden-emu/eden](https://git.eden-emu.dev/eden-emu/eden). This repo maintains the Xbox port, Developer Mode guide, and community releases; it is not the official Eden project's release channel.

## Download and install

1. Enable **Developer Mode** on your Xbox. Connect the Xbox and your computer/phone to the same local network.
2. Open [Releases](https://github.com/temamumtaza/eden-xbox/releases/latest), download `EdenXbox-...zip`, and extract it on your computer. The release bundle contains the app, public certificate, Microsoft dependency, checksums, and guides. If there is no release on that page yet, the community package has not been published.
3. Open **Device Portal** from Xbox Dev Home. In a browser on your computer, visit `https://<XBOX-IP>:11443`.
4. In Device Portal, open **Apps → Install Certificate**, select `eden-xbox.cer`, and install it.
5. Open **Apps → Deploy apps**. Select `eden-xbox.appx`, add `Microsoft.VCLibs.x64.14.00.appx` as a dependency, then choose **Install**.
6. In the app list, set Eden to **Game** mode and launch it. Do not expose Device Portal to the public internet or forward your router port to the Xbox.

If installation reports an untrusted package, make sure you installed the certificate from the same release ZIP. See [Microsoft's Device Portal guide](https://learn.microsoft.com/en-us/windows/advanced-settings/device-portal) for more information.

## Set up your own keys, firmware, and games

Use the Device Portal browser to place files in Eden's storage. In **File Explorer**, open **User Folders → LocalAppData → Eden package → LocalState**. The package name can vary by version; select the installed Eden package. These are the data folders Eden reads, not temporary staging folders. You can also import keys and firmware through **Configuración → Gestor de archivos** when the Xbox folder picker can access their source folder.

Inside `LocalState`, use **New Folder** to create any missing folders until the layout looks like this:

```text
LocalState/
├── eden/
│   ├── keys/
│   │   ├── prod.keys
│   │   └── title.keys       (optional)
│   └── nand/
│       └── system/
│           └── Contents/
│               └── registered/
│                   └── *.nca (extracted from your firmware ZIP)
└── games/
    ├── game-1.nsp
    └── game-2.xci
```

### Add your keys

There are two ways to add them:

- In Eden, open **Configuración → Gestor de archivos → Importar claves de tu consola**, then select a folder with `prod.keys` directly inside it. You can place the optional `title.keys` in the same folder. Eden copies and reloads the keys after a successful import.
- Or upload `prod.keys` and, if available, `title.keys` to `LocalState/eden/keys/` using **File Explorer → Upload**. Eden reads keys from this location when it starts.

### Add a firmware ZIP

1. In Device Portal, open `LocalState/eden/nand/system/Contents/registered/`, choose **Upload**, and select your firmware dump ZIP.
2. When the browser offers to extract the ZIP, choose or check **Extract** / **Extract after upload**. Wait for extraction to finish.
3. Make sure the `.nca` files are directly inside the `registered` folder. If the ZIP creates an extra folder, move the `.nca` files into `registered`.
4. Make sure your keys are in place before using the firmware.

The Device Portal File Explorer extracts the ZIP during upload; Eden reads the extracted files from the standard folder above. Microsoft documents the upload extraction option in the [Device Portal API reference](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core).

Alternatively, extract the ZIP on your computer first. In Eden, choose **Configuración → Gestor de archivos → Importar firmware de tu consola** and select the folder containing the extracted `.nca` files directly. A valid key set is required before importing firmware.

### Add games

Choose internal or external storage:

- **Internal storage:** upload your own game dumps to `LocalState/games/` using Device Portal File Explorer.
- **USB or network folder:** connect the USB drive, or make the network location available to Xbox. In Eden, open **Configuración → Gestor de archivos → Agregar carpeta de juegos**, then choose the game folder in the Xbox system folder picker. Eden saves the permission Windows grants and reads games from that location without copying the dumps into `LocalState`. Keep the drive or location available while Eden uses it.

> The folder picker only grants access to locations Windows displays and permits. An SMB/network folder can be used only if it appears in that picker and Xbox can open it; network visibility, authentication, and behavior on Xbox hardware have not been verified. Eden cannot bypass the Windows sandbox to open arbitrary private app folders or locations the system has not granted. If an external location does not appear, use `LocalState/games/` through Device Portal.

External scans recognize `.nsp`, `.xci`, and `.nro`, up to 10,000 entries. The depth limit counts the selected folder as the first level; keep game files in that folder or up to four subfolders below it.

After importing or uploading, check the key status and firmware file count in **Configuración → Gestor de archivos**. If a game is missing, return to the library and press **Menu** to refresh the list.

### Legal and security notice

Only use `prod.keys`, `title.keys`, firmware, and game dumps made from a Switch you own or are authorized to use. **This project does not provide keys, firmware, games, piracy links, or piracy support. Use it responsibly and follow your local laws.** Do not upload those files to issues, forums, or this repository; they are not included in release packages.

## Updates and help

- Install a newer version over the existing one with the same release certificate. Do not uninstall Eden first; app data is normally preserved during an update.
- If a game is missing, press **Menu** to refresh the library. For internal storage, check `LocalState/games/`; for external storage, make sure the drive/location is still available and add it again through **Configuración → Gestor de archivos → Agregar carpeta de juegos**.
- If a game is more than four subfolders below the selected folder or the scan exceeds 10,000 entries, choose a folder closer to the game or reduce the number of items being scanned.
- If keys are not detected, check that the filename is exactly `prod.keys` and that it is directly inside `LocalState/eden/keys/`, then relaunch Eden.
- If firmware is not detected, extract the ZIP in the Device Portal browser and make sure `.nca` files are directly inside `LocalState/eden/nand/system/Contents/registered/`.
- For engineering details, see the [developer README in the repository](https://github.com/temamumtaza/eden-xbox/blob/xbox/README-developer.md). License: [GPL-3.0-or-later](LICENSE.txt).
