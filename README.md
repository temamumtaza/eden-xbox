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

**Batas platform:** Eden mengemulasikan Nintendo Switch, bukan Wii. Pustaka tidak memindai dump Wii `.iso`/`.wbfs`; format yang dikenali saat ini adalah dump Switch `.nsp`/`.xci` dan aplikasi homebrew `.nro`. *Super Mario Galaxy 2* juga memiliki edisi Switch. Nama game saja tidak menentukan platform dump, dan kompatibilitas edisi Switch pada build Xbox ini belum diverifikasi.

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

Gunakan browser Device Portal untuk menaruh berkas di penyimpanan Eden. Pada **File Explorer**, buka **User Folders → LocalAppData → paket Eden → LocalState**. Nama paket bisa berbeda menurut versi; pilih paket Eden yang terpasang. Folder berikut adalah lokasi data yang dibaca Eden, bukan folder sementara. Keys dan firmware juga bisa diimpor lewat **Configuración → Gestor de archivos** jika browser Eden atau jalur absolut yang diizinkan Windows dapat membuka folder sumbernya.

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

Ada dua cara:

- Di Eden, buka **Configuración → Gestor de archivos → Importar claves de tu consola**, lalu pilih folder yang berisi `prod.keys` langsung di dalamnya. `title.keys` boleh ada di folder yang sama, tetapi opsional. Eden menyalin dan memuat ulang keys setelah impor berhasil.
- Atau unggah `prod.keys` dan, jika tersedia, `title.keys` ke `LocalState/eden/keys/` lewat **File Explorer → Upload**. Eden membaca keys dari lokasi ini saat dijalankan.

### Pasang firmware ZIP

1. Di Device Portal, buka `LocalState/eden/nand/system/Contents/registered/`, tekan **Upload**, lalu pilih ZIP firmware hasil dump milikmu.
2. Jika dialog upload menampilkan opsi ekstraksi, pilih atau centang **Extract** / **Extract after upload**. Tunggu sampai selesai.
3. Pastikan berkas `.nca` berada langsung di dalam folder `registered`. Jika ZIP membuat folder tambahan, pindahkan berkas `.nca` ke `registered`.
4. Pastikan keys sudah ada sebelum firmware digunakan.

API upload Device Portal mendukung parameter `extract=true`, tetapi tampilan checkbox di File Explorer portal dapat berbeda menurut versi dan belum diverifikasi pada semua konsol Xbox. Eden membaca file hasil ekstraksi dari folder standar di atas. Jika UI portal tidak menawarkan ekstraksi, ekstrak ZIP di komputer lalu unggah file `.nca` ke folder `registered`, atau gunakan impor firmware Eden di bawah.

Alternatifnya, ekstrak ZIP di komputer lebih dulu, lalu di Eden pilih **Configuración → Gestor de archivos → Importar firmware de tu consola** dan pilih folder yang berisi file `.nca` hasil ekstrak secara langsung. Impor firmware memerlukan keys yang valid.

### Tambahkan game

Kamu bisa memakai penyimpanan internal atau lokasi eksternal:

- **Penyimpanan internal:** unggah dump game milikmu ke `LocalState/games/` lewat File Explorer Device Portal.
- **USB atau folder jaringan:** di Eden buka **Configuración → Gestor de archivos**. Pada Xbox, **Buscar carpeta de juegos** membuka browser Eden untuk penyimpanan internal Eden, removable volume yang diekspos Windows, dan folder dengan izin tersimpan; di PC, pilihan itu memakai pemilih folder Windows. **Escribir ruta de juegos** untuk memasukkan jalur absolut seperti `D:\Juegos` atau `\\servidor\share\Juegos`. Jalur USB harus memakai huruf drive yang ditampilkan Windows; jalur SMB harus menunjuk share dan folder yang bisa dibuka Windows. Eden menyimpan izin folder yang diberikan Windows dan membaca game dari sana tanpa menyalin dump ke `LocalState`.

> Di Xbox, browser Eden hanya menampilkan penyimpanan internal Eden, removable volume yang diekspos Windows, dan folder dengan izin tersimpan; browser tidak dapat membuka sembarang Downloads atau folder privat aplikasi lain. Jalur USB/SMB tetap bergantung pada lokasi yang diizinkan Windows. Eden tidak menampilkan dialog login SMB dan tidak menyimpan kredensial. Share dengan autentikasi domain belum didukung karena paket tidak mendeklarasikan `enterpriseAuthentication`; akses SMB lainnya tetap bergantung pada izin Windows/Xbox. Perilaku `RemovableDevices`, UNC, dan pembacaan file pada perangkat Xbox masih perlu diuji. Jika lokasi eksternal tidak bisa dibuka, gunakan `LocalState/games/` melalui Device Portal.

Asosiasi tipe berkas pada paket diperlukan Windows untuk membatasi akses removable storage dan UNC ke `.nsp`, `.xci`, `.nro`, `.keys`, serta `.nca`. Gunakan alur folder di Eden untuk menambahkan game atau mengimpor file; membuka satu berkas langsung dari aplikasi lain belum didukung.

Pencarian eksternal mengenali `.nsp`, `.xci`, dan `.nro`, hingga 10.000 entri. Batas kedalaman menghitung folder yang dipilih sebagai tingkat pertama; simpan file di folder itu atau sampai empat subfolder di bawahnya.

Setelah impor atau unggahan selesai, periksa status keys dan jumlah file firmware di **Configuración → Gestor de archivos**. Jika game belum muncul, kembali ke pustaka lalu tekan **Menu** untuk memperbarui daftar.

### Catatan hukum dan keamanan

Gunakan hanya `prod.keys`, `title.keys`, firmware, dan dump game yang kamu buat dari Switch milikmu sendiri atau yang penggunaannya memang kamu izinkan. **Proyek ini tidak menyediakan keys, firmware, game, tautan unduhan bajakan, atau dukungan pembajakan. Gunakan dengan bijak dan patuhi hukum yang berlaku.** Jangan unggah berkas tersebut ke issue, forum, atau repository; berkas itu juga tidak disertakan di paket rilis.

## Pembaruan dan bantuan

- Pasang versi baru di atas versi lama menggunakan sertifikat rilis yang sama. Jangan menghapus Eden lebih dulu; data aplikasi biasanya dipertahankan saat pembaruan.
- Jika game belum muncul, tekan **Menu** untuk memperbarui pustaka. Untuk penyimpanan internal, pastikan file ada di `LocalState/games/`; untuk eksternal, pastikan drive/lokasi masih tersedia lalu pilih **Buscar carpeta de juegos** atau **Escribir ruta de juegos** di **Configuración → Gestor de archivos**.
- Jika game berada lebih dari empat subfolder di bawah folder yang dipilih atau hasil pemindaian melebihi 10.000 entri, pilih folder yang lebih dekat ke game atau kurangi isi folder yang dipindai.
- Jika keys belum terbaca, pastikan nama file tepat `prod.keys` dan berada langsung di `LocalState/eden/keys/`, lalu jalankan ulang Eden.
- Jika firmware belum terbaca, pastikan ZIP diekstrak oleh browser Device Portal dan file `.nca` berada langsung di `LocalState/eden/nand/system/Contents/registered/`.
- Untuk catatan teknis pengembangan, lihat [README developer di repository](https://github.com/temamumtaza/eden-xbox/blob/xbox/README-developer.md). Lisensi: [GPL-3.0-or-later](LICENSE.txt).
