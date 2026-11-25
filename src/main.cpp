#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>

#include "port_monitor.h"
#include "system_monitor.h"
#include "app_ui.h"

#include <atomic>
#include <cwchar>
#include <thread>

namespace {

constexpr UINT WM_PORTS_UPDATED  = WM_APP + 1;
constexpr UINT WM_TRAY_ICON      = WM_APP + 2;
constexpr UINT WM_RESTORE_WINDOW = WM_APP + 3;
constexpr UINT kTrayIconId       = 1001;
constexpr UINT kCmdTrayOpen      = 2001;
constexpr UINT kCmdTrayExit      = 2002;

struct AppContext {
    porttool::PortMonitor   portMonitor;
    porttool::SystemMonitor sysMonitor;
    porttool::AppUI         ui;
    std::atomic<bool>       running{true};
    std::thread             pollThread;
    bool                    trackingMouse = false;
    bool                    trayAdded     = false;
    NOTIFYICONDATAW         nid{};
};

void AddTrayIcon(HWND hwnd, AppContext* ctx) {
    if (ctx->trayAdded) return;
    ctx->nid = {};
    ctx->nid.cbSize           = sizeof(NOTIFYICONDATAW);
    ctx->nid.hWnd             = hwnd;
    ctx->nid.uID              = kTrayIconId;
    ctx->nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    ctx->nid.uCallbackMessage = WM_TRAY_ICON;
    ctx->nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(ctx->nid.szTip, L"Port Tool - Read-Only Hardware Monitor");
    if (Shell_NotifyIconW(NIM_ADD, &ctx->nid)) {
        ctx->trayAdded = true;
    }
}

void RemoveTrayIcon(AppContext* ctx) {
    if (!ctx->trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &ctx->nid);
    ctx->trayAdded = false;
}

void RestoreFromTray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

void ShowTrayContextMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdTrayOpen, L"Open Port Tool");
    SetMenuDefaultItem(menu, kCmdTrayOpen, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdTrayExit, L"Exit Port Tool");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

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

        case WM_TIMER:
            if (ctx && wParam == 1 && ctx->ui.NeedsFastRedraw()) {
                InvalidateRect(hwnd, nullptr, FALSE);
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

        case WM_CLOSE:
            if (ctx && ctx->ui.GetMinimizeToTray()) {
                AddTrayIcon(hwnd, ctx);
                ctx->ui.OnHiddenToTray();
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_TRAY_ICON:
            if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                RestoreFromTray(hwnd);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayContextMenu(hwnd);
            }
            return 0;

        case WM_RESTORE_WINDOW:
            RestoreFromTray(hwnd);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == kCmdTrayOpen) {
                RestoreFromTray(hwnd);
            } else if (LOWORD(wParam) == kCmdTrayExit) {
                if (ctx) RemoveTrayIcon(ctx);
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DESTROY:
            if (ctx) RemoveTrayIcon(ctx);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    // Single-instance check: if already running (e.g. in tray), restore existing window
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\PortTool_SingleInstance_Mutex");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"PortToolMainWindowClass", nullptr);
        if (existing) {
            PostMessageW(existing, WM_RESTORE_WINDOW, 0, 0);
        }
        CloseHandle(hMutex);
        return 0;
    }

    bool startMinimized = (pCmdLine && wcsstr(pCmdLine, L"--minimized") != nullptr);

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

    AddTrayIcon(hwnd, &ctx);
    if (startMinimized) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
    }
    SetTimer(hwnd, 1, 33, nullptr); // 30 FPS refresh ONLY when viewing live audio waveform

    // Background live read-only polling thread (respects Settings sample rate)
    ctx.pollThread = std::thread([&ctx, hwnd]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (ctx.running.load(std::memory_order_relaxed)) {
            uint64_t t0 = GetTickCount64();
            ctx.portMonitor.PollOnce();
            ctx.sysMonitor.PollOnce();
            PostMessageW(hwnd, WM_PORTS_UPDATED, 0, 0);

            uint64_t targetMs = ctx.ui.GetSampleIntervalMs();
            uint64_t elapsed  = GetTickCount64() - t0;
            uint64_t remain   = (elapsed < targetMs) ? (targetMs - elapsed) : 0;
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
    if (hMutex) CloseHandle(hMutex);
    return 0;
}

