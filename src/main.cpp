#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>

#include "port_monitor.h"
#include "system_monitor.h"
#include "app_ui.h"

#include <atomic>
#include <thread>

namespace {

constexpr UINT WM_PORTS_UPDATED = WM_APP + 1;

struct AppContext {
    porttool::PortMonitor   portMonitor;
    porttool::SystemMonitor sysMonitor;
    porttool::AppUI         ui;
    std::atomic<bool>       running{true};
    std::thread             pollThread;
    bool                    trackingMouse = false;
};

float GetDpiScale(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    return (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<AppContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_SIZE:
            if (ctx) {
                ctx->ui.Resize(LOWORD(lParam), HIWORD(lParam));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            if (ctx) ctx->ui.Render();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // Prevent flicker; Direct2D paints entire client area

        case WM_PORTS_UPDATED:
            if (ctx) {
                ctx->ui.UpdateData(ctx->portMonitor.GetSnapshot(), ctx->sysMonitor.GetSnapshot());
            }
            return 0;

        case WM_MOUSEMOVE:
            if (ctx) {
                if (!ctx->trackingMouse) {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize    = sizeof(tme);
                    tme.dwFlags   = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                    ctx->trackingMouse = true;
                }
                float scale = GetDpiScale(hwnd);
                ctx->ui.OnMouseMove(GET_X_LPARAM(lParam) / scale, GET_Y_LPARAM(lParam) / scale);
            }
            return 0;

        case WM_MOUSELEAVE:
            if (ctx) {
                ctx->trackingMouse = false;
                ctx->ui.OnMouseLeave();
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (ctx) {
                float scale = GetDpiScale(hwnd);
                ctx->ui.OnLButtonDown(GET_X_LPARAM(lParam) / scale, GET_Y_LPARAM(lParam) / scale);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (ctx) {
                ctx->ui.OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            }
            return 0;

        case WM_KEYDOWN:
            if (ctx) {
                ctx->ui.OnKeyDown(wParam);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 860;
            mmi->ptMinTrackSize.y = 560;
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"PortToolMainWindowClass";
    RegisterClassExW(&wc);

    AppContext ctx;

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Port Tool - Read-Only Hardware Port Monitor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 740,
        nullptr, nullptr, hInstance, &ctx);

    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    if (!ctx.ui.Initialize(hwnd)) {
        DestroyWindow(hwnd);
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Background 2 Hz (0.5 sec) read-only polling thread
    ctx.pollThread = std::thread([&ctx, hwnd]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (ctx.running.load(std::memory_order_relaxed)) {
            uint64_t t0 = GetTickCount64();
            ctx.portMonitor.PollOnce();
            ctx.sysMonitor.PollOnce();
            PostMessageW(hwnd, WM_PORTS_UPDATED, 0, 0);

            uint64_t elapsed = GetTickCount64() - t0;
            uint64_t remain  = (elapsed < porttool::kSampleIntervalMs) ? (porttool::kSampleIntervalMs - elapsed) : 0;
            while (remain > 0 && ctx.running.load(std::memory_order_relaxed)) {
                DWORD step = static_cast<DWORD>(remain > 50 ? 50 : remain);
                Sleep(step);
                remain -= step;
            }
        }
        CoUninitialize();
    });

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ctx.running.store(false, std::memory_order_relaxed);
    if (ctx.pollThread.joinable()) {
        ctx.pollThread.join();
    }

    ctx.ui.Shutdown();
    CoUninitialize();
    return 0;
}

