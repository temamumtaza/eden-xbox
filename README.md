<!--
SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

**Bahasa:** [English](README.en.md) · **Bahasa Indonesia**

<p align="center">
  <img src="dist/qt_themes/default/icons/256x256/eden.png" alt="Logo Eden" width="112">
</p>

<h1 align="center">Eden untuk Xbox</h1>

<p align="center">Panduan mengunduh, memasang, dan menyiapkan Eden di Xbox Series X|S melalui Developer Mode.</p>

> **Rilis komunitas: preview.** Eden Xbox masih dalam pengembangan. Pengujian yang dilaporkan untuk rilis ini mencakup satu uji coba *Wild West Crops* yang berjalan di Xbox Series S. Hasil itu bukan jaminan semua game kompatibel atau bebas masalah.

Antarmuka aplikasi pada build saat ini masih berbahasa Spanyol; nama menu di panduan mengikuti label yang muncul di Xbox.

## Sumber repositori

Repo ini adalah fork komunitas tidak resmi dari [JulianDr14/eden-xbox](https://github.com/JulianDr14/eden-xbox), repo sumber port Xbox. Proyek utama Eden dan sumber upstream-nya adalah [eden-emu/eden](https://git.eden-emu.dev/eden-emu/eden). Repo ini memelihara port Xbox, panduan Developer Mode, dan rilis komunitas; ini bukan kanal rilis resmi proyek Eden.

## Unduh dan pasang

1. Aktifkan **Developer Mode** di Xbox dan sambungkan Xbox serta komputer/ponsel ke jaringan lokal yang sama.
2. Buka [Releases](https://github.com/temamumtaza/eden-xbox/releases/latest), unduh `EdenXbox-...zip`, lalu ekstrak di komputer. Rilis berisi aplikasi, sertifikat publik, dependency Microsoft, checksum, dan panduan. Jika belum ada rilis di halaman itu, paket komunitas belum diterbitkan.
3. Di Xbox Dev Home, buka **Device Portal**. Dari browser di komputer, kunjungi `https://<IP-XBOX>:11443`.
4. Di Device Portal, buka **Apps → Install Certificate**, pilih `eden-xbox.cer`, lalu pasang sertifikat.
5. Buka **Apps → Deploy apps**. Pilih `eden-xbox.appx`, tambahkan `Microsoft.VCLibs.x64.14.00.appx` sebagai dependency, lalu tekan **Install**.
6. Di daftar aplikasi, atur Eden ke **Game** mode lalu jalankan. Jangan buka Device Portal ke internet publik atau meneruskan port router ke Xbox.

Jika pemasangan gagal karena paket tidak tepercaya, pastikan sertifikat dari ZIP rilis yang sama sudah dipasang. Untuk detail Device Portal, lihat [panduan Microsoft](https://learn.microsoft.com/en-us/windows/advanced-settings/device-portal).

## Siapkan keys, firmware, dan game milikmu

Gunakan browser Device Portal untuk menaruh berkas di penyimpanan Eden. Pada **File Explorer**, buka **User Folders → LocalAppData → paket Eden → LocalState**. Nama paket bisa berbeda menurut versi; pilih paket Eden yang terpasang. Folder berikut adalah lokasi data yang dibaca Eden, bukan folder sementara.

Di dalam `LocalState`, buat folder yang belum ada dengan **New Folder** hingga susunannya seperti ini:

```text
LocalState/
├── eden/
│   ├── keys/
│   │   ├── prod.keys
│   │   └── title.keys       (opsional)
│   └── nand/
│       └── system/
│           └── Contents/
│               └── registered/
│                   └── *.nca (hasil ekstrak ZIP firmware)
└── games/
    ├── game-1.nsp
    └── game-2.xci
```

### Pasang keys milikmu

Unggah `prod.keys` dan, jika tersedia, `title.keys` ke `LocalState/eden/keys/` lewat **File Explorer → Upload**. `prod.keys` wajib; `title.keys` opsional. Eden membaca keys dari lokasi ini saat dijalankan.

### Pasang firmware ZIP

1. Di Device Portal, buka `LocalState/eden/nand/system/Contents/registered/`, tekan **Upload**, lalu pilih ZIP firmware hasil dump milikmu.
2. Saat browser menawarkan opsi untuk mengekstrak ZIP, pilih atau centang **Extract** / **Extract after upload**. Tunggu sampai selesai.
3. Pastikan berkas `.nca` berada langsung di dalam folder `registered`. Jika ZIP membuat folder tambahan, pindahkan berkas `.nca` ke `registered`.
4. Pastikan keys sudah ada sebelum firmware digunakan.

Opsi **Extract** dilakukan oleh File Explorer Device Portal saat upload ZIP; Eden membaca file hasil ekstraksi dari folder standar di atas. Microsoft mendokumentasikan parameter ekstraksi untuk upload berkas melalui [API Device Portal](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core).

### Tambahkan game

Unggah dump game milikmu ke `LocalState/games/`, atau sambungkan USB yang berisi game. Untuk folder game di USB, pilih **Configuración → Gestor de archivos → Agregar carpeta de juegos**.

Setelah semua berkas selesai diunggah, tekan **B** dari pustaka Eden untuk keluar, lalu jalankan Eden lagi dari Dev Home. Di **Configuración → Gestor de archivos**, periksa bahwa status menunjukkan **Claves listas** dan jumlah file firmware.

### Catatan hukum dan keamanan

Gunakan hanya `prod.keys`, `title.keys`, firmware, dan dump game yang kamu buat dari Switch milikmu sendiri atau yang penggunaannya memang kamu izinkan. **Proyek ini tidak menyediakan keys, firmware, game, tautan unduhan bajakan, atau dukungan pembajakan. Gunakan dengan bijak dan patuhi hukum yang berlaku.** Jangan unggah berkas tersebut ke issue, forum, atau repository; berkas itu juga tidak disertakan di paket rilis.

## Pembaruan dan bantuan

- Pasang versi baru di atas versi lama menggunakan sertifikat rilis yang sama. Jangan menghapus Eden lebih dulu; data aplikasi biasanya dipertahankan saat pembaruan.
- Jika game belum muncul, tekan **Menu** untuk memperbarui pustaka, pastikan file ada di `LocalState/games/`, atau tambahkan folder USB dari menu Gestor de archivos.
- Jika keys belum terbaca, pastikan nama file tepat `prod.keys` dan berada langsung di `LocalState/eden/keys/`, lalu jalankan ulang Eden.
- Jika firmware belum terbaca, pastikan ZIP diekstrak oleh browser Device Portal dan file `.nca` berada langsung di `LocalState/eden/nand/system/Contents/registered/`.
- Untuk catatan teknis pengembangan, lihat [README developer di repository](https://github.com/temamumtaza/eden-xbox/blob/xbox/README-developer.md). Lisensi: [GPL-3.0-or-later](LICENSE.txt).
