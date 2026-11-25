#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>

#include "port_types.h"
#include "ui_theme.h"

namespace porttool {

struct HitButton {
    D2D1_RECT_F  rect{};
    int          actionId = 0;
    std::wstring payload;
};

class AppUI {
public:
    AppUI();
    ~AppUI();

    bool Initialize(HWND hwnd);
    void Shutdown();

    void Resize(UINT width, UINT height);
    void UpdateData(std::vector<PortInfo> ports, std::vector<SysCompInfo> sysComps);
    void Render();

    // Input handlers
    void OnMouseMove(float dipX, float dipY);
    void OnMouseLeave();
    void OnLButtonDown(float dipX, float dipY);
    void OnMouseWheel(int delta);
    void OnKeyDown(WPARAM vk);

    bool IsDarkMode() const { return isDark_; }
    void SetDarkMode(bool dark);

private:
    enum MainTab {
        kTabPorts = 0,
        kTabSystem
    };

    enum ActionId {
        kActNone = 0,
        kActToggleTheme,
        kActSwitchTabPorts,
        kActSwitchTabSystem,
        kActFilterAll,
        kActFilterUSB,
        kActFilterNet,
        kActFilterPower,
        kActFilterDisplay,
        kActFilterAudio,
        kActFilterSerial,
        kActBackToMain,
        kActOpenPort,
        kActBackToSysMain,
        kActOpenSysComp
    };

    bool CreateDeviceResources();
    void DiscardDeviceResources();
    void ApplyWindowTheme();

    void DrawTopBar(float width);

    // Ports Tab
    void DrawMainPage(float width, float height);
    void DrawDetailPage(float width, float height);
    void DrawDataGraph(const D2D1_RECT_F& cardRect, const PortInfo& port);
    void DrawPowerGraph(const D2D1_RECT_F& cardRect, const PortInfo& port);

    // System Hardware Tab (CPU, GPU(s), Disk(s), Memory)
    void DrawSystemMainPage(float width, float height);
    void DrawSystemDetailPage(float width, float height);
    void DrawSysUtilGraph(const D2D1_RECT_F& cardRect, const SysCompInfo& comp);
    void DrawSysDataGraph(const D2D1_RECT_F& cardRect, const SysCompInfo& comp);
    void DrawSysPowerGraph(const D2D1_RECT_F& cardRect, const SysCompInfo& comp);
    void DrawSysTempGraph(const D2D1_RECT_F& cardRect, const SysCompInfo& comp);

    void DrawRoundedRect(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& fill, const D2D1_COLOR_F& stroke, float strokeWidth = 1.0f);
    void DrawTextLine(const std::wstring& text, const D2D1_RECT_F& r, IDWriteTextFormat* fmt, const D2D1_COLOR_F& color,
                      DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

    std::vector<const PortInfo*> GetFilteredPorts() const;
    const PortInfo*    FindSelectedPort() const;
    const SysCompInfo* FindSelectedSysComp() const;

    HWND hwnd_ = nullptr;

    ID2D1Factory*          d2dFactory_ = nullptr;
    ID2D1HwndRenderTarget* rt_         = nullptr;
    ID2D1SolidColorBrush*  brush_      = nullptr;

    IDWriteFactory*     dwFactory_  = nullptr;
    IDWriteTextFormat*  fmtTitle_   = nullptr;
    IDWriteTextFormat*  fmtHeading_ = nullptr;
    IDWriteTextFormat*  fmtBody_    = nullptr;
    IDWriteTextFormat*  fmtBold_    = nullptr;
    IDWriteTextFormat*  fmtSmall_   = nullptr;
    IDWriteTextFormat*  fmtMono_    = nullptr;
    IDWriteTextFormat*  fmtKpi_     = nullptr;

    bool         isDark_ = true;
    ThemePalette theme_{};

    // Navigation & View state
    MainTab      activeTab_      = kTabPorts;
    bool         inDetailView_   = false;
    std::wstring selectedPortId_;
    int          categoryFilter_ = -1;
    float        scrollY_        = 0.0f;
    float        maxScrollY_     = 0.0f;

    bool         inSysDetailView_ = false;
    std::wstring selectedSysId_;
    float        sysScrollY_      = 0.0f;
    float        sysMaxScrollY_   = 0.0f;

    // Hover state
    float mouseX_ = -1.0f;
    float mouseY_ = -1.0f;
    bool  mouseInside_ = false;

    // Interactive hit-test regions rebuilt every frame
    std::vector<HitButton> hitButtons_;

    // Latest snapshots
    std::vector<PortInfo>    ports_;
    std::vector<SysCompInfo> sysComps_;
};

} // namespace porttool
