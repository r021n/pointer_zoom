#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>
#include <magnification.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

extern "C" {
BOOL WINAPI MagInitialize(void);
BOOL WINAPI MagUninitialize(void);
BOOL WINAPI MagSetFullscreenTransform(float magLevel, int x, int y);
BOOL WINAPI MagShowSystemCursor(BOOL fShowCursor);
}

namespace {

constexpr float kZoomLevel = 2.0f;
constexpr int kHotkeyToggle = 1;
constexpr int kHotkeyQuit = 2;
constexpr int kPollIntervalMs = 8;

// Platformer camera lag parameter (~180ms delay)
constexpr float kCameraSmoothTime = 0.18f;

// Custom animated pointer overlay dimensions (compact 64x64)
constexpr int kOverlaySize = 64;
constexpr float kTipOffsetX = 18.0f;
constexpr float kTipOffsetY = 18.0f;

std::atomic<bool> g_running{true};
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_transformWarned{false};

// Overlay layered window handles (owned and managed solely by MagnifierThread)
HWND g_overlayHwnd = nullptr;
HDC g_hdcMem = nullptr;
HBITMAP g_hDIB = nullptr;
HGDIOBJ g_oldBmp = nullptr;

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

double GetTimeSeconds() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
}

// Critically damped spring (Unity SmoothDamp style) for organic platformer camera follow
float SmoothDamp(float current, float target, float& currentVelocity, float smoothTime, float deltaTime) {
    smoothTime = (std::max)(0.0001f, smoothTime);
    float omega = 2.0f / smoothTime;
    float x = omega * deltaTime;
    float expVal = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = current - target;
    float originalTo = target;

    float temp = (currentVelocity + omega * change) * deltaTime;
    currentVelocity = (currentVelocity - omega * temp) * expVal;
    float output = target + (change + temp) * expVal;

    if ((originalTo - current > 0.0f) == (output > originalTo)) {
        output = originalTo;
        currentVelocity = 0.0f;
    }
    return output;
}

// Guaranteed 100% click-through window procedure: never intercepts clicks across any app
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            return FALSE;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool InitOverlayWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"PointerZoomOverlayClass";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // WS_DISABLED completely disables all input handling at user32 level.
    // WS_EX_TRANSPARENT instructs DWM to pass all hit-testing through to underlying windows.
    g_overlayHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"PointerZoomOverlayClass",
        L"PointerOverlay",
        WS_POPUP | WS_DISABLED,
        0, 0, kOverlaySize, kOverlaySize,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_overlayHwnd) return false;

    ShowWindow(g_overlayHwnd, SW_HIDE);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kOverlaySize;
    bmi.bmiHeader.biHeight = -kOverlaySize; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdcScreen = GetDC(nullptr);
    g_hdcMem = CreateCompatibleDC(hdcScreen);
    void* bits = nullptr;
    g_hDIB = CreateDIBSection(g_hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_oldBmp = SelectObject(g_hdcMem, g_hDIB);
    ReleaseDC(nullptr, hdcScreen);

    return true;
}

void CleanupOverlayWindow() {
    if (g_overlayHwnd) {
        ShowWindow(g_overlayHwnd, SW_HIDE);
        DestroyWindow(g_overlayHwnd);
        g_overlayHwnd = nullptr;
    }
    if (g_hdcMem) {
        if (g_oldBmp) SelectObject(g_hdcMem, g_oldBmp);
        if (g_hDIB) DeleteObject(g_hDIB);
        DeleteDC(g_hdcMem);
        g_hdcMem = nullptr;
        g_hDIB = nullptr;
        g_oldBmp = nullptr;
    }
}

void RenderPointerOverlay(POINT cursorPt, float scale, float waveProgress) {
    if (!g_overlayHwnd || !g_hdcMem) return;

    {
        Gdiplus::Graphics g(g_hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        // 1. Expanding click ripple at the hotspot tip
        if (waveProgress >= 0.0f && waveProgress <= 1.0f) {
            float radius = 3.0f + 14.0f * waveProgress;
            int alpha = static_cast<int>(180.0f * (1.0f - waveProgress));
            if (alpha > 0) {
                Gdiplus::Pen wavePen(Gdiplus::Color(alpha, 40, 160, 255), 2.0f * (1.0f - waveProgress * 0.5f));
                g.DrawEllipse(&wavePen, kTipOffsetX - radius, kTipOffsetY - radius, radius * 2.0f, radius * 2.0f);
            }
        }

        // 2. Base pointer coordinates relative to Tip (0, 0)
        const Gdiplus::PointF basePoints[7] = {
            {0.0f, 0.0f},       // Tip (Hotspot)
            {0.0f, 21.0f},      // Left edge
            {5.2f, 16.2f},      // Inner crook
            {9.2f, 24.2f},      // Tail outer edge
            {12.8f, 22.4f},     // Tail right edge
            {8.8f, 14.5f},      // Outer crook
            {15.0f, 14.5f}      // Right wing
        };

        Gdiplus::PointF arrowPts[7];
        Gdiplus::PointF shadowPts[7];
        constexpr float shadowOffX = 1.5f;
        constexpr float shadowOffY = 2.0f;

        for (int i = 0; i < 7; ++i) {
            float sx = basePoints[i].X * scale;
            float sy = basePoints[i].Y * scale;
            arrowPts[i] = Gdiplus::PointF(kTipOffsetX + sx, kTipOffsetY + sy);
            shadowPts[i] = Gdiplus::PointF(kTipOffsetX + sx + shadowOffX, kTipOffsetY + sy + shadowOffY);
        }

        // Soft drop shadow
        Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(75, 0, 0, 0));
        g.FillPolygon(&shadowBrush, shadowPts, 7);

        // Crisp white pointer body
        Gdiplus::SolidBrush whiteBrush(Gdiplus::Color(255, 255, 255, 255));
        g.FillPolygon(&whiteBrush, arrowPts, 7);

        // Dark charcoal outer border
        Gdiplus::Pen outlinePen(Gdiplus::Color(255, 25, 25, 28), 1.8f);
        outlinePen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPolygon(&outlinePen, arrowPts, 7);
    }

    POINT ptSrc = { 0, 0 };
    POINT ptDst = {
        cursorPt.x - static_cast<int>(kTipOffsetX),
        cursorPt.y - static_cast<int>(kTipOffsetY)
    };
    SIZE size = { kOverlaySize, kOverlaySize };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC hdcScreen = GetDC(nullptr);
    UpdateLayeredWindow(g_overlayHwnd, hdcScreen, &ptDst, &size, g_hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, hdcScreen);
}

void MagnifierThread() {
    if (!MagInitialize()) {
        std::printf("[!] MagInitialize gagal (error %lu).\n",
                    static_cast<unsigned long>(GetLastError()));
        std::fflush(stdout);
    }

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    if (!InitOverlayWindow(hInstance)) {
        std::printf("[!] InitOverlayWindow gagal (error %lu).\n", GetLastError());
    }

    float curX = 0.0f;
    float curY = 0.0f;
    float velX = 0.0f;
    float velY = 0.0f;
    bool tracking = false;

    // Pointer animation state
    float curScale = 1.0f;
    float scaleVel = 0.0f;
    float clickPulseTimer = 0.0f;
    float waveTimer = -1.0f;
    bool lastPressed = false;

    double lastTime = GetTimeSeconds();

    while (g_running.load(std::memory_order_relaxed)) {
        // Pump local window messages for overlay
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        double now = GetTimeSeconds();
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;
        dt = std::clamp(dt, 0.001f, 0.050f);

        if (!g_enabled.load(std::memory_order_relaxed)) {
            if (tracking) {
                ApplyTransform(1.0f, 0, 0);
                MagShowSystemCursor(TRUE);
                if (g_overlayHwnd) {
                    ShowWindow(g_overlayHwnd, SW_HIDE);
                }
                tracking = false;
                velX = 0.0f;
                velY = 0.0f;
                curScale = 1.0f;
                scaleVel = 0.0f;
                clickPulseTimer = 0.0f;
                waveTimer = -1.0f;
                lastPressed = false;
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
        targetX = std::clamp(targetX, 0.0f, maxX);
        targetY = std::clamp(targetY, 0.0f, maxY);

        if (!tracking) {
            curX = targetX;
            curY = targetY;
            velX = 0.0f;
            velY = 0.0f;
            tracking = true;

            MagShowSystemCursor(FALSE);
            if (g_overlayHwnd) {
                ShowWindow(g_overlayHwnd, SW_SHOWNOACTIVATE);
            }
        } else {
            // Platformer camera follow with smooth dampening lag
            curX = SmoothDamp(curX, targetX, velX, kCameraSmoothTime, dt);
            curY = SmoothDamp(curY, targetY, velY, kCameraSmoothTime, dt);

            if (curX <= 0.0f || curX >= maxX) velX = 0.0f;
            if (curY <= 0.0f || curY >= maxY) velY = 0.0f;

            curX = std::clamp(curX, 0.0f, maxX);
            curY = std::clamp(curY, 0.0f, maxY);
        }

        ApplyTransform(kZoomLevel, static_cast<int>(std::lround(curX)),
                       static_cast<int>(std::lround(curY)));

        // --- Fast Async Mouse Click Detection (No Hooks, Zero Latency) ---
        const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        const bool middleDown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        const bool isPressed = leftDown || rightDown || middleDown;

        if (isPressed && !lastPressed) {
            clickPulseTimer = 0.09f; // 90ms impulse hold
            waveTimer = 0.0f;        // Trigger ripple
        }
        lastPressed = isPressed;

        bool isDepressed = isPressed;
        if (clickPulseTimer > 0.0f) {
            clickPulseTimer -= dt;
            isDepressed = true;
        }

        // Pointer shrinks to 0.68x when clicked, returns to 1.0x with spring bounce
        const float targetScale = isDepressed ? 0.68f : 1.0f;
        constexpr float kSpringStiffness = 460.0f;
        constexpr float kSpringDamping = 26.0f;
        float force = kSpringStiffness * (targetScale - curScale);
        scaleVel += force * dt;
        scaleVel -= kSpringDamping * scaleVel * dt;
        curScale += scaleVel * dt;

        float waveProgress = -1.0f;
        if (waveTimer >= 0.0f) {
            waveTimer += dt;
            waveProgress = waveTimer / 0.22f; // 220ms ripple
            if (waveProgress > 1.0f) {
                waveTimer = -1.0f;
                waveProgress = -1.0f;
            }
        }

        RenderPointerOverlay(pt, curScale, waveProgress);

        Sleep(kPollIntervalMs);
    }

    if (tracking) {
        MagSetFullscreenTransform(1.0f, 0, 0);
        MagShowSystemCursor(TRUE);
    }
    CleanupOverlayWindow();
    MagUninitialize();
}

void PrintBanner() {
    std::printf("=== Zoom Pointer (Hardware-Accelerated Magnifier) ===\n");
    std::printf("  Ctrl+Alt+Z : toggle zoom %.1fx (platformer smooth follow & click animation)\n",
                kZoomLevel);
    std::printf("  Ctrl+Alt+Q : keluar & bersihkan memori\n");
    std::printf("Status: berjalan di background...\n");
    std::fflush(stdout);
}

}  // namespace

int main() {
    EnableDpiAwareness();

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

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

    Gdiplus::GdiplusShutdown(gdiplusToken);

    return 0;
}
