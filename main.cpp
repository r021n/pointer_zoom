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
#include <string>
#include <thread>
#include <vector>

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
constexpr int kHotkeyRecord = 3;
constexpr int kPollIntervalMs = 8;

// Perekaman layar
constexpr int kRecordFps = 30;
constexpr UINT32 kRecordBitrate = 8000000; // 8 Mbps (Kualitas tinggi H.264)

// Platformer camera lag parameter (~280ms delay untuk camera follow yang lebih smooth dan terasa delay-nya)
constexpr float kCameraSmoothTime = 0.28f;

// Custom animated pointer overlay dimensions (compact 64x64)
constexpr int kOverlaySize = 64;
constexpr float kTipOffsetX = 18.0f;
constexpr float kTipOffsetY = 18.0f;

struct CameraSharedState {
    std::atomic<bool> isZoomed{false};
    std::atomic<float> curX{0.0f};
    std::atomic<float> curY{0.0f};
    std::atomic<int> mouseX{0};
    std::atomic<int> mouseY{0};
    std::atomic<float> pointerScale{1.0f};
    std::atomic<float> waveProgress{-1.0f};
};

CameraSharedState g_camState;

std::atomic<bool> g_running{true};
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_transformWarned{false};

std::atomic<bool> g_recording{false};
std::atomic<bool> g_saving{false};
std::thread g_recorderThread;

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

    g_overlayHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"PointerZoomOverlayClass",
        L"PointerOverlay",
        WS_POPUP | WS_DISABLED,
        0, 0, kOverlaySize, kOverlaySize,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_overlayHwnd) return false;

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

// Menggambar pointer kustom (dengan ripple click dan indikator REC jika aktif)
void DrawPointerShape(Gdiplus::Graphics& g, float tipX, float tipY, float scale, float waveProgress, bool isRecording) {
    // 1. Expanding click ripple at hotspot tip
    if (waveProgress >= 0.0f && waveProgress <= 1.0f) {
        float radius = (3.0f + 14.0f * waveProgress) * (scale > 1.2f ? 1.4f : 1.0f);
        int alpha = static_cast<int>(180.0f * (1.0f - waveProgress));
        if (alpha > 0) {
            Gdiplus::Pen wavePen(Gdiplus::Color(alpha, 40, 160, 255), 2.2f * (1.0f - waveProgress * 0.5f));
            g.DrawEllipse(&wavePen, tipX - radius, tipY - radius, radius * 2.0f, radius * 2.0f);
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
    const float shadowOffX = 1.5f * (std::max)(1.0f, scale * 0.7f);
    const float shadowOffY = 2.0f * (std::max)(1.0f, scale * 0.7f);

    for (int i = 0; i < 7; ++i) {
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
    if (isRecording) {
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

void RenderPointerOverlay(POINT cursorPt, float scale, float waveProgress) {
    if (!g_overlayHwnd || !g_hdcMem) return;

    const bool isRec = g_recording.load(std::memory_order_relaxed);

    {
        Gdiplus::Graphics g(g_hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        DrawPointerShape(g, kTipOffsetX, kTipOffsetY, scale, waveProgress, isRec);
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
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        double now = GetTimeSeconds();
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;
        dt = std::clamp(dt, 0.001f, 0.050f);

        POINT pt{};
        GetCursorPos(&pt);

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

            // Update shared state for recorder even when zoom is OFF
            g_camState.isZoomed.store(false, std::memory_order_relaxed);
            g_camState.curX.store(0.0f, std::memory_order_relaxed);
            g_camState.curY.store(0.0f, std::memory_order_relaxed);
            g_camState.mouseX.store(pt.x, std::memory_order_relaxed);
            g_camState.mouseY.store(pt.y, std::memory_order_relaxed);
            g_camState.pointerScale.store(1.0f, std::memory_order_relaxed);
            g_camState.waveProgress.store(-1.0f, std::memory_order_relaxed);

            Sleep(kPollIntervalMs * 2);
            continue;
        }

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

        // --- Fast Async Mouse Click Detection ---
        const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        const bool middleDown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        const bool isPressed = leftDown || rightDown || middleDown;

        if (isPressed && !lastPressed) {
            clickPulseTimer = 0.09f;
            waveTimer = 0.0f;
        }
        lastPressed = isPressed;

        bool isDepressed = isPressed;
        if (clickPulseTimer > 0.0f) {
            clickPulseTimer -= dt;
            isDepressed = true;
        }

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
            waveProgress = waveTimer / 0.22f;
            if (waveProgress > 1.0f) {
                waveTimer = -1.0f;
                waveProgress = -1.0f;
            }
        }

        RenderPointerOverlay(pt, curScale, waveProgress);

        // Update shared state for recorder
        g_camState.isZoomed.store(true, std::memory_order_relaxed);
        g_camState.curX.store(curX, std::memory_order_relaxed);
        g_camState.curY.store(curY, std::memory_order_relaxed);
        g_camState.mouseX.store(pt.x, std::memory_order_relaxed);
        g_camState.mouseY.store(pt.y, std::memory_order_relaxed);
        g_camState.pointerScale.store(curScale, std::memory_order_relaxed);
        g_camState.waveProgress.store(waveProgress, std::memory_order_relaxed);

        Sleep(kPollIntervalMs);
    }

    if (tracking) {
        MagSetFullscreenTransform(1.0f, 0, 0);
        MagShowSystemCursor(TRUE);
    }
    CleanupOverlayWindow();
    MagUninitialize();
}

std::wstring GetTempRecordFilePath() {
    wchar_t tempDir[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, tempDir);
    if (len == 0 || len > MAX_PATH) {
        return L"zoom_record_temp.mp4";
    }
    wchar_t tempFile[MAX_PATH];
    if (GetTempFileNameW(tempDir, L"ZRC", 0, tempFile) != 0) {
        std::wstring path = tempFile;
        size_t dot = path.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            path = path.substr(0, dot) + L".mp4";
        } else {
            path += L".mp4";
        }
        return path;
    }
    return std::wstring(tempDir) + L"zoom_record_temp.mp4";
}

void PromptSaveRecording(const std::wstring& tempPath) {
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

    if (GetSaveFileNameW(&ofn)) {
        if (MoveFileExW(tempPath.c_str(), szFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            std::printf("[REC] Video berhasil disimpan: %ls\n", szFile);
            std::fflush(stdout);
            MessageBeep(MB_OK);
            MessageBoxW(nullptr, (std::wstring(L"Rekaman layar berhasil disimpan!\n\nLokasi:\n") + szFile).c_str(),
                        L"Perekaman Selesai", MB_ICONINFORMATION | MB_OK | MB_SYSTEMMODAL);
        } else {
            std::printf("[REC] Gagal memindahkan file rekaman (error %lu).\n", GetLastError());
            std::fflush(stdout);
            DeleteFileW(tempPath.c_str());
        }
    } else {
        std::printf("[REC] Penyimpanan dibatalkan oleh pengguna. File sementara dihapus.\n");
        std::fflush(stdout);
        DeleteFileW(tempPath.c_str());
    }

    g_saving.store(false);
}

// Thread perekaman video layar native Windows Media Foundation
void RecorderThreadFunc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    const int screenW = GetSystemMetrics(SM_CXSCREEN) & ~1; // Pastikan genap untuk encoder H.264
    const int screenH = GetSystemMetrics(SM_CYSCREEN) & ~1;
    const std::wstring tempPath = GetTempRecordFilePath();

    IMFSinkWriter* pSinkWriter = nullptr;
    HRESULT hr = MFCreateSinkWriterFromURL(tempPath.c_str(), nullptr, nullptr, &pSinkWriter);
    if (FAILED(hr)) {
        std::printf("[REC] Gagal membuat sink writer (0x%08lx).\n", hr);
        std::fflush(stdout);
        MFShutdown();
        CoUninitialize();
        g_recording.store(false);
        return;
    }

    DWORD streamIndex = 0;

    // Konfigurasi Video Output: H.264 MP4
    IMFMediaType* pMediaTypeOut = nullptr;
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
    if (FAILED(hr)) {
        std::printf("[REC] Gagal AddStream (0x%08lx).\n", hr);
        std::fflush(stdout);
        pSinkWriter->Release();
        MFShutdown();
        CoUninitialize();
        g_recording.store(false);
        return;
    }

    // Konfigurasi Video Input: RGB32
    IMFMediaType* pMediaTypeIn = nullptr;
    MFCreateMediaType(&pMediaTypeIn);
    pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    pMediaTypeIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(pMediaTypeIn, MF_MT_FRAME_SIZE, screenW, screenH);
    MFSetAttributeRatio(pMediaTypeIn, MF_MT_FRAME_RATE, kRecordFps, 1);
    MFSetAttributeRatio(pMediaTypeIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = pSinkWriter->SetInputMediaType(streamIndex, pMediaTypeIn, nullptr);
    pMediaTypeIn->Release();
    if (FAILED(hr)) {
        std::printf("[REC] Gagal SetInputMediaType (0x%08lx).\n", hr);
        std::fflush(stdout);
        pSinkWriter->Release();
        MFShutdown();
        CoUninitialize();
        g_recording.store(false);
        return;
    }

    hr = pSinkWriter->BeginWriting();
    if (FAILED(hr)) {
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

    void* pFrameBits = nullptr;
    HBITMAP hFrameBmp = CreateDIBSection(hdcFrame, &bmi, DIB_RGB_COLORS, &pFrameBits, nullptr, 0);
    HGDIOBJ oldFrameBmp = SelectObject(hdcFrame, hFrameBmp);

    const DWORD cbBuffer = static_cast<DWORD>(screenW * screenH * 4);
    const LONGLONG frameDuration100ns = 10000000LL / kRecordFps;
    const double frameIntervalSec = 1.0 / static_cast<double>(kRecordFps);

    int64_t frameIndex = 0;
    double startTime = GetTimeSeconds();

    while (g_recording.load(std::memory_order_relaxed) && g_running.load(std::memory_order_relaxed)) {
        // 1. Baca camera zoom & cursor state terkini
        const bool isZoomed = g_camState.isZoomed.load(std::memory_order_relaxed);
        const float curX = g_camState.curX.load(std::memory_order_relaxed);
        const float curY = g_camState.curY.load(std::memory_order_relaxed);
        const int mouseX = g_camState.mouseX.load(std::memory_order_relaxed);
        const int mouseY = g_camState.mouseY.load(std::memory_order_relaxed);
        const float ptrScale = g_camState.pointerScale.load(std::memory_order_relaxed);
        const float waveProg = g_camState.waveProgress.load(std::memory_order_relaxed);

        // 2. Render frame video persis seperti yang terlihat di mata pengguna
        // Langsung blit dari hdcScreen ke hdcFrame tanpa buffer perantara untuk performa tinggi (<2ms per frame)
        if (isZoomed) {
            const int regionW = static_cast<int>(screenW / kZoomLevel);
            const int regionH = static_cast<int>(screenH / kZoomLevel);
            int srcX = std::clamp(static_cast<int>(std::lround(curX)), 0, screenW - regionW);
            int srcY = std::clamp(static_cast<int>(std::lround(curY)), 0, screenH - regionH);

            SetStretchBltMode(hdcFrame, COLORONCOLOR);
            StretchBlt(hdcFrame, 0, 0, screenW, screenH, hdcScreen, srcX, srcY, regionW, regionH, SRCCOPY);

            // Render custom pointer overlay di koordinat layar magnified
            // Catatan: g_overlayHwnd sudah di-exclude dari capture via SetWindowDisplayAffinity
            // sehingga di sini hanya akan ada tepat 1 pointer bersih tanpa duplikat
            float viewCursorX = (static_cast<float>(mouseX) - curX) * kZoomLevel;
            float viewCursorY = (static_cast<float>(mouseY) - curY) * kZoomLevel;

            {
                Gdiplus::Graphics g(hdcFrame);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                DrawPointerShape(g, viewCursorX, viewCursorY, ptrScale * kZoomLevel, waveProg, true);
            }
        } else {
            // Zoom OFF: salin 1:1 seluruh layar
            BitBlt(hdcFrame, 0, 0, screenW, screenH, hdcScreen, 0, 0, SRCCOPY);

            // Gambar kursor Windows aktif
            CURSORINFO ci{};
            ci.cbSize = sizeof(CURSORINFO);
            bool drewCursor = false;
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                ICONINFO ii = {};
                if (GetIconInfo(ci.hCursor, &ii)) {
                    DrawIconEx(hdcFrame, ci.ptScreenPos.x - ii.xHotspot, ci.ptScreenPos.y - ii.yHotspot,
                               ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                    drewCursor = true;
                }
            }
            if (!drewCursor) {
                // Fallback gambar kursor jika handle kursor tidak terbaca
                Gdiplus::Graphics g(hdcFrame);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                DrawPointerShape(g, static_cast<float>(mouseX), static_cast<float>(mouseY), ptrScale, waveProg, true);
            }
        }

        GdiFlush();

        // 3. Encode frame ke Media Foundation
        IMFMediaBuffer* pBuffer = nullptr;
        hr = MFCreateMemoryBuffer(cbBuffer, &pBuffer);
        if (SUCCEEDED(hr)) {
            BYTE* pDst = nullptr;
            pBuffer->Lock(&pDst, nullptr, nullptr);
            memcpy(pDst, pFrameBits, cbBuffer);
            pBuffer->Unlock();
            pBuffer->SetCurrentLength(cbBuffer);

            IMFSample* pSample = nullptr;
            hr = MFCreateSample(&pSample);
            if (SUCCEEDED(hr)) {
                pSample->AddBuffer(pBuffer);
                pBuffer->Release();

                LONGLONG sampleTime = frameIndex * frameDuration100ns;
                pSample->SetSampleTime(sampleTime);
                pSample->SetSampleDuration(frameDuration100ns);

                pSinkWriter->WriteSample(streamIndex, pSample);
                pSample->Release();
            } else {
                pBuffer->Release();
            }
        }

        frameIndex++;

        // 4. Pacing frame rate 30 FPS tersinkronisasi waktu nyata (Real-Time Wall Clock)
        // Memastikan video diputar pada kecepatan normal 1:1 (tidak kecepetan / fast-forward)
        double targetTime = startTime + static_cast<double>(frameIndex) * frameIntervalSec;
        double now = GetTimeSeconds();
        double sleepSec = targetTime - now;

        if (sleepSec > 0.001) {
            Sleep(static_cast<DWORD>(sleepSec * 1000.0));
        } else if (sleepSec < -frameIntervalSec) {
            // Jika ada frame delay dari OS, tulis duplicate frame catch-up agar total durasi video pas persis dengan waktu asli
            int missedFrames = static_cast<int>((-sleepSec) / frameIntervalSec);
            missedFrames = (std::min)(missedFrames, 5);
            for (int d = 0; d < missedFrames && g_recording.load(std::memory_order_relaxed); ++d) {
                IMFMediaBuffer* pDupBuf = nullptr;
                if (SUCCEEDED(MFCreateMemoryBuffer(cbBuffer, &pDupBuf))) {
                    BYTE* pDst = nullptr;
                    pDupBuf->Lock(&pDst, nullptr, nullptr);
                    memcpy(pDst, pFrameBits, cbBuffer);
                    pDupBuf->Unlock();
                    pDupBuf->SetCurrentLength(cbBuffer);

                    IMFSample* pDupSample = nullptr;
                    if (SUCCEEDED(MFCreateSample(&pDupSample))) {
                        pDupSample->AddBuffer(pDupBuf);
                        pDupBuf->Release();

                        LONGLONG sampleTime = frameIndex * frameDuration100ns;
                        pDupSample->SetSampleTime(sampleTime);
                        pDupSample->SetSampleDuration(frameDuration100ns);

                        pSinkWriter->WriteSample(streamIndex, pDupSample);
                        pDupSample->Release();
                    } else {
                        pDupBuf->Release();
                    }
                }
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
    if (frameIndex > 0 && g_running.load(std::memory_order_relaxed)) {
        PromptSaveRecording(tempPath);
    } else {
        DeleteFileW(tempPath.c_str());
    }
}

void PrintBanner() {
    std::printf("========================================================\n");
    std::printf("  Zoom Pointer & Screen Recorder (Hardware-Accelerated)\n");
    std::printf("========================================================\n");
    std::printf("  Ctrl+Alt+Z : Toggle zoom %.1fx (platformer smooth follow & pointer fx)\n",
                kZoomLevel);
    std::printf("  Ctrl+Alt+R : Mulai / Stop rekaman layar (sesuai kondisi zoom)\n");
    std::printf("  Ctrl+Alt+Q : Keluar & bersihkan memori\n");
    std::printf("--------------------------------------------------------\n");
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
    const bool hotkeyRecordOk =
        RegisterHotKey(nullptr, kHotkeyRecord, MOD_CONTROL | MOD_ALT, 'R');
    const bool hotkeyQuitOk =
        RegisterHotKey(nullptr, kHotkeyQuit, MOD_CONTROL | MOD_ALT, 'Q');

    if (!hotkeyToggleOk || !hotkeyRecordOk || !hotkeyQuitOk) {
        std::printf("[!] RegisterHotKey gagal (error %lu). Hotkey mungkin dipakai aplikasi lain.\n",
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
            } else if (msg.wParam == kHotkeyRecord) {
                if (g_saving.load(std::memory_order_relaxed)) {
                    std::printf("[REC] Sedang memproses penyimpanan dialog sebelumnya, harap tunggu...\n");
                    std::fflush(stdout);
                } else if (!g_recording.load()) {
                    if (g_recorderThread.joinable()) {
                        g_recorderThread.join();
                    }
                    g_recording.store(true);
                    MessageBeep(MB_ICONASTERISK);
                    std::printf("\n[REC] >>> PEREKAMAN DIMULAI (Ctrl+Alt+R untuk berhenti) <<<\n");
                    std::fflush(stdout);
                    g_recorderThread = std::thread(RecorderThreadFunc);
                } else {
                    std::printf("\n[REC] >>> PEREKAMAN DIHENTIKAN. Menyelesaikan video... <<<\n");
                    std::fflush(stdout);
                    g_recording.store(false);
                    MessageBeep(MB_OK);
                }
            } else if (msg.wParam == kHotkeyQuit) {
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

    if (g_recorderThread.joinable()) {
        g_recorderThread.join();
    }

    if (magnifier.joinable()) {
        magnifier.join();
    }

    UnregisterHotKey(nullptr, kHotkeyToggle);
    UnregisterHotKey(nullptr, kHotkeyRecord);
    UnregisterHotKey(nullptr, kHotkeyQuit);

    Gdiplus::GdiplusShutdown(gdiplusToken);

    return 0;
}
