================================================================================
WORKFLOW PENGEMBANGAN APLIKASI ZOOM POINTER (C++ & MinGW-w64)
================================================================================

## [TAHAP 1: PERSIAPAN ENVIRONMENT & STRUKTUR FOLDER]

1. Buka Terminal / CMD dan pastikan MinGW terdeteksi di PATH:

   > g++ --version

2. Buat folder workspace proyek dan buka di VS Code:

   > mkdir WindowsZoomPointer
   > cd WindowsZoomPointer
   > code .

3. Pastikan ekstensi "C/C++" (oleh Microsoft) sudah terpasang di VS Code.

## [TAHAP 2: PEMBUATAN FILE SOURCE CODE]

1. Buat file baru bernama `main.cpp` di root folder.
2. Isi file tersebut dengan modul utama:
   - Header Win32: <windows.h>, <magnification.h>
   - Inisialisasi API: MagInitialize()
   - DPI Awareness: SetProcessDpiAwarenessContext()
   - Hotkey Registration: RegisterHotKey(..., MOD_CONTROL | MOD_ALT, 'Z')
   - Background Thread (std::thread): Polling GetCursorPos() & MagSetFullscreenTransform()
   - Message Loop: GetMessage() untuk menangkap event WM_HOTKEY
   - Cleanup: MagSetFullscreenTransform(1.0f, 0, 0), MagUninitialize(), UnregisterHotKey()

## [TAHAP 3: SETUP AUTOMATION BUILD DI VS CODE (OPSIONAL TAPI PRAKTIS)]

1. Tekan Ctrl+Shift+P -> pilih "Tasks: Configure Default Build Task".
2. Edit file `.vscode/tasks.json` agar me-link library Windows yang dibutuhkan:

   {
   "version": "2.0.0",
   "tasks": [
   {
   "type": "shell",
   "label": "Build Zoom App",
   "command": "g++",
   "args": [
   "-g",
   "${workspaceFolder}/main.cpp",
   "-o",
   "${workspaceFolder}/zoom_app.exe",
   "-lmagnification",
   "-luser32",
   "-O2"
   ],
   "group": {
   "kind": "build",
   "isDefault": true
   }
   }
   ]
   }

## [TAHAP 4: KOMPILASI & LINKING (VIA TERMINAL MANUAL)]

Jika tidak menggunakan tasks.json, jalankan perintah kompilasi langsung melalui build.bat atau terminal:

> .\build.bat

Atau via command manual:
> g++ main.cpp zoom_app_res.o -o zoom_app.exe -Blib/ -Llib -lmagnification -luser32 -lgdiplus -lgdi32 -lmfplat -lmfreadwrite -lmfuuid -lole32 -lcomdlg32 -O2 -std=c++17 -Wall -Wextra -static-libgcc -static-libstdc++

Penjelasan flag penting:

- -lmagnification : Melakukan linking ke magnification.dll bawaan Windows
- -luser32 : Melakukan linking ke fungsi kursor, DPI, dan hotkey
- -lgdiplus : Melakukan linking ke GDI+ untuk rendering pointer kursor antialiased
- -lgdi32 : Melakukan linking ke GDI untuk bitmap per-pixel alpha layering
- -lmfplat -lmfreadwrite -lmfuuid : Windows Media Foundation Sink Writer untuk merekam video native H.264 MP4 hardware/software terakselerasi
- -lole32 : Subsistem COM untuk inisialisasi Media Foundation
- -lcomdlg32 : Dialog native Windows "Simpan Sebagai..." (GetSaveFileName)
- -O2 : Optimasi compiler untuk responsivitas kursor 60 FPS dan capture video 30 FPS

## [TAHAP 5: RUNNING, TESTING & VERIFIKASI]

1. Jalankan binary hasil build:

   > .\zoom_app.exe

2. Uji fungsi tombol:
   - Tekan Ctrl + Alt + Z -> Layar zoom 2x dan bergerak mulus mengikuti kursor
   - Tekan Ctrl + Alt + Z lagi -> Layar reset ke ukuran normal (1.0x)
   - Tekan Ctrl + Alt + R -> Mulai merekam layar sesuai kondisi zoom (kursor & efek riak ikut terekam)
   - Tekan Ctrl + Alt + R lagi -> Menghentikan rekaman & membuka modal dialog Windows untuk memilih lokasi penyimpanan video (.mp4)
   - Tekan Ctrl + Alt + Q -> Keluar aplikasi dan membersihkan memori

3. Penanganan Izin (UAC):
   - Jika `MagSetFullscreenTransform` mengembalikan nilai FALSE / layar tidak membesar,
     jalankan terminal/VS Code dengan mode "Run as Administrator" atau gunakan file
     manifest `.exe` dengan hak akses requireAdministrator yang sudah diset di build.bat.
================================================================================
