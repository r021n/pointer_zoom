#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <propidl.h>
#include <gdiplus.h>
#include <magnification.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C"
{
    BOOL WINAPI MagInitialize(void);
    BOOL WINAPI MagUninitialize(void);
    BOOL WINAPI MagSetFullscreenTransform(float magLevel, int x, int y);
    BOOL WINAPI MagShowSystemCursor(BOOL fShowCursor);
}

namespace
{

    constexpr float kZoomLevel = 1.75f;
    constexpr int kHotkeyToggle = 1;
    constexpr int kHotkeyQuit = 2;
    constexpr int kHotkeyRecord = 3;
    constexpr int kPollIntervalMs = 8;
    constexpr bool kCaptureIncludesMagnifier = true;

    // Perekaman layar
    constexpr int kRecordFps = 30;
    constexpr UINT32 kRecordBitrate = 8000000; // 8 Mbps (Kualitas tinggi H.264)

    // Platformer camera lag parameter (~280ms delay untuk camera follow yang lebih smooth dan terasa delay-nya)
    constexpr float kCameraSmoothTime = 0.28f;

    // Transisi halus saat masuk zoom in dan keluar zoom out (~200ms smooth damp)
    constexpr float kZoomSmoothTime = 0.20f;

    // Base pointer scale (diperbesar agar lebih jelas dan proporsional saat presentasi/zoom)
    constexpr float kBasePointerScale = 1.55f;

    // Custom animated pointer overlay dimensions (96x96 untuk mengakomodasi pointer lebih besar + drop shadow + ripple)
    constexpr int kOverlaySize = 96;
    constexpr float kTipOffsetX = 28.0f;
    constexpr float kTipOffsetY = 28.0f;

    // Dapatkan resolusi fisik asli monitor (misal 1920x1080), bebas dari kebohongan virtualisasi DPI (1536x864)
    void GetPhysicalScreenResolution(int &outW, int &outH)
    {
        HDC hdc = GetDC(nullptr);
        outW = GetDeviceCaps(hdc, DESKTOPHORZRES);
        outH = GetDeviceCaps(hdc, DESKTOPVERTRES);
        ReleaseDC(nullptr, hdc);
        if (outW <= 0)
            outW = GetSystemMetrics(SM_CXSCREEN);
        if (outH <= 0)
            outH = GetSystemMetrics(SM_CYSCREEN);
        outW &= ~1; // Pastikan genap untuk encoder video
        outH &= ~1;
    }

    struct CamSnapshot
    {
        bool isZoomed = false;
        float zoomLevel = 1.0f;
        int camX = 0;
        int camY = 0;
        int regionW = 0;
        int regionH = 0;
        float normCursorX = 0.5f; // 0.0 s.d 1.0 (relatif terhadap viewport tampilan)
        float normCursorY = 0.5f; // 0.0 s.d 1.0
        float pointerScale = kBasePointerScale;
        float waveProgress = -1.0f;
        POINT cursor{0, 0};
    };

    std::mutex g_camMutex;
    CamSnapshot g_camState;

    // Snapshot atomik: koordinat kursor ternormalisasi (0-1) + kamera + efek kursor disimpan
    // dalam 1 paket sinkron dari iterasi yang SAMA, bebas desinkronisasi.
    void PublishCamState(bool isZoomed, float zoomLevel, int camX, int camY, int regionW, int regionH,
                         float normX, float normY, float pointerScale,
                         float waveProgress, const POINT &cursor)
    {
        std::lock_guard<std::mutex> lock(g_camMutex);
        g_camState.isZoomed = isZoomed;
        g_camState.zoomLevel = zoomLevel;
        g_camState.camX = camX;
        g_camState.camY = camY;
        g_camState.regionW = regionW;
        g_camState.regionH = regionH;
        g_camState.normCursorX = normX;
        g_camState.normCursorY = normY;
        g_camState.pointerScale = pointerScale;
        g_camState.waveProgress = waveProgress;
        g_camState.cursor = cursor;
    }

    CamSnapshot ReadCamState()
    {
        std::lock_guard<std::mutex> lock(g_camMutex);
        return g_camState;
    }

    std::atomic<bool> g_running{true};
    std::atomic<bool> g_enabled{false};
    std::atomic<bool> g_transformWarned{false};

    std::atomic<bool> g_recording{false};
    std::atomic<bool> g_saving{false};
    std::atomic<bool> g_recorderBusy{false};
    std::thread g_recorderThread;

    // Overlay layered window handles (owned and managed solely by MagnifierThread)
    HWND g_overlayHwnd = nullptr;
    HDC g_hdcMem = nullptr;
    HBITMAP g_hDIB = nullptr;
    HGDIOBJ g_oldBmp = nullptr;

    template <typename Fn>
    Fn LoadUser32Proc(const char *name)
    {
        static HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32)
        {
            return nullptr;
        }
        return reinterpret_cast<Fn>(reinterpret_cast<void *>(
            GetProcAddress(user32, name)));
    }

    void EnableDpiAwareness()
    {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI *)(HANDLE);
        if (auto setCtx =
                LoadUser32Proc<SetProcessDpiAwarenessContextFn>(
                    "SetProcessDpiAwarenessContext"))
        {
            if (setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
            {
                return;
            }
        }
        using SetProcessDPIAwareFn = BOOL(WINAPI *)();
        if (auto setLegacy = LoadUser32Proc<SetProcessDPIAwareFn>(
                "SetProcessDPIAware"))
        {
            setLegacy();
        }
    }

    void WarnTransformFailure(DWORD error)
    {
        if (!g_transformWarned.exchange(true))
        {
            std::printf(
                "[!] MagSetFullscreenTransform GAGAL (GetLastError=%lu).\n"
                "    Jalankan aplikasi ini sebagai Administrator, atau gunakan\n"
                "    manifest dengan hak akses UIAccess.\n",
                static_cast<unsigned long>(error));
            std::fflush(stdout);
        }
    }

    void ApplyTransform(float level, int x, int y)
    {
        if (!MagSetFullscreenTransform(level, x, y))
        {
            WarnTransformFailure(GetLastError());
        }
    }

    double GetTimeSeconds()
    {
        static LARGE_INTEGER freq = []
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return f;
        }();
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
    }

    // Critically damped spring (Unity SmoothDamp style) for organic platformer camera follow
    float SmoothDamp(float current, float target, float &currentVelocity, float smoothTime, float deltaTime)
    {
        smoothTime = (std::max)(0.0001f, smoothTime);
        float omega = 2.0f / smoothTime;
        float x = omega * deltaTime;
        float expVal = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        float change = current - target;
        float originalTo = target;

        float temp = (currentVelocity + omega * change) * deltaTime;
        currentVelocity = (currentVelocity - omega * temp) * expVal;
        float output = target + (change + temp) * expVal;

        if ((originalTo - current > 0.0f) == (output > originalTo))
        {
            output = originalTo;
            currentVelocity = 0.0f;
        }
        return output;
    }

    // Guaranteed 100% click-through window procedure: never intercepts clicks across any app
    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
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

    bool InitOverlayWindow(HINSTANCE hInstance)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = L"PointerZoomOverlayClass";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        g_overlayHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"PointerZoomOverlayClass",
            L"PointerOverlay",
            WS_POPUP | WS_DISABLED,
            0, 0, kOverlaySize, kOverlaySize,
            nullptr, nullptr, hInstance, nullptr);

        if (!g_overlayHwnd)
            return false;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
        // Sembunyikan window overlay dari screen capture (BitBlt/DWM) agar tidak muncul 2 kursor saat merekam
        SetWindowDisplayAffinity(g_overlayHwnd, WDA_EXCLUDEFROMCAPTURE);

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
        void *bits = nullptr;
        g_hDIB = CreateDIBSection(g_hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        g_oldBmp = SelectObject(g_hdcMem, g_hDIB);
        ReleaseDC(nullptr, hdcScreen);

        return true;
    }

    void CleanupOverlayWindow()
    {
        if (g_overlayHwnd)
        {
            ShowWindow(g_overlayHwnd, SW_HIDE);
            DestroyWindow(g_overlayHwnd);
            g_overlayHwnd = nullptr;
        }
        if (g_hdcMem)
        {
            if (g_oldBmp)
                SelectObject(g_hdcMem, g_oldBmp);
            if (g_hDIB)
                DeleteObject(g_hDIB);
            DeleteDC(g_hdcMem);
            g_hdcMem = nullptr;
            g_hDIB = nullptr;
            g_oldBmp = nullptr;
        }
    }

    // Menggambar pointer kustom (dengan ripple click dan indikator REC jika aktif)
    void DrawPointerShape(Gdiplus::Graphics &g, float tipX, float tipY, float scale, float waveProgress, bool isRecording)
    {
        // 1. Expanding click ripple at hotspot tip
        if (waveProgress >= 0.0f && waveProgress <= 1.0f)
        {
            float radius = (3.0f + 14.0f * waveProgress) * (scale > 1.2f ? 1.4f : 1.0f);
            int alpha = static_cast<int>(180.0f * (1.0f - waveProgress));
            if (alpha > 0)
            {
                Gdiplus::Pen wavePen(Gdiplus::Color(alpha, 40, 160, 255), 2.2f * (1.0f - waveProgress * 0.5f));
                g.DrawEllipse(&wavePen, tipX - radius, tipY - radius, radius * 2.0f, radius * 2.0f);
            }
        }

        // 2. Base pointer coordinates relative to Tip (0, 0)
        const Gdiplus::PointF basePoints[7] = {
            {0.0f, 0.0f},   // Tip (Hotspot)
            {0.0f, 21.0f},  // Left edge
            {5.2f, 16.2f},  // Inner crook
            {9.2f, 24.2f},  // Tail outer edge
            {12.8f, 22.4f}, // Tail right edge
            {8.8f, 14.5f},  // Outer crook
            {15.0f, 14.5f}  // Right wing
        };

        Gdiplus::PointF arrowPts[7];
        Gdiplus::PointF shadowPts[7];
        const float shadowOffX = 1.5f * (std::max)(1.0f, scale * 0.7f);
        const float shadowOffY = 2.0f * (std::max)(1.0f, scale * 0.7f);

        for (int i = 0; i < 7; ++i)
        {
            float sx = basePoints[i].X * scale;
            float sy = basePoints[i].Y * scale;
            arrowPts[i] = Gdiplus::PointF(tipX + sx, tipY + sy);
            shadowPts[i] = Gdiplus::PointF(tipX + sx + shadowOffX, tipY + sy + shadowOffY);
        }

        // Soft drop shadow
        Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(75, 0, 0, 0));
        g.FillPolygon(&shadowBrush, shadowPts, 7);

        // Crisp white pointer body
        Gdiplus::SolidBrush whiteBrush(Gdiplus::Color(255, 255, 255, 255));
        g.FillPolygon(&whiteBrush, arrowPts, 7);

        // Dark charcoal outer border
        Gdiplus::Pen outlinePen(Gdiplus::Color(255, 25, 25, 28), 1.8f * (std::max)(1.0f, scale * 0.7f));
        outlinePen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPolygon(&outlinePen, arrowPts, 7);

        // Indikator recording aktif (titik merah berkedip halus di samping kursor)
        if (isRecording)
        {
            double now = GetTimeSeconds();
            // Pulsing alpha ~0.5 Hz
            int pulseAlpha = 180 + static_cast<int>(70.0 * std::sin(now * 8.0));
            pulseAlpha = std::clamp(pulseAlpha, 80, 255);
            float dotRadius = 4.0f * (std::max)(1.0f, scale * 0.6f);
            float dotX = tipX + 18.0f * scale;
            float dotY = tipY + 2.0f * scale;

            Gdiplus::SolidBrush recDotBrush(Gdiplus::Color(pulseAlpha, 235, 35, 35));
            g.FillEllipse(&recDotBrush, dotX - dotRadius, dotY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);

            Gdiplus::Pen recDotBorder(Gdiplus::Color(255, 255, 255, 255), 1.2f);
            g.DrawEllipse(&recDotBorder, dotX - dotRadius, dotY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
        }
    }

    void RenderPointerOverlay(POINT cursorPt, float scale, float waveProgress)
    {
        if (!g_overlayHwnd || !g_hdcMem)
            return;

        const bool isRec = g_recording.load(std::memory_order_relaxed);

        {
            Gdiplus::Graphics g(g_hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.Clear(Gdiplus::Color(0, 0, 0, 0));
            DrawPointerShape(g, kTipOffsetX, kTipOffsetY, scale, waveProgress, isRec);
        }

        POINT ptSrc = {0, 0};
        POINT ptDst = {
            cursorPt.x - static_cast<int>(kTipOffsetX),
            cursorPt.y - static_cast<int>(kTipOffsetY)};
        SIZE size = {kOverlaySize, kOverlaySize};
        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        HDC hdcScreen = GetDC(nullptr);
        UpdateLayeredWindow(g_overlayHwnd, hdcScreen, &ptDst, &size, g_hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, hdcScreen);
    }

    void MagnifierThread()
    {
        if (!MagInitialize())
        {
            std::printf("[!] MagInitialize gagal (error %lu).\n",
                        static_cast<unsigned long>(GetLastError()));
            std::fflush(stdout);
        }

        HINSTANCE hInstance = GetModuleHandleW(nullptr);
        if (!InitOverlayWindow(hInstance))
        {
            std::printf("[!] InitOverlayWindow gagal (error %lu).\n", GetLastError());
        }

        float curCamX = 0.0f;
        float curCamY = 0.0f;
        float velX = 0.0f;
        float velY = 0.0f;
        bool tracking = false;

        // Zoom animation transition state
        float currentProgress = 0.0f;
        float progressVelocity = 0.0f;

        // Pointer animation state
        float curScale = kBasePointerScale;
        float scaleVel = 0.0f;
        float clickPulseTimer = 0.0f;
        float waveTimer = -1.0f;
        bool lastPressed = false;

        double lastTime = GetTimeSeconds();
        double lastTopmostTime = 0.0;

        while (g_running.load(std::memory_order_relaxed))
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            double now = GetTimeSeconds();
            float dt = static_cast<float>(now - lastTime);
            lastTime = now;
            dt = std::clamp(dt, 0.001f, 0.050f);

            POINT pt{};
            using GetPhysicalCursorPosFn = BOOL(WINAPI *)(LPPOINT);
            static auto pGetPhysicalCursorPos = LoadUser32Proc<GetPhysicalCursorPosFn>("GetPhysicalCursorPos");
            if (!pGetPhysicalCursorPos || !pGetPhysicalCursorPos(&pt))
            {
                GetCursorPos(&pt);
            }

            const bool enabled = g_enabled.load(std::memory_order_relaxed);
            const float targetProgress = enabled ? 1.0f : 0.0f;

            // Jika zoom dimatikan dan animasi zoom-out sudah selesai tuntas:
            if (!enabled && currentProgress <= 0.0f)
            {
                if (tracking)
                {
                    ApplyTransform(1.0f, 0, 0);
                    MagShowSystemCursor(TRUE);
                    if (g_overlayHwnd)
                    {
                        ShowWindow(g_overlayHwnd, SW_HIDE);
                    }
                    tracking = false;
                    velX = 0.0f;
                    velY = 0.0f;
                    progressVelocity = 0.0f;
                    curScale = kBasePointerScale;
                    scaleVel = 0.0f;
                    clickPulseTimer = 0.0f;
                    waveTimer = -1.0f;
                    lastPressed = false;
                }

                int screenW = 1920, screenH = 1080;
                GetPhysicalScreenResolution(screenW, screenH);
                const float normX = (screenW > 0) ? (static_cast<float>(pt.x) / static_cast<float>(screenW)) : 0.0f;
                const float normY = (screenH > 0) ? (static_cast<float>(pt.y) / static_cast<float>(screenH)) : 0.0f;

                // Update shared state for recorder even when zoom is OFF (1:1 normalized)
                PublishCamState(false, 1.0f, 0, 0, screenW, screenH, normX, normY, kBasePointerScale, -1.0f, pt);

                Sleep(kPollIntervalMs);
                continue;
            }

            int screenW = 1920, screenH = 1080;
            GetPhysicalScreenResolution(screenW, screenH);

            const float regionW_zoomed = static_cast<float>(screenW) / kZoomLevel;
            const float regionH_zoomed = static_cast<float>(screenH) / kZoomLevel;
            const float maxZoomedX = (std::max)(0.0f, static_cast<float>(screenW) - regionW_zoomed);
            const float maxZoomedY = (std::max)(0.0f, static_cast<float>(screenH) - regionH_zoomed);

            float targetCamX = static_cast<float>(pt.x) - regionW_zoomed * 0.5f;
            float targetCamY = static_cast<float>(pt.y) - regionH_zoomed * 0.5f;
            targetCamX = std::clamp(targetCamX, 0.0f, maxZoomedX);
            targetCamY = std::clamp(targetCamY, 0.0f, maxZoomedY);

            if (!tracking)
            {
                // Inisialisasi awal saat toggle zoom in ditekan
                curCamX = targetCamX;
                curCamY = targetCamY;
                velX = 0.0f;
                velY = 0.0f;
                currentProgress = 0.0f;
                progressVelocity = 0.0f;
                tracking = true;

                // MagShowSystemCursor(FALSE);
                // if (g_overlayHwnd)
                // {
                //     ShowWindow(g_overlayHwnd, SW_SHOWNOACTIVATE);
                // }
            }
            else
            {
                // Platformer camera follow with smooth dampening lag towards cursor center
                curCamX = SmoothDamp(curCamX, targetCamX, velX, kCameraSmoothTime, dt);
                curCamY = SmoothDamp(curCamY, targetCamY, velY, kCameraSmoothTime, dt);

                if (curCamX <= 0.0f || curCamX >= maxZoomedX)
                    velX = 0.0f;
                if (curCamY <= 0.0f || curCamY >= maxZoomedY)
                    velY = 0.0f;

                curCamX = std::clamp(curCamX, 0.0f, maxZoomedX);
                curCamY = std::clamp(curCamY, 0.0f, maxZoomedY);
            }

            // Animasikan progress zoom in/out dengan SmoothDamp
            currentProgress = SmoothDamp(currentProgress, targetProgress, progressVelocity, kZoomSmoothTime, dt);
            if (!enabled && currentProgress <= 0.002f)
            {
                currentProgress = 0.0f;
                progressVelocity = 0.0f;
            }
            else if (enabled && (1.0f - currentProgress) <= 0.002f)
            {
                currentProgress = 1.0f;
                progressVelocity = 0.0f;
            }

            // Hitung faktor zoom dinamis
            const float curZoom = 1.0f + (kZoomLevel - 1.0f) * currentProgress;

            // Hitung ukuran viewport aktif saat transisi
            const float curRegionW = static_cast<float>(screenW) / curZoom;
            const float curRegionH = static_cast<float>(screenH) / curZoom;
            const float curMaxX = (std::max)(0.0f, static_cast<float>(screenW) - curRegionW);
            const float curMaxY = (std::max)(0.0f, static_cast<float>(screenH) - curRegionH);

            // Interpolasikan posisi camera transform
            float actualCamX = curCamX * currentProgress;
            float actualCamY = curCamY * currentProgress;
            actualCamX = std::clamp(actualCamX, 0.0f, curMaxX);
            actualCamY = std::clamp(actualCamY, 0.0f, curMaxY);

            const int camX = static_cast<int>(std::lround(actualCamX));
            const int camY = static_cast<int>(std::lround(actualCamY));

            ApplyTransform(curZoom, camX, camY);

            // --- Fast Async Mouse Click Detection ---
            const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            const bool middleDown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
            const bool isPressed = leftDown || rightDown || middleDown;

            if (isPressed && !lastPressed)
            {
                clickPulseTimer = 0.09f;
                waveTimer = 0.0f;
            }
            lastPressed = isPressed;

            bool isDepressed = isPressed;
            if (clickPulseTimer > 0.0f)
            {
                clickPulseTimer -= dt;
                isDepressed = true;
            }

            const float targetScale = isDepressed ? (kBasePointerScale * 0.78f) : kBasePointerScale;
            constexpr float kSpringStiffness = 460.0f;
            constexpr float kSpringDamping = 26.0f;
            float force = kSpringStiffness * (targetScale - curScale);
            scaleVel += force * dt;
            scaleVel -= kSpringDamping * scaleVel * dt;
            curScale += scaleVel * dt;

            float waveProgress = -1.0f;
            if (waveTimer >= 0.0f)
            {
                waveTimer += dt;
                waveProgress = waveTimer / 0.22f;
                if (waveProgress > 1.0f)
                {
                    waveTimer = -1.0f;
                    waveProgress = -1.0f;
                }
            }

            RenderPointerOverlay(pt, curScale, waveProgress);

            if (g_overlayHwnd && (now - lastTopmostTime) > 0.2)
            {
                SetWindowPos(g_overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                lastTopmostTime = now;
            }

            const int regionWInt = static_cast<int>(std::lround(curRegionW));
            const int regionHInt = static_cast<int>(std::lround(curRegionH));
            const int clampedCamX = std::clamp(camX, 0, (std::max)(1, screenW - regionWInt));
            const int clampedCamY = std::clamp(camY, 0, (std::max)(1, screenH - regionHInt));

            // Koordinat ternormalisasi (0.0 s.d 1.0) posisi kursor relatif terhadap viewport kamera zoom
            const float normX = (regionWInt > 0)
                                    ? (static_cast<float>(pt.x - clampedCamX) / static_cast<float>(regionWInt))
                                    : 0.0f;
            const float normY = (regionHInt > 0)
                                    ? (static_cast<float>(pt.y - clampedCamY) / static_cast<float>(regionHInt))
                                    : 0.0f;

            // Update shared state for recorder (atomik: kursor ternormalisasi + camera + efek dalam 1 snapshot)
            PublishCamState(currentProgress > 0.0005f, curZoom, clampedCamX, clampedCamY, regionWInt, regionHInt,
                            normX, normY, curScale, waveProgress, pt);

            Sleep(kPollIntervalMs);
        }

        if (tracking || currentProgress > 0.0f)
        {
            MagSetFullscreenTransform(1.0f, 0, 0);
            MagShowSystemCursor(TRUE);
        }
        CleanupOverlayWindow();
        MagUninitialize();
    }

    std::wstring GetTempRecordFilePath()
    {
        wchar_t tempDir[MAX_PATH];
        DWORD len = GetTempPathW(MAX_PATH, tempDir);
        if (len == 0 || len > MAX_PATH)
        {
            return L"zoom_record_temp.mp4";
        }
        wchar_t tempFile[MAX_PATH];
        if (GetTempFileNameW(tempDir, L"ZRC", 0, tempFile) != 0)
        {
            std::wstring path = tempFile;
            size_t dot = path.find_last_of(L'.');
            if (dot != std::wstring::npos)
            {
                path = path.substr(0, dot) + L".mp4";
            }
            else
            {
                path += L".mp4";
            }
            return path;
        }
        return std::wstring(tempDir) + L"zoom_record_temp.mp4";
    }

    void PromptSaveRecording(const std::wstring &tempPath)
    {
        g_saving.store(true);

        wchar_t szFile[MAX_PATH] = L"";
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf_s(szFile, MAX_PATH, L"ZoomRecording_%04d%02d%02d_%02d%02d%02d.mp4",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"MP4 Video (*.mp4)\0*.mp4\0Semua File (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrTitle = L"Simpan Hasil Rekaman Layar";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        ofn.lpstrDefExt = L"mp4";

        std::printf("[REC] Membuka jendela dialog penyimpanan file...\n");
        std::fflush(stdout);

        if (GetSaveFileNameW(&ofn))
        {
            if (MoveFileExW(tempPath.c_str(), szFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
            {
                std::printf("[REC] Video berhasil disimpan: %ls\n", szFile);
                std::fflush(stdout);
                MessageBeep(MB_OK);
                MessageBoxW(nullptr, (std::wstring(L"Rekaman layar berhasil disimpan!\n\nLokasi:\n") + szFile).c_str(),
                            L"Perekaman Selesai", MB_ICONINFORMATION | MB_OK | MB_SYSTEMMODAL);
            }
            else
            {
                std::printf("[REC] Gagal memindahkan file rekaman (error %lu).\n", GetLastError());
                std::fflush(stdout);
                DeleteFileW(tempPath.c_str());
            }
        }
        else
        {
            std::printf("[REC] Penyimpanan dibatalkan oleh pengguna. File sementara dihapus.\n");
            std::fflush(stdout);
            DeleteFileW(tempPath.c_str());
        }

        g_saving.store(false);
    }

    // Thread perekaman video layar native Windows Media Foundation
    void RecorderThreadFunc()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);

        int screenW = 1920, screenH = 1080;
        GetPhysicalScreenResolution(screenW, screenH);
        std::printf("[REC] Resolusi frame perekaman fisik: %dx%d (BitBlt 1:1)\n", screenW, screenH);
        std::fflush(stdout);

        const std::wstring tempPath = GetTempRecordFilePath();

        IMFSinkWriter *pSinkWriter = nullptr;
        HRESULT hr = MFCreateSinkWriterFromURL(tempPath.c_str(), nullptr, nullptr, &pSinkWriter);
        if (FAILED(hr))
        {
            std::printf("[REC] Gagal membuat sink writer (0x%08lx).\n", hr);
            std::fflush(stdout);
            MFShutdown();
            CoUninitialize();
            g_recording.store(false);
            return;
        }

        DWORD streamIndex = 0;

        // Konfigurasi Video Output: H.264 MP4
        IMFMediaType *pMediaTypeOut = nullptr;
        MFCreateMediaType(&pMediaTypeOut);
        pMediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pMediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        pMediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, kRecordBitrate);
        pMediaTypeOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(pMediaTypeOut, MF_MT_FRAME_SIZE, screenW, screenH);
        MFSetAttributeRatio(pMediaTypeOut, MF_MT_FRAME_RATE, kRecordFps, 1);
        MFSetAttributeRatio(pMediaTypeOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = pSinkWriter->AddStream(pMediaTypeOut, &streamIndex);
        pMediaTypeOut->Release();
        if (FAILED(hr))
        {
            std::printf("[REC] Gagal AddStream (0x%08lx).\n", hr);
            std::fflush(stdout);
            pSinkWriter->Release();
            MFShutdown();
            CoUninitialize();
            g_recording.store(false);
            return;
        }

        // Konfigurasi Video Input: RGB32
        IMFMediaType *pMediaTypeIn = nullptr;
        MFCreateMediaType(&pMediaTypeIn);
        pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        pMediaTypeIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(pMediaTypeIn, MF_MT_FRAME_SIZE, screenW, screenH);
        MFSetAttributeRatio(pMediaTypeIn, MF_MT_FRAME_RATE, kRecordFps, 1);
        MFSetAttributeRatio(pMediaTypeIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = pSinkWriter->SetInputMediaType(streamIndex, pMediaTypeIn, nullptr);
        pMediaTypeIn->Release();
        if (FAILED(hr))
        {
            std::printf("[REC] Gagal SetInputMediaType (0x%08lx).\n", hr);
            std::fflush(stdout);
            pSinkWriter->Release();
            MFShutdown();
            CoUninitialize();
            g_recording.store(false);
            return;
        }

        hr = pSinkWriter->BeginWriting();
        if (FAILED(hr))
        {
            std::printf("[REC] Gagal BeginWriting (0x%08lx).\n", hr);
            std::fflush(stdout);
            pSinkWriter->Release();
            MFShutdown();
            CoUninitialize();
            g_recording.store(false);
            return;
        }

        // Persiapan DC & DIB Section untuk Screen Capture & Compositing
        HDC hdcScreen = GetDC(nullptr);

        HDC hdcFrame = CreateCompatibleDC(hdcScreen);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = screenW;
        bmi.bmiHeader.biHeight = screenH; // Positive height = Bottom-up DIB, standar wajib Media Foundation RGB32 agar video tidak terbalik
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void *pFrameBits = nullptr;
        HBITMAP hFrameBmp = CreateDIBSection(hdcFrame, &bmi, DIB_RGB_COLORS, &pFrameBits, nullptr, 0);
        HGDIOBJ oldFrameBmp = SelectObject(hdcFrame, hFrameBmp);

        const DWORD cbBuffer = static_cast<DWORD>(screenW * screenH * 4);
        const LONGLONG frameDuration100ns = 10000000LL / kRecordFps;
        const double frameIntervalSec = 1.0 / static_cast<double>(kRecordFps);

        int64_t frameIndex = 0;
        double startTime = GetTimeSeconds();

        // Render 1 frame: capture layar + gambar kursor menggunakan koordinat ternormalisasi (0 s.d 1)
        // yang dicatat secara sinkron dalam satu paket snapshot bersama kamera magnifier.
        auto renderFrame = [&]()
        {
            const CamSnapshot snap = ReadCamState();

            if (snap.isZoomed && snap.zoomLevel > 1.0005f)
            {
                // Sesuai dengan MagSetFullscreenTransform(snap.zoomLevel, camX, camY):
                // Layar diperbesar dinamis sesuai snap.zoomLevel saat transisi halus.
                const float z = snap.zoomLevel;
                float renderCursorX, renderCursorY, renderScale;

                if (kCaptureIncludesMagnifier)
                {
                    // Layar sudah ter-zoom oleh DWM: cukup salin 1:1 seperti yang terlihat di monitor.
                    BitBlt(hdcFrame, 0, 0, screenW, screenH, hdcScreen, 0, 0, SRCCOPY);

                    // Posisi kursor di layar ter-zoom: (world - cam) * zoom
                    renderCursorX = static_cast<float>(snap.cursor.x - snap.camX) * z;
                    renderCursorY = static_cast<float>(snap.cursor.y - snap.camY) * z;
                    renderScale = snap.pointerScale * z;
                }
                else
                {
                    const int srcX = snap.camX;
                    const int srcY = snap.camY;
                    const int srcW = static_cast<int>(std::lround(screenW / z));
                    const int srcH = static_cast<int>(std::lround(screenH / z));

                    SetStretchBltMode(hdcFrame, COLORONCOLOR);
                    StretchBlt(hdcFrame, 0, 0, screenW, screenH, hdcScreen, srcX, srcY, srcW, srcH, SRCCOPY);

                    const float scaleX = static_cast<float>(screenW) / srcW;
                    const float scaleY = static_cast<float>(screenH) / srcH;
                    renderCursorX = static_cast<float>(snap.cursor.x - srcX) * scaleX;
                    renderCursorY = static_cast<float>(snap.cursor.y - srcY) * scaleY;
                    renderScale = snap.pointerScale * scaleX;
                }

                Gdiplus::Graphics g(hdcFrame);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                DrawPointerShape(g, renderCursorX, renderCursorY, renderScale, snap.waveProgress, true);
            }
            else
            {
                // Zoom OFF: salin 1:1 seluruh layar
                BitBlt(hdcFrame, 0, 0, screenW, screenH, hdcScreen, 0, 0, SRCCOPY);

                // Ambil posisi kursor fisik layar
                POINT ptCursor{};
                using GetPhysicalCursorPosFn = BOOL(WINAPI *)(LPPOINT);
                static auto pGetPhysicalCursorPos = LoadUser32Proc<GetPhysicalCursorPosFn>("GetPhysicalCursorPos");
                if (!pGetPhysicalCursorPos || !pGetPhysicalCursorPos(&ptCursor))
                {
                    GetCursorPos(&ptCursor);
                }

                // Gambar kursor Windows aktif
                CURSORINFO ci{};
                ci.cbSize = sizeof(CURSORINFO);
                bool drewCursor = false;
                if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))
                {
                    ICONINFO ii = {};
                    if (GetIconInfo(ci.hCursor, &ii))
                    {
                        // Gunakan koordinat ci.ptScreenPos yang sinkron dengan handle kursor
                        int targetX = ci.ptScreenPos.x - static_cast<int>(ii.xHotspot);
                        int targetY = ci.ptScreenPos.y - static_cast<int>(ii.yHotspot);

                        // Gambar dengan ukuran default sistem DPI-scaled (0, 0 + DI_NORMAL | DI_DEFAULTSIZE)
                        // agar ukuran dan letak hotspot cocok persis dengan yang digambar oleh DWM Windows
                        DrawIconEx(hdcFrame, targetX, targetY, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL | DI_DEFAULTSIZE);

                        if (ii.hbmColor)
                            DeleteObject(ii.hbmColor);
                        if (ii.hbmMask)
                            DeleteObject(ii.hbmMask);
                        drewCursor = true;
                    }
                }
                if (!drewCursor)
                {
                    Gdiplus::Graphics g(hdcFrame);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    DrawPointerShape(g, static_cast<float>(ptCursor.x), static_cast<float>(ptCursor.y),
                                     snap.pointerScale, snap.waveProgress, true);
                }
            }

            GdiFlush();
        };

        auto encodeFrame = [&](IMFSinkWriter *writer, DWORD stream, LONGLONG sampleTime)
        {
            IMFMediaBuffer *pBuffer = nullptr;
            if (SUCCEEDED(MFCreateMemoryBuffer(cbBuffer, &pBuffer)))
            {
                BYTE *pDst = nullptr;
                pBuffer->Lock(&pDst, nullptr, nullptr);
                memcpy(pDst, pFrameBits, cbBuffer);
                pBuffer->Unlock();
                pBuffer->SetCurrentLength(cbBuffer);

                IMFSample *pSample = nullptr;
                if (SUCCEEDED(MFCreateSample(&pSample)))
                {
                    pSample->AddBuffer(pBuffer);
                    pBuffer->Release();
                    pSample->SetSampleTime(sampleTime);
                    pSample->SetSampleDuration(frameDuration100ns);
                    writer->WriteSample(stream, pSample);
                    pSample->Release();
                }
                else
                {
                    pBuffer->Release();
                }
            }
        };

        while (g_recording.load(std::memory_order_relaxed) && g_running.load(std::memory_order_relaxed))
        {
            renderFrame();
            encodeFrame(pSinkWriter, streamIndex, frameIndex * frameDuration100ns);
            frameIndex++;

            // Pacing frame rate 30 FPS tersinkronisasi waktu nyata (Real-Time Wall Clock)
            double targetTime = startTime + static_cast<double>(frameIndex) * frameIntervalSec;
            double now = GetTimeSeconds();
            double sleepSec = targetTime - now;

            if (sleepSec > 0.001)
            {
                Sleep(static_cast<DWORD>(sleepSec * 1000.0));
            }
            else if (sleepSec < -frameIntervalSec)
            {
                // Jika ada frame delay dari OS, render frame baru (bukan salinan frame lama)
                // agar posisi kursor di setiap frame tetap akurat sesuai waktunya.
                int missedFrames = static_cast<int>((-sleepSec) / frameIntervalSec);
                missedFrames = (std::min)(missedFrames, 5);
                for (int d = 0; d < missedFrames && g_recording.load(std::memory_order_relaxed); ++d)
                {
                    renderFrame();
                    encodeFrame(pSinkWriter, streamIndex, frameIndex * frameDuration100ns);
                    frameIndex++;
                }
            }
        }

        std::printf("[REC] Memfinalisasi video MP4 (%lld frame)...\n", static_cast<long long>(frameIndex));
        std::fflush(stdout);

        // Finalize MP4 writer
        pSinkWriter->Finalize();
        pSinkWriter->Release();

        // Cleanup GDI
        SelectObject(hdcFrame, oldFrameBmp);
        DeleteObject(hFrameBmp);
        DeleteDC(hdcFrame);

        ReleaseDC(nullptr, hdcScreen);

        MFShutdown();
        CoUninitialize();

        // Tampilkan modal save file dialog kepada user jika aplikasi masih berjalan
        if (frameIndex > 0 && g_running.load(std::memory_order_relaxed))
        {
            PromptSaveRecording(tempPath);
        }
        else
        {
            DeleteFileW(tempPath.c_str());
        }
    }

    void PrintBanner()
    {
        std::printf("========================================================\n");
        std::printf("  Zoom Pointer & Screen Recorder (Hardware-Accelerated)\n");
        std::printf("========================================================\n");
        std::printf("  Ctrl+Alt+Z : Toggle zoom %.1fx (transisi animasi halus & camera follow)\n",
                    kZoomLevel);
        std::printf("  Ctrl+Alt+R : Mulai / Stop rekaman layar (sesuai kondisi zoom)\n");
        std::printf("  Ctrl+Alt+Q : Keluar & bersihkan memori\n");
        std::printf("--------------------------------------------------------\n");
        std::printf("Status: berjalan di background...\n");
        std::fflush(stdout);
    }

} // namespace

int main()
{
    EnableDpiAwareness();

    {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hIn, &mode))
        {
            mode &= ~ENABLE_QUICK_EDIT_MODE;
            mode |= ENABLE_EXTENDED_FLAGS;
            SetConsoleMode(hIn, mode);
        }
    }

    // Resolusi timer 1 ms (default ~15.6 ms) agar polling kursor & pacing frame akurat.
    using TimePeriodFn = UINT(WINAPI *)(UINT);
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    TimePeriodFn pTimeBegin = winmm ? reinterpret_cast<TimePeriodFn>(
                                          reinterpret_cast<void *>(GetProcAddress(winmm, "timeBeginPeriod")))
                                    : nullptr;
    TimePeriodFn pTimeEnd = winmm ? reinterpret_cast<TimePeriodFn>(
                                        reinterpret_cast<void *>(GetProcAddress(winmm, "timeEndPeriod")))
                                  : nullptr;
    if (pTimeBegin)
        pTimeBegin(1);

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    const bool hotkeyToggleOk =
        RegisterHotKey(nullptr, kHotkeyToggle, MOD_CONTROL | MOD_ALT, 'Z');
    const bool hotkeyRecordOk =
        RegisterHotKey(nullptr, kHotkeyRecord, MOD_CONTROL | MOD_ALT, 'R');
    const bool hotkeyQuitOk =
        RegisterHotKey(nullptr, kHotkeyQuit, MOD_CONTROL | MOD_ALT, 'Q');

    if (!hotkeyToggleOk || !hotkeyRecordOk || !hotkeyQuitOk)
    {
        std::printf("[!] RegisterHotKey gagal (error %lu). Hotkey mungkin dipakai aplikasi lain.\n",
                    GetLastError());
    }

    PrintBanner();

    std::thread magnifier(MagnifierThread);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message == WM_HOTKEY)
        {
            if (msg.wParam == kHotkeyToggle)
            {
                const bool enable = !g_enabled.load();
                g_transformWarned.store(false);
                g_enabled.store(enable);
                std::printf("Zoom: %s (%.1fx)\n", enable ? "ON" : "OFF",
                            enable ? kZoomLevel : 1.0f);
                std::fflush(stdout);
            }
            else if (msg.wParam == kHotkeyRecord)
            {
                if (g_saving.load() || (g_recorderBusy.load() && !g_recording.load()))
                {
                    std::printf("[REC] Rekaman sebelumnya masih diproses/disimpan, harap tunggu...\n");
                    std::fflush(stdout);
                }
                else if (!g_recording.load())
                {
                    if (g_recorderThread.joinable())
                        g_recorderThread.join(); // sekarang instan
                    g_recording.store(true);
                    g_recorderBusy.store(true);
                    MessageBeep(MB_ICONASTERISK);
                    std::printf("\n[REC] >>> PEREKAMAN DIMULAI (Ctrl+Alt+R untuk berhenti) <<<\n");
                    std::fflush(stdout);
                    g_recorderThread = std::thread([]
                                                   {
                        RecorderThreadFunc();
                        g_recorderBusy.store(false); });
                }
                else
                {
                    std::printf("\n[REC] >>> PEREKAMAN DIHENTIKAN. Menyelesaikan video... <<<\n");
                    std::fflush(stdout);
                    g_recording.store(false);
                    MessageBeep(MB_OK);
                }
            }
            else if (msg.wParam == kHotkeyQuit)
            {
                std::printf("Keluar...\n");
                std::fflush(stdout);
                g_running.store(false);
                g_recording.store(false);
                PostQuitMessage(0);
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    g_recording.store(false);

    if (g_recorderThread.joinable())
    {
        g_recorderThread.join();
    }

    if (magnifier.joinable())
    {
        magnifier.join();
    }

    UnregisterHotKey(nullptr, kHotkeyToggle);
    UnregisterHotKey(nullptr, kHotkeyRecord);
    UnregisterHotKey(nullptr, kHotkeyQuit);

    Gdiplus::GdiplusShutdown(gdiplusToken);

    if (pTimeEnd)
        pTimeEnd(1);

    return 0;
}
