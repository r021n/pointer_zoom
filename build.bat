@echo off
setlocal
cd /d "%~dp0"

echo [1/3] Kompilasi resource manifest...
windres zoom_app.rc -O coff -o zoom_app_res.o
if errorlevel 1 (
    echo [GAGAL] windres
    exit /b 1
)
copy /Y zoom_app_res.o lib\default-manifest.o >nul

echo [2/3] Kompilasi + linking main.cpp...
g++ main.cpp zoom_app_res.o -o zoom_app.exe -Blib/ -Llib -lmagnification -luser32 -O2 -std=c++17 -Wall -Wextra -static-libgcc -static-libstdc++
if errorlevel 1 (
    echo [GAGAL] g++
    exit /b 1
)

echo [3/3] Verifikasi manifest requireAdministrator...
findstr /c:"requireAdministrator" zoom_app.exe >nul
if errorlevel 1 (
    echo [GAGAL] manifest requireAdministrator tidak ada di zoom_app.exe
    exit /b 1
)
findstr /c:"asInvoker" zoom_app.exe >nul
if not errorlevel 1 (
    echo [GAGAL] manifest default asInvoker masih ikut ter-link
    exit /b 1
)

echo.
echo BUILD OK: zoom_app.exe
echo Jalankan: zoom_app.exe  (akan muncul prompt UAC - pilih Yes)
exit /b 0
