#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <magnification.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

extern "C" {
BOOL WINAPI MagInitialize(void);
BOOL WINAPI MagUninitialize(void);
BOOL WINAPI MagSetFullscreenTransform(float magLevel, int x, int y);
}

namespace {

constexpr float kZoomLevel = 2.0f;
constexpr int kHotkeyToggle = 1;
constexpr int kHotkeyQuit = 2;
constexpr int kPollIntervalMs = 8;
constexpr float kSmoothing = 0.4f;

std::atomic<bool> g_running{true};
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_transformWarned{false};

template <typename Fn>
Fn LoadUser32Proc(const char* name) {
    static HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return nullptr;
    }
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(
        GetProcAddress(user32, name)));
}

void EnableDpiAwareness() {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI *)(HANDLE);
    if (auto setCtx =
            LoadUser32Proc<SetProcessDpiAwarenessContextFn>(
                "SetProcessDpiAwarenessContext")) {
        if (setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
            return;
        }
    }
    using SetProcessDPIAwareFn = BOOL(WINAPI *)();
    if (auto setLegacy = LoadUser32Proc<SetProcessDPIAwareFn>(
            "SetProcessDPIAware")) {
        setLegacy();
    }
}

void WarnTransformFailure(DWORD error) {
    if (!g_transformWarned.exchange(true)) {
        std::printf(
            "[!] MagSetFullscreenTransform GAGAL (GetLastError=%lu).\n"
            "    Jalankan aplikasi ini sebagai Administrator, atau gunakan\n"
            "    manifest dengan hak akses UIAccess.\n",
            static_cast<unsigned long>(error));
        std::fflush(stdout);
    }
}

void ApplyTransform(float level, int x, int y) {
    if (!MagSetFullscreenTransform(level, x, y)) {
        WarnTransformFailure(GetLastError());
    }
}

void MagnifierThread() {
    if (!MagInitialize()) {
        std::printf("[!] MagInitialize gagal (error %lu).\n",
                    static_cast<unsigned long>(GetLastError()));
        std::fflush(stdout);
    }

    float curX = 0.0f;
    float curY = 0.0f;
    bool tracking = false;

    while (g_running.load(std::memory_order_relaxed)) {
        if (!g_enabled.load(std::memory_order_relaxed)) {
            if (tracking) {
                ApplyTransform(1.0f, 0, 0);
                tracking = false;
            }
            Sleep(kPollIntervalMs * 2);
            continue;
        }

        POINT pt{};
        GetCursorPos(&pt);

        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        const float regionW = static_cast<float>(screenW) / kZoomLevel;
        const float regionH = static_cast<float>(screenH) / kZoomLevel;

        float targetX = static_cast<float>(pt.x) - regionW * 0.5f;
        float targetY = static_cast<float>(pt.y) - regionH * 0.5f;

        const float maxX = static_cast<float>(screenW) - regionW;
        const float maxY = static_cast<float>(screenH) - regionH;
        if (targetX < 0.0f) targetX = 0.0f;
        if (targetX > maxX) targetX = maxX;
        if (targetY < 0.0f) targetY = 0.0f;
        if (targetY > maxY) targetY = maxY;

        if (!tracking) {
            curX = targetX;
            curY = targetY;
            tracking = true;
        } else {
            curX += (targetX - curX) * kSmoothing;
            curY += (targetY - curY) * kSmoothing;
            if (std::fabs(targetX - curX) < 0.5f) curX = targetX;
            if (std::fabs(targetY - curY) < 0.5f) curY = targetY;
        }

        ApplyTransform(kZoomLevel, static_cast<int>(std::lround(curX)),
                       static_cast<int>(std::lround(curY)));
        Sleep(kPollIntervalMs);
    }

    if (tracking) {
        MagSetFullscreenTransform(1.0f, 0, 0);
    }
    MagUninitialize();
}

void PrintBanner() {
    std::printf("=== Zoom Pointer (Hardware-Accelerated Magnifier) ===\n");
    std::printf("  Ctrl+Alt+Z : toggle zoom %.1fx (cursor-follow panning)\n",
                kZoomLevel);
    std::printf("  Ctrl+Alt+Q : keluar & bersihkan memori\n");
    std::printf("Status: berjalan di background...\n");
    std::fflush(stdout);
}

}  // namespace

int main() {
    EnableDpiAwareness();

    const bool hotkeyToggleOk =
        RegisterHotKey(nullptr, kHotkeyToggle, MOD_CONTROL | MOD_ALT, 'Z');
    const bool hotkeyQuitOk =
        RegisterHotKey(nullptr, kHotkeyQuit, MOD_CONTROL | MOD_ALT, 'Q');

    if (!hotkeyToggleOk || !hotkeyQuitOk) {
        std::printf("[!] RegisterHotKey gagal (error %lu). Hotkey mungkin "
                    "dipakai aplikasi lain.\n",
                    GetLastError());
    }

    PrintBanner();

    std::thread magnifier(MagnifierThread);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == kHotkeyToggle) {
                const bool enable = !g_enabled.load();
                g_transformWarned.store(false);
                g_enabled.store(enable);
                std::printf("Zoom: %s (%.1fx)\n", enable ? "ON" : "OFF",
                            enable ? kZoomLevel : 1.0f);
                std::fflush(stdout);
            } else if (msg.wParam == kHotkeyQuit) {
                std::printf("Keluar...\n");
                std::fflush(stdout);
                g_running.store(false);
                PostQuitMessage(0);
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    if (magnifier.joinable()) {
        magnifier.join();
    }

    UnregisterHotKey(nullptr, kHotkeyToggle);
    UnregisterHotKey(nullptr, kHotkeyQuit);

    return 0;
}
