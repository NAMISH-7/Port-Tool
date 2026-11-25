#include "app_ui.h"

#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace porttool {

namespace {

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

bool PtInRectF(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

double NiceUpperBound(double maxVal, double minFloor) {
    if (maxVal < minFloor) return minFloor;
    double exp10 = std::pow(10.0, std::floor(std::log10(maxVal)));
    double frac  = maxVal / exp10;
    double nice  = 1.0;
    if (frac <= 1.2)      nice = 1.2;
    else if (frac <= 2.0) nice = 2.0;
    else if (frac <= 2.5) nice = 2.5;
    else if (frac <= 5.0) nice = 5.0;
    else                  nice = 10.0;
    return nice * exp10;
}

} // namespace

AppUI::AppUI() {
    theme_ = MakeDarkTheme();
}

AppUI::~AppUI() {
    Shutdown();
}

bool AppUI::Initialize(HWND hwnd) {
    hwnd_ = hwnd;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwFactory_)))) {
        return false;
    }

    auto makeFmt = [&](const wchar_t* font, float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** outFmt) {
        if (FAILED(dwFactory_->CreateTextFormat(font, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", outFmt))) {
            return false;
        }
        (*outFmt)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        (*outFmt)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{};
        trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        IDWriteInlineObject* ellipsis = nullptr;
        if (SUCCEEDED(dwFactory_->CreateEllipsisTrimmingSign(*outFmt, &ellipsis)) && ellipsis) {
            (*outFmt)->SetTrimming(&trim, ellipsis);
            ellipsis->Release();
        }
        return true;
    };

    if (!makeFmt(L"Segoe UI", 18.0f, DWRITE_FONT_WEIGHT_BOLD,      &fmtTitle_))   return false;
    if (!makeFmt(L"Segoe UI", 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtHeading_)) return false;
    if (!makeFmt(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_NORMAL,    &fmtBody_))    return false;
    if (!makeFmt(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtBold_))    return false;
    if (!makeFmt(L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_NORMAL,    &fmtSmall_))   return false;
    if (!makeFmt(L"Consolas", 12.5f, DWRITE_FONT_WEIGHT_NORMAL,    &fmtMono_))    return false;
    if (!makeFmt(L"Segoe UI", 22.0f, DWRITE_FONT_WEIGHT_BOLD,      &fmtKpi_))     return false;

    ApplyWindowTheme();
    return CreateDeviceResources();
}

void AppUI::Shutdown() {
    DiscardDeviceResources();
    SafeRelease(fmtTitle_);
    SafeRelease(fmtHeading_);
    SafeRelease(fmtBody_);
    SafeRelease(fmtBold_);
    SafeRelease(fmtSmall_);
    SafeRelease(fmtMono_);
    SafeRelease(fmtKpi_);
    SafeRelease(dwFactory_);
    SafeRelease(d2dFactory_);
}

bool AppUI::CreateDeviceResources() {
    if (rt_) return true;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(
        std::max<UINT>(1, rc.right - rc.left),
        std::max<UINT>(1, rc.bottom - rc.top));

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = 0.0f;
    rtProps.dpiY = 0.0f;

    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        rtProps,
        D2D1::HwndRenderTargetProperties(hwnd_, size),
        &rt_);
    if (FAILED(hr)) return false;

    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    hr = rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush_);
    return SUCCEEDED(hr);
}

void AppUI::DiscardDeviceResources() {
    SafeRelease(brush_);
    SafeRelease(rt_);
}

void AppUI::ApplyWindowTheme() {
    if (!hwnd_) return;
    BOOL useDark = isDark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
}

void AppUI::SetDarkMode(bool dark) {
    isDark_ = dark;
    theme_  = isDark_ ? MakeDarkTheme() : MakeLightTheme();
    ApplyWindowTheme();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppUI::Resize(UINT width, UINT height) {
    if (rt_ && width > 0 && height > 0) {
        rt_->Resize(D2D1::SizeU(width, height));
    }
}

void AppUI::UpdateData(std::vector<PortInfo> ports) {
    ports_ = std::move(ports);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppUI::OnMouseMove(float dipX, float dipY) {
    mouseX_ = dipX;
    mouseY_ = dipY;
    mouseInside_ = true;

    bool overClickable = false;
    for (const auto& btn : hitButtons_) {
        if (PtInRectF(btn.rect, dipX, dipY)) {
            overClickable = true;
            break;
        }
    }
    SetCursor(LoadCursorW(nullptr, overClickable ? IDC_HAND : IDC_ARROW));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppUI::OnMouseLeave() {
    mouseInside_ = false;
    mouseX_ = -1.0f;
    mouseY_ = -1.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppUI::OnLButtonDown(float dipX, float dipY) {
    for (auto it = hitButtons_.rbegin(); it != hitButtons_.rend(); ++it) {
        if (PtInRectF(it->rect, dipX, dipY)) {
            switch (it->actionId) {
                case kActToggleTheme:
                    SetDarkMode(!isDark_);
                    break;
                case kActFilterAll:
                    categoryFilter_ = -1;
                    scrollY_ = 0.0f;
                    inDetailView_ = false;
                    break;
                case kActFilterUSB:
                    categoryFilter_ = static_cast<int>(PortCategory::USB);
                    scrollY_ = 0.0f;
                    inDetailView_ = false;
                    break;
                case kActFilterNet:
                    categoryFilter_ = static_cast<int>(PortCategory::Network);
                    scrollY_ = 0.0f;
                    inDetailView_ = false;
                    break;
                case kActFilterPower:
                    categoryFilter_ = static_cast<int>(PortCategory::Power);
                    scrollY_ = 0.0f;
                    inDetailView_ = false;
                    break;
                case kActFilterDisplay:
                    categoryFilter_ = static_cast<int>(PortCategory::Display);
                    scrollY_ = 0.0f;
                    inDetailView_ = false;
                    break;
                case kActFilterAudio:
                    categoryFilter_ = static_cast<int>(PortCategory::Audio);
                    scrollY_ = 0.0f;
                    inDetailView_ = false;
                    break;
                case kActFilterSerial:
                    categoryFilter_ = static_cast<int>(PortCategory::Serial);
                    scrollY_ = 0.0f;
                    inDetailView_ = false;
                    break;
                case kActBackToMain:
                    inDetailView_ = false;
                    selectedPortId_.clear();
                    break;
                case kActOpenPort:
                    selectedPortId_ = it->payload;
                    inDetailView_ = true;
                    break;
                default:
                    break;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
}

void AppUI::OnMouseWheel(int delta) {
    if (inDetailView_) return;
    scrollY_ -= (static_cast<float>(delta) / 120.0f) * 48.0f;
    scrollY_ = std::clamp(scrollY_, 0.0f, maxScrollY_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppUI::OnKeyDown(WPARAM vk) {
    if ((vk == VK_ESCAPE || vk == VK_BACK) && inDetailView_) {
        inDetailView_ = false;
        selectedPortId_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

std::vector<const PortInfo*> AppUI::GetFilteredPorts() const {
    std::vector<const PortInfo*> list;
    list.reserve(ports_.size());
    for (const auto& p : ports_) {
        if (categoryFilter_ >= 0 && static_cast<int>(p.category) != categoryFilter_) continue;
        list.push_back(&p);
    }
    return list;
}

const PortInfo* AppUI::FindSelectedPort() const {
    for (const auto& p : ports_) {
        if (p.id == selectedPortId_) return &p;
    }
    return nullptr;
}

void AppUI::DrawRoundedRect(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& fill,
                            const D2D1_COLOR_F& stroke, float strokeWidth) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    if (fill.a > 0.0f) {
        brush_->SetColor(fill);
        rt_->FillRoundedRectangle(rr, brush_);
    }
    if (stroke.a > 0.0f && strokeWidth > 0.0f) {
        brush_->SetColor(stroke);
        rt_->DrawRoundedRectangle(rr, brush_, strokeWidth);
    }
}

void AppUI::DrawTextLine(const std::wstring& text, const D2D1_RECT_F& r, IDWriteTextFormat* fmt,
                         const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align) {
    fmt->SetTextAlignment(align);
    brush_->SetColor(color);
    rt_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, r, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void AppUI::Render() {
    if (!CreateDeviceResources()) return;

    hitButtons_.clear();

    rt_->BeginDraw();
    rt_->Clear(theme_.bgWindow);

    D2D1_SIZE_F sz = rt_->GetSize();
    float w = sz.width;
    float h = sz.height;

    DrawTopBar(w);

    if (inDetailView_) {
        DrawDetailPage(w, h);
    } else {
        DrawMainPage(w, h);
    }

    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

void AppUI::DrawTopBar(float width) {
    constexpr float kTopH = 56.0f;
    D2D1_RECT_F bar = D2D1::RectF(0, 0, width, kTopH);
    brush_->SetColor(theme_.bgHeader);
    rt_->FillRectangle(bar, brush_);

    brush_->SetColor(theme_.border);
    rt_->DrawLine(D2D1::Point2F(0, kTopH), D2D1::Point2F(width, kTopH), brush_, 1.0f);

    // App Title
    DrawTextLine(L"Port Tool", D2D1::RectF(20, 8, 140, 34), fmtTitle_, theme_.textPrimary);
    DrawTextLine(L"Live Hardware Port Monitor", D2D1::RectF(20, 32, 220, 50), fmtSmall_, theme_.textSecondary);

    // Read-Only Guarantee Badge
    D2D1_RECT_F roRect = D2D1::RectF(215, 16, 330, 40);
    DrawRoundedRect(roRect, 12.0f, theme_.bgBadgeActive, D2D1::ColorF(0, 0, 0, 0), 0.0f);
    DrawTextLine(L"Read-Only Mode", roRect, fmtSmall_, theme_.statusActive, DWRITE_TEXT_ALIGNMENT_CENTER);

    // Theme Toggle Button (Right aligned)
    D2D1_RECT_F themeBtn = D2D1::RectF(width - 148.0f, 12.0f, width - 20.0f, 44.0f);
    bool hoverTheme = mouseInside_ && PtInRectF(themeBtn, mouseX_, mouseY_);
    DrawRoundedRect(themeBtn, 8.0f, hoverTheme ? theme_.bgSurfaceHover : theme_.bgPillIdle, theme_.borderStrong, 1.0f);
    DrawTextLine(isDark_ ? L"Light Mode" : L"Dark Mode", themeBtn, fmtBold_, theme_.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    hitButtons_.push_back({themeBtn, kActToggleTheme, L""});
}

void AppUI::DrawMainPage(float width, float height) {
    constexpr float kPad   = 20.0f;
    float y = 72.0f;

    // 1. Summary KPI Row
    size_t totalPorts  = ports_.size();
    size_t activePorts = 0;
    double totalToBps  = 0.0;
    double totalFroBps = 0.0;
    double sysPowerW   = 0.0;
    PowerDirection sysPowerDir = PowerDirection::None;
    bool hasSysPower = false;

    for (const auto& p : ports_) {
        if (p.active) ++activePorts;
        if (p.current.hasDataTo)  totalToBps  += p.current.bytesToSec;
        if (p.current.hasDataFro) totalFroBps += p.current.bytesFroSec;
        if (p.category == PortCategory::Power && p.current.hasPower && !hasSysPower) {
            hasSysPower = true;
            sysPowerW   = p.current.powerWatts;
            sysPowerDir = p.current.powerDir;
        }
    }

    float kpiGap = 12.0f;
    float kpiW   = (width - kPad * 2.0f - kpiGap * 3.0f) / 4.0f;
    float kpiH   = 68.0f;

    struct KpiItem { std::wstring label; std::wstring value; D2D1_COLOR_F accent; };
    std::vector<KpiItem> kpis = {
        { L"Active / Total Ports", std::to_wstring(activePorts) + L" / " + std::to_wstring(totalPorts), theme_.statusActive },
        { L"Total Data To (Out)",  FormatBytesPerSec(totalToBps, true), theme_.seriesTo },
        { L"Total Data Fro (In)",  FormatBytesPerSec(totalFroBps, true), theme_.seriesFro },
        { L"System Power",         FormatPower(sysPowerW, sysPowerDir, hasSysPower),
          (sysPowerDir == PowerDirection::Accepted) ? theme_.seriesPowerAcc : theme_.seriesPowerDel }
    };

    for (size_t i = 0; i < kpis.size(); ++i) {
        float x = kPad + static_cast<float>(i) * (kpiW + kpiGap);
        D2D1_RECT_F card = D2D1::RectF(x, y, x + kpiW, y + kpiH);
        DrawRoundedRect(card, 10.0f, theme_.bgSurface, theme_.border, 1.0f);
        DrawTextLine(kpis[i].label, D2D1::RectF(x + 14, y + 10, x + kpiW - 14, y + 28), fmtSmall_, theme_.textSecondary);
        DrawTextLine(kpis[i].value, D2D1::RectF(x + 14, y + 30, x + kpiW - 14, y + 60), fmtKpi_, kpis[i].accent);
    }

    y += kpiH + 16.0f;

    // 2. Category Filter Pills
    struct FilterPill { const wchar_t* label; int filterVal; int actId; };
    const FilterPill pills[] = {
        { L"All Ports", -1,                                      kActFilterAll },
        { L"USB",       static_cast<int>(PortCategory::USB),     kActFilterUSB },
        { L"Network",   static_cast<int>(PortCategory::Network), kActFilterNet },
        { L"Power",     static_cast<int>(PortCategory::Power),   kActFilterPower },
        { L"Display",   static_cast<int>(PortCategory::Display), kActFilterDisplay },
        { L"Audio",     static_cast<int>(PortCategory::Audio),   kActFilterAudio },
        { L"Serial",    static_cast<int>(PortCategory::Serial),  kActFilterSerial },
    };

    float px = kPad;
    for (const auto& pill : pills) {
        float pw = (pill.filterVal == -1) ? 84.0f : 76.0f;
        D2D1_RECT_F r = D2D1::RectF(px, y, px + pw, y + 30.0f);
        bool selected = (categoryFilter_ == pill.filterVal);
        bool hover    = mouseInside_ && PtInRectF(r, mouseX_, mouseY_);
        D2D1_COLOR_F bg = selected ? theme_.bgPillActive : (hover ? theme_.bgSurfaceHover : theme_.bgPillIdle);
        D2D1_COLOR_F fg = selected ? theme_.textOnAccent : theme_.textPrimary;
        DrawRoundedRect(r, 15.0f, bg, selected ? D2D1::ColorF(0, 0, 0, 0) : theme_.border, 1.0f);
        DrawTextLine(pill.label, r, fmtSmall_, fg, DWRITE_TEXT_ALIGNMENT_CENTER);
        hitButtons_.push_back({r, pill.actId, L""});
        px += pw + 8.0f;
    }

    DrawTextLine(L"Click any port name to view its live graphs",
                 D2D1::RectF(px + 10.0f, y, width - kPad, y + 30.0f),
                 fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);

    y += 42.0f;

    // 3. Main Ports Table Container
    D2D1_RECT_F tableBox = D2D1::RectF(kPad, y, width - kPad, height - 16.0f);
    DrawRoundedRect(tableBox, 10.0f, theme_.bgSurface, theme_.border, 1.0f);

    float tw = tableBox.right - tableBox.left;
    float colName   = tableBox.left + 16.0f;
    float colCat    = tableBox.left + tw * 0.27f;
    float colStat   = tableBox.left + tw * 0.35f;
    float colDev    = tableBox.left + tw * 0.45f;
    float colTo     = tableBox.left + tw * 0.68f;
    float colFro    = tableBox.left + tw * 0.78f;
    float colPwr    = tableBox.left + tw * 0.88f;

    // Table Header
    constexpr float kHdrH = 36.0f;
    D2D1_RECT_F hdrRect = D2D1::RectF(tableBox.left, tableBox.top, tableBox.right, tableBox.top + kHdrH);
    brush_->SetColor(theme_.border);
    rt_->DrawLine(D2D1::Point2F(hdrRect.left, hdrRect.bottom), D2D1::Point2F(hdrRect.right, hdrRect.bottom), brush_, 1.0f);

    DrawTextLine(L"PORT NAME",        D2D1::RectF(colName, hdrRect.top, colCat - 8,      hdrRect.bottom), fmtSmall_, theme_.textMuted);
    DrawTextLine(L"TYPE",             D2D1::RectF(colCat,  hdrRect.top, colStat - 8,     hdrRect.bottom), fmtSmall_, theme_.textMuted);
    DrawTextLine(L"STATUS",           D2D1::RectF(colStat, hdrRect.top, colDev - 8,      hdrRect.bottom), fmtSmall_, theme_.textMuted);
    DrawTextLine(L"CONNECTED DEVICE", D2D1::RectF(colDev,  hdrRect.top, colTo - 8,       hdrRect.bottom), fmtSmall_, theme_.textMuted);
    DrawTextLine(L"TO (OUT)",         D2D1::RectF(colTo,   hdrRect.top, colFro - 8,      hdrRect.bottom), fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawTextLine(L"FRO (IN)",         D2D1::RectF(colFro,  hdrRect.top, colPwr - 8,      hdrRect.bottom), fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawTextLine(L"POWER",            D2D1::RectF(colPwr,  hdrRect.top, tableBox.right - 16, hdrRect.bottom), fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);

    // Scrollable Rows
    auto list = GetFilteredPorts();
    constexpr float kRowH = 44.0f;
    float listTop  = hdrRect.bottom;
    float listH    = std::max(0.0f, tableBox.bottom - listTop);
    float contentH = static_cast<float>(list.size()) * kRowH;
    maxScrollY_    = std::max(0.0f, contentH - listH);
    scrollY_       = std::clamp(scrollY_, 0.0f, maxScrollY_);

    rt_->PushAxisAlignedClip(D2D1::RectF(tableBox.left + 1.0f, listTop + 1.0f, tableBox.right - 1.0f, tableBox.bottom - 1.0f),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (list.empty()) {
        DrawTextLine(L"No ports detected in this category.",
                     D2D1::RectF(tableBox.left, listTop + 20, tableBox.right, listTop + 60),
                     fmtBody_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        for (size_t i = 0; i < list.size(); ++i) {
            const PortInfo* p = list[i];
            float ry = listTop + static_cast<float>(i) * kRowH - scrollY_;
            if (ry + kRowH < listTop || ry > tableBox.bottom) continue;

            D2D1_RECT_F rowRect = D2D1::RectF(tableBox.left + 1.0f, ry, tableBox.right - 1.0f, ry + kRowH);
            bool hover = mouseInside_ && PtInRectF(rowRect, mouseX_, mouseY_) &&
                         mouseY_ >= listTop && mouseY_ <= tableBox.bottom;

            if (hover) {
                brush_->SetColor(theme_.bgSurfaceHover);
                rt_->FillRectangle(rowRect, brush_);
            }

            // Row separator
            brush_->SetColor(theme_.gridLine);
            rt_->DrawLine(D2D1::Point2F(rowRect.left + 12.0f, rowRect.bottom),
                          D2D1::Point2F(rowRect.right - 12.0f, rowRect.bottom), brush_, 1.0f);

            // Clickable Port Name
            DrawTextLine(p->name, D2D1::RectF(colName, ry, colCat - 8, ry + kRowH),
                         fmtBold_, hover ? theme_.textAccent : theme_.textPrimary);

            // Category
            DrawTextLine(CategoryToString(p->category), D2D1::RectF(colCat, ry, colStat - 8, ry + kRowH),
                         fmtSmall_, theme_.textSecondary);

            // Status Badge
            D2D1_RECT_F badge = D2D1::RectF(colStat, ry + 11.0f, std::min(colDev - 12.0f, colStat + 84.0f), ry + kRowH - 11.0f);
            DrawRoundedRect(badge, 11.0f, p->active ? theme_.bgBadgeActive : theme_.bgBadgeUnused,
                            D2D1::ColorF(0, 0, 0, 0), 0.0f);
            DrawTextLine(p->statusText, badge, fmtSmall_,
                         p->active ? theme_.statusActive : theme_.statusUnused, DWRITE_TEXT_ALIGNMENT_CENTER);

            // Connected Device
            DrawTextLine(p->connectedDevice.empty() ? L"N/A" : p->connectedDevice,
                         D2D1::RectF(colDev, ry, colTo - 8, ry + kRowH),
                         fmtBody_, p->active ? theme_.textPrimary : theme_.textMuted);

            // To (Out)
            std::wstring toStr = FormatBytesPerSec(p->current.bytesToSec, p->current.hasDataTo);
            DrawTextLine(toStr, D2D1::RectF(colTo, ry, colFro - 8, ry + kRowH),
                         fmtMono_, p->current.hasDataTo ? theme_.seriesTo : theme_.textMuted,
                         DWRITE_TEXT_ALIGNMENT_TRAILING);

            // Fro (In)
            std::wstring froStr = FormatBytesPerSec(p->current.bytesFroSec, p->current.hasDataFro);
            DrawTextLine(froStr, D2D1::RectF(colFro, ry, colPwr - 8, ry + kRowH),
                         fmtMono_, p->current.hasDataFro ? theme_.seriesFro : theme_.textMuted,
                         DWRITE_TEXT_ALIGNMENT_TRAILING);

            // Power
            std::wstring pwrStr = FormatPower(p->current.powerWatts, p->current.powerDir, p->current.hasPower);
            D2D1_COLOR_F pwrCol = theme_.textMuted;
            if (p->current.hasPower) {
                pwrCol = (p->current.powerDir == PowerDirection::Accepted) ? theme_.seriesPowerAcc : theme_.seriesPowerDel;
            }
            DrawTextLine(pwrStr, D2D1::RectF(colPwr, ry, tableBox.right - 16, ry + kRowH),
                         fmtMono_, pwrCol, DWRITE_TEXT_ALIGNMENT_TRAILING);

            // Register clickable hit region (clipped to visible list area)
            D2D1_RECT_F hitR = D2D1::RectF(rowRect.left, std::max(rowRect.top, listTop),
                                           rowRect.right, std::min(rowRect.bottom, tableBox.bottom));
            if (hitR.bottom > hitR.top) {
                hitButtons_.push_back({hitR, kActOpenPort, p->id});
            }
        }
    }

    // Subtle Scrollbar
    if (maxScrollY_ > 0.0f && listH > 20.0f) {
        float thumbH = std::max(28.0f, listH * (listH / contentH));
        float thumbY = listTop + (scrollY_ / maxScrollY_) * (listH - thumbH);
        D2D1_RECT_F thumb = D2D1::RectF(tableBox.right - 8.0f, thumbY + 2.0f, tableBox.right - 3.0f, thumbY + thumbH - 2.0f);
        DrawRoundedRect(thumb, 2.5f, theme_.borderStrong, D2D1::ColorF(0, 0, 0, 0), 0.0f);
    }

    rt_->PopAxisAlignedClip();
}

void AppUI::DrawDetailPage(float width, float height) {
    constexpr float kPad = 20.0f;
    float y = 70.0f;

    // Back Button
    D2D1_RECT_F backBtn = D2D1::RectF(kPad, y, kPad + 168.0f, y + 34.0f);
    bool hoverBack = mouseInside_ && PtInRectF(backBtn, mouseX_, mouseY_);
    DrawRoundedRect(backBtn, 8.0f, hoverBack ? theme_.bgSurfaceHover : theme_.bgSurface, theme_.borderStrong, 1.0f);
    DrawTextLine(L"<- Back to All Ports", backBtn, fmtBold_, theme_.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    hitButtons_.push_back({backBtn, kActBackToMain, L""});

    const PortInfo* port = FindSelectedPort();
    if (!port) {
        DrawTextLine(L"Selected port is no longer present.",
                     D2D1::RectF(kPad, y + 60, width - kPad, y + 100), fmtHeading_, theme_.textSecondary);
        return;
    }

    y += 46.0f;

    // Port Summary Header Card
    float hdrH = 88.0f;
    D2D1_RECT_F hdrCard = D2D1::RectF(kPad, y, width - kPad, y + hdrH);
    DrawRoundedRect(hdrCard, 10.0f, theme_.bgSurface, theme_.border, 1.0f);

    DrawTextLine(port->name, D2D1::RectF(hdrCard.left + 16, hdrCard.top + 10, hdrCard.left + 540, hdrCard.top + 36),
                 fmtTitle_, theme_.textPrimary);

    std::wstring subLine = std::wstring(CategoryToString(port->category)) + L"  |  Device: " +
                           (port->connectedDevice.empty() ? L"N/A" : port->connectedDevice) +
                           L"  |  " + port->hardwareSpec;
    DrawTextLine(subLine, D2D1::RectF(hdrCard.left + 16, hdrCard.top + 38, hdrCard.right - 360, hdrCard.top + 60),
                 fmtBody_, theme_.textSecondary);

    // Status pill
    D2D1_RECT_F statPill = D2D1::RectF(hdrCard.left + 16, hdrCard.top + 62, hdrCard.left + 116, hdrCard.top + 82);
    DrawRoundedRect(statPill, 10.0f, port->active ? theme_.bgBadgeActive : theme_.bgBadgeUnused, D2D1::ColorF(0, 0, 0, 0), 0.0f);
    DrawTextLine(port->statusText, statPill, fmtSmall_, port->active ? theme_.statusActive : theme_.statusUnused,
                 DWRITE_TEXT_ALIGNMENT_CENTER);

    // Instant live metrics on the right side of the header card
    float rx = hdrCard.right - 340.0f;
    DrawTextLine(L"TO (OUT)", D2D1::RectF(rx, hdrCard.top + 14, rx + 105, hdrCard.top + 32), fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawTextLine(FormatBytesPerSec(port->current.bytesToSec, port->current.hasDataTo),
                 D2D1::RectF(rx, hdrCard.top + 34, rx + 105, hdrCard.top + 62), fmtBold_,
                 port->current.hasDataTo ? theme_.seriesTo : theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);

    DrawTextLine(L"FRO (IN)", D2D1::RectF(rx + 110, hdrCard.top + 14, rx + 215, hdrCard.top + 32), fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawTextLine(FormatBytesPerSec(port->current.bytesFroSec, port->current.hasDataFro),
                 D2D1::RectF(rx + 110, hdrCard.top + 34, rx + 215, hdrCard.top + 62), fmtBold_,
                 port->current.hasDataFro ? theme_.seriesFro : theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);

    DrawTextLine(L"POWER", D2D1::RectF(rx + 220, hdrCard.top + 14, rx + 324, hdrCard.top + 32), fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    D2D1_COLOR_F pwrCol = theme_.textMuted;
    if (port->current.hasPower) {
        pwrCol = (port->current.powerDir == PowerDirection::Accepted) ? theme_.seriesPowerAcc : theme_.seriesPowerDel;
    }
    DrawTextLine(FormatPower(port->current.powerWatts, port->current.powerDir, port->current.hasPower),
                 D2D1::RectF(rx + 220, hdrCard.top + 34, rx + 324, hdrCard.top + 62), fmtBold_,
                 pwrCol, DWRITE_TEXT_ALIGNMENT_TRAILING);

    y += hdrH + 16.0f;

    // Two Stacked Live Graphs: Data Transfer Speed vs Time, and Power vs Time
    float availH = std::max(260.0f, height - y - 16.0f);
    float gap    = 16.0f;
    float graphH = (availH - gap) * 0.5f;

    DrawDataGraph(D2D1::RectF(kPad, y, width - kPad, y + graphH), *port);
    DrawPowerGraph(D2D1::RectF(kPad, y + graphH + gap, width - kPad, y + graphH * 2.0f + gap), *port);
}

void AppUI::DrawDataGraph(const D2D1_RECT_F& card, const PortInfo& port) {
    DrawRoundedRect(card, 10.0f, theme_.bgSurface, theme_.border, 1.0f);

    DrawTextLine(L"Data Transfer Speed vs Time (To & Fro)",
                 D2D1::RectF(card.left + 16, card.top + 10, card.left + 380, card.top + 34),
                 fmtHeading_, theme_.textPrimary);

    if (!port.dataNote.empty()) {
        DrawTextLine(port.dataNote,
                     D2D1::RectF(card.left + 360, card.top + 12, card.right - 190, card.top + 34),
                     fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // Legend
    float lx = card.right - 175.0f;
    brush_->SetColor(theme_.seriesTo);
    rt_->FillRectangle(D2D1::RectF(lx, card.top + 18, lx + 12, card.top + 26), brush_);
    DrawTextLine(L"To (Out)", D2D1::RectF(lx + 16, card.top + 10, lx + 82, card.top + 34), fmtSmall_, theme_.textSecondary);

    brush_->SetColor(theme_.seriesFro);
    rt_->FillRectangle(D2D1::RectF(lx + 86, card.top + 18, lx + 98, card.top + 26), brush_);
    DrawTextLine(L"Fro (In)", D2D1::RectF(lx + 102, card.top + 10, lx + 168, card.top + 34), fmtSmall_, theme_.textSecondary);

    // Plot Area
    D2D1_RECT_F plot = D2D1::RectF(card.left + 78.0f, card.top + 42.0f, card.right - 20.0f, card.bottom - 28.0f);
    DrawRoundedRect(plot, 6.0f, theme_.bgChart, theme_.gridLine, 1.0f);

    bool anyData = false;
    double maxVal = 0.0;
    for (const auto& s : port.history) {
        if (s.hasDataTo)  { anyData = true; maxVal = std::max(maxVal, s.bytesToSec); }
        if (s.hasDataFro) { anyData = true; maxVal = std::max(maxVal, s.bytesFroSec); }
    }

    if (!anyData) {
        DrawTextLine(L"N/A — Port Unused or Data Transfer Telemetry Unavailable",
                     plot, fmtHeading_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    double yMax = NiceUpperBound(maxVal * 1.15, 1024.0);
    float plotW = plot.right - plot.left;
    float plotH = plot.bottom - plot.top;

    // Horizontal gridlines & Y-axis labels
    for (int i = 0; i <= 4; ++i) {
        float frac = static_cast<float>(i) / 4.0f;
        float gy = plot.bottom - frac * plotH;
        if (i > 0 && i < 4) {
            brush_->SetColor(theme_.gridLine);
            rt_->DrawLine(D2D1::Point2F(plot.left, gy), D2D1::Point2F(plot.right, gy), brush_, 1.0f);
        }
        DrawTextLine(FormatBytesPerSec(yMax * frac, true),
                     D2D1::RectF(card.left + 6.0f, gy - 9.0f, plot.left - 8.0f, gy + 9.0f),
                     fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // X-axis time ticks (-60s to Now)
    const wchar_t* xLabels[] = { L"-60s", L"-45s", L"-30s", L"-15s", L"Now" };
    for (int i = 0; i < 5; ++i) {
        float frac = static_cast<float>(i) / 4.0f;
        float gx = plot.left + frac * plotW;
        DWRITE_TEXT_ALIGNMENT al = (i == 0) ? DWRITE_TEXT_ALIGNMENT_LEADING :
                                   (i == 4) ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER;
        DrawTextLine(xLabels[i], D2D1::RectF(gx - 28.0f, plot.bottom + 4.0f, gx + 28.0f, card.bottom - 4.0f),
                     fmtSmall_, theme_.textMuted, al);
    }

    size_t n = port.history.size();
    auto xForIndex = [&](size_t idx) -> float {
        // Right-aligned timeline of kMaxHistorySamples points
        size_t offset = kMaxHistorySamples - n + idx;
        return plot.left + (static_cast<float>(offset) / static_cast<float>(kMaxHistorySamples - 1)) * plotW;
    };
    auto yForVal = [&](double v) -> float {
        double ratio = std::clamp(v / yMax, 0.0, 1.0);
        return plot.bottom - static_cast<float>(ratio) * plotH;
    };

    auto drawSeries = [&](bool isTo, const D2D1_COLOR_F& strokeCol, const D2D1_COLOR_F& fillCol) {
        if (n < 2) return;
        ID2D1PathGeometry* areaGeo = nullptr;
        ID2D1PathGeometry* lineGeo = nullptr;
        if (FAILED(d2dFactory_->CreatePathGeometry(&areaGeo)) || FAILED(d2dFactory_->CreatePathGeometry(&lineGeo))) {
            SafeRelease(areaGeo);
            SafeRelease(lineGeo);
            return;
        }
        ID2D1GeometrySink* areaSink = nullptr;
        ID2D1GeometrySink* lineSink = nullptr;
        areaGeo->Open(&areaSink);
        lineGeo->Open(&lineSink);

        bool inSeg = false;
        float segStartX = 0.0f;
        float lastX = 0.0f;

        for (size_t i = 0; i < n; ++i) {
            const auto& s = port.history[i];
            bool valid = isTo ? s.hasDataTo : s.hasDataFro;
            double val = isTo ? s.bytesToSec : s.bytesFroSec;
            float px = xForIndex(i);
            float py = yForVal(val);

            if (valid) {
                if (!inSeg) {
                    areaSink->BeginFigure(D2D1::Point2F(px, plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
                    areaSink->AddLine(D2D1::Point2F(px, py));
                    lineSink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_HOLLOW);
                    segStartX = px;
                    inSeg = true;
                } else {
                    areaSink->AddLine(D2D1::Point2F(px, py));
                    lineSink->AddLine(D2D1::Point2F(px, py));
                }
                lastX = px;
            } else if (inSeg) {
                areaSink->AddLine(D2D1::Point2F(lastX, plot.bottom));
                areaSink->EndFigure(D2D1_FIGURE_END_CLOSED);
                lineSink->EndFigure(D2D1_FIGURE_END_OPEN);
                inSeg = false;
            }
        }
        if (inSeg) {
            areaSink->AddLine(D2D1::Point2F(lastX, plot.bottom));
            areaSink->EndFigure(D2D1_FIGURE_END_CLOSED);
            lineSink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        (void)segStartX;

        areaSink->Close();
        lineSink->Close();
        SafeRelease(areaSink);
        SafeRelease(lineSink);

        brush_->SetColor(fillCol);
        rt_->FillGeometry(areaGeo, brush_);
        brush_->SetColor(strokeCol);
        rt_->DrawGeometry(lineGeo, brush_, 2.0f);

        SafeRelease(areaGeo);
        SafeRelease(lineGeo);
    };

    drawSeries(true,  theme_.seriesTo,  theme_.seriesToFill);
    drawSeries(false, theme_.seriesFro, theme_.seriesFroFill);

    // Interactive Hover Crosshair & Tooltip
    if (mouseInside_ && PtInRectF(plot, mouseX_, mouseY_) && n > 0) {
        float rel = std::clamp((mouseX_ - plot.left) / plotW, 0.0f, 1.0f);
        int slot  = static_cast<int>(std::round(rel * static_cast<float>(kMaxHistorySamples - 1)));
        int startSlot = static_cast<int>(kMaxHistorySamples - n);
        if (slot >= startSlot && slot < static_cast<int>(kMaxHistorySamples)) {
            size_t idx = static_cast<size_t>(slot - startSlot);
            const auto& s = port.history[idx];
            float cx = xForIndex(idx);

            brush_->SetColor(theme_.borderStrong);
            rt_->DrawLine(D2D1::Point2F(cx, plot.top), D2D1::Point2F(cx, plot.bottom), brush_, 1.0f);

            if (s.hasDataTo) {
                brush_->SetColor(theme_.seriesTo);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, yForVal(s.bytesToSec)), 4.0f, 4.0f), brush_);
            }
            if (s.hasDataFro) {
                brush_->SetColor(theme_.seriesFro);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, yForVal(s.bytesFroSec)), 4.0f, 4.0f), brush_);
            }

            int secsAgo = static_cast<int>(n - 1 - idx);
            std::wstring tipTime = (secsAgo == 0) ? L"Now" : (L"-" + std::to_wstring(secsAgo) + L"s");
            std::wstring tipTo   = L"To:  " + FormatBytesPerSec(s.bytesToSec, s.hasDataTo);
            std::wstring tipFro  = L"Fro: " + FormatBytesPerSec(s.bytesFroSec, s.hasDataFro);

            float tw = 152.0f, th = 62.0f;
            float tx = (cx + tw + 14.0f < plot.right) ? (cx + 10.0f) : (cx - tw - 10.0f);
            float ty = plot.top + 8.0f;
            D2D1_RECT_F tipR = D2D1::RectF(tx, ty, tx + tw, ty + th);
            DrawRoundedRect(tipR, 6.0f, theme_.tooltipBg, theme_.tooltipBorder, 1.0f);
            DrawTextLine(tipTime, D2D1::RectF(tx + 8, ty + 4,  tx + tw - 8, ty + 20), fmtSmall_, theme_.textMuted);
            DrawTextLine(tipTo,   D2D1::RectF(tx + 8, ty + 20, tx + tw - 8, ty + 38), fmtMono_,  theme_.seriesTo);
            DrawTextLine(tipFro,  D2D1::RectF(tx + 8, ty + 38, tx + tw - 8, ty + 56), fmtMono_,  theme_.seriesFro);
        }
    }
}

void AppUI::DrawPowerGraph(const D2D1_RECT_F& card, const PortInfo& port) {
    DrawRoundedRect(card, 10.0f, theme_.bgSurface, theme_.border, 1.0f);

    DrawTextLine(L"Power Delivered / Accepted vs Time",
                 D2D1::RectF(card.left + 16, card.top + 10, card.left + 340, card.top + 34),
                 fmtHeading_, theme_.textPrimary);

    if (!port.powerNote.empty()) {
        DrawTextLine(port.powerNote,
                     D2D1::RectF(card.left + 320, card.top + 12, card.right - 225, card.top + 34),
                     fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // Legend
    float lx = card.right - 210.0f;
    brush_->SetColor(theme_.seriesPowerDel);
    rt_->FillRectangle(D2D1::RectF(lx, card.top + 18, lx + 12, card.top + 26), brush_);
    DrawTextLine(L"Delivered", D2D1::RectF(lx + 16, card.top + 10, lx + 96, card.top + 34), fmtSmall_, theme_.textSecondary);

    brush_->SetColor(theme_.seriesPowerAcc);
    rt_->FillRectangle(D2D1::RectF(lx + 100, card.top + 18, lx + 112, card.top + 26), brush_);
    DrawTextLine(L"Accepted", D2D1::RectF(lx + 116, card.top + 10, lx + 200, card.top + 34), fmtSmall_, theme_.textSecondary);

    // Plot Area
    D2D1_RECT_F plot = D2D1::RectF(card.left + 78.0f, card.top + 42.0f, card.right - 20.0f, card.bottom - 28.0f);
    DrawRoundedRect(plot, 6.0f, theme_.bgChart, theme_.gridLine, 1.0f);

    bool anyPower = false;
    double maxW = 0.0;
    PowerDirection dominantDir = PowerDirection::Delivered;
    for (const auto& s : port.history) {
        if (s.hasPower && s.powerDir != PowerDirection::None) {
            anyPower = true;
            maxW = std::max(maxW, s.powerWatts);
            dominantDir = s.powerDir;
        }
    }

    if (!anyPower) {
        DrawTextLine(L"N/A — Port Unused or Power Telemetry Not Reported by Hardware",
                     plot, fmtHeading_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    double yMax = NiceUpperBound(maxW * 1.25, 1.0);
    float plotW = plot.right - plot.left;
    float plotH = plot.bottom - plot.top;

    for (int i = 0; i <= 4; ++i) {
        float frac = static_cast<float>(i) / 4.0f;
        float gy = plot.bottom - frac * plotH;
        if (i > 0 && i < 4) {
            brush_->SetColor(theme_.gridLine);
            rt_->DrawLine(D2D1::Point2F(plot.left, gy), D2D1::Point2F(plot.right, gy), brush_, 1.0f);
        }
        DrawTextLine(FormatPowerShort(yMax * frac, dominantDir, true),
                     D2D1::RectF(card.left + 6.0f, gy - 9.0f, plot.left - 8.0f, gy + 9.0f),
                     fmtSmall_, theme_.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    const wchar_t* xLabels[] = { L"-60s", L"-45s", L"-30s", L"-15s", L"Now" };
    for (int i = 0; i < 5; ++i) {
        float frac = static_cast<float>(i) / 4.0f;
        float gx = plot.left + frac * plotW;
        DWRITE_TEXT_ALIGNMENT al = (i == 0) ? DWRITE_TEXT_ALIGNMENT_LEADING :
                                   (i == 4) ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER;
        DrawTextLine(xLabels[i], D2D1::RectF(gx - 28.0f, plot.bottom + 4.0f, gx + 28.0f, card.bottom - 4.0f),
                     fmtSmall_, theme_.textMuted, al);
    }

    size_t n = port.history.size();
    auto xForIndex = [&](size_t idx) -> float {
        size_t offset = kMaxHistorySamples - n + idx;
        return plot.left + (static_cast<float>(offset) / static_cast<float>(kMaxHistorySamples - 1)) * plotW;
    };
    auto yForVal = [&](double v) -> float {
        double ratio = std::clamp(v / yMax, 0.0, 1.0);
        return plot.bottom - static_cast<float>(ratio) * plotH;
    };

    D2D1_COLOR_F strokeCol = (dominantDir == PowerDirection::Accepted) ? theme_.seriesPowerAcc     : theme_.seriesPowerDel;
    D2D1_COLOR_F fillCol   = (dominantDir == PowerDirection::Accepted) ? theme_.seriesPowerAccFill : theme_.seriesPowerDelFill;

    if (n >= 2) {
        ID2D1PathGeometry* areaGeo = nullptr;
        ID2D1PathGeometry* lineGeo = nullptr;
        if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&areaGeo)) && SUCCEEDED(d2dFactory_->CreatePathGeometry(&lineGeo))) {
            ID2D1GeometrySink* areaSink = nullptr;
            ID2D1GeometrySink* lineSink = nullptr;
            areaGeo->Open(&areaSink);
            lineGeo->Open(&lineSink);

            bool inSeg = false;
            float lastX = 0.0f;
            for (size_t i = 0; i < n; ++i) {
                const auto& s = port.history[i];
                bool valid = s.hasPower && s.powerDir != PowerDirection::None;
                float px = xForIndex(i);
                float py = yForVal(s.powerWatts);

                if (valid) {
                    if (!inSeg) {
                        areaSink->BeginFigure(D2D1::Point2F(px, plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
                        areaSink->AddLine(D2D1::Point2F(px, py));
                        lineSink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_HOLLOW);
                        inSeg = true;
                    } else {
                        areaSink->AddLine(D2D1::Point2F(px, py));
                        lineSink->AddLine(D2D1::Point2F(px, py));
                    }
                    lastX = px;
                } else if (inSeg) {
                    areaSink->AddLine(D2D1::Point2F(lastX, plot.bottom));
                    areaSink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    lineSink->EndFigure(D2D1_FIGURE_END_OPEN);
                    inSeg = false;
                }
            }
            if (inSeg) {
                areaSink->AddLine(D2D1::Point2F(lastX, plot.bottom));
                areaSink->EndFigure(D2D1_FIGURE_END_CLOSED);
                lineSink->EndFigure(D2D1_FIGURE_END_OPEN);
            }
            areaSink->Close();
            lineSink->Close();
            SafeRelease(areaSink);
            SafeRelease(lineSink);

            brush_->SetColor(fillCol);
            rt_->FillGeometry(areaGeo, brush_);
            brush_->SetColor(strokeCol);
            rt_->DrawGeometry(lineGeo, brush_, 2.2f);
        }
        SafeRelease(areaGeo);
        SafeRelease(lineGeo);
    }

    // Hover Crosshair & Tooltip
    if (mouseInside_ && PtInRectF(plot, mouseX_, mouseY_) && n > 0) {
        float rel = std::clamp((mouseX_ - plot.left) / plotW, 0.0f, 1.0f);
        int slot  = static_cast<int>(std::round(rel * static_cast<float>(kMaxHistorySamples - 1)));
        int startSlot = static_cast<int>(kMaxHistorySamples - n);
        if (slot >= startSlot && slot < static_cast<int>(kMaxHistorySamples)) {
            size_t idx = static_cast<size_t>(slot - startSlot);
            const auto& s = port.history[idx];
            float cx = xForIndex(idx);

            brush_->SetColor(theme_.borderStrong);
            rt_->DrawLine(D2D1::Point2F(cx, plot.top), D2D1::Point2F(cx, plot.bottom), brush_, 1.0f);

            if (s.hasPower && s.powerDir != PowerDirection::None) {
                D2D1_COLOR_F dotCol = (s.powerDir == PowerDirection::Accepted) ? theme_.seriesPowerAcc : theme_.seriesPowerDel;
                brush_->SetColor(dotCol);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, yForVal(s.powerWatts)), 4.0f, 4.0f), brush_);
            }

            int secsAgo = static_cast<int>(n - 1 - idx);
            std::wstring tipTime = (secsAgo == 0) ? L"Now" : (L"-" + std::to_wstring(secsAgo) + L"s");
            std::wstring tipVal  = FormatPower(s.powerWatts, s.powerDir, s.hasPower);
            std::wstring tipDir  = (!s.hasPower || s.powerDir == PowerDirection::None) ? L"Status: N/A" :
                                   (s.powerDir == PowerDirection::Accepted ? L"Direction: Accepted" : L"Direction: Delivered");

            float tw = 164.0f, th = 62.0f;
            float tx = (cx + tw + 14.0f < plot.right) ? (cx + 10.0f) : (cx - tw - 10.0f);
            float ty = plot.top + 8.0f;
            D2D1_RECT_F tipR = D2D1::RectF(tx, ty, tx + tw, ty + th);
            DrawRoundedRect(tipR, 6.0f, theme_.tooltipBg, theme_.tooltipBorder, 1.0f);
            DrawTextLine(tipTime, D2D1::RectF(tx + 8, ty + 4,  tx + tw - 8, ty + 20), fmtSmall_, theme_.textMuted);
            DrawTextLine(tipVal,  D2D1::RectF(tx + 8, ty + 20, tx + tw - 8, ty + 38), fmtMono_,  strokeCol);
            DrawTextLine(tipDir,  D2D1::RectF(tx + 8, ty + 38, tx + tw - 8, ty + 56), fmtSmall_, theme_.textSecondary);
        }
    }
}

} // namespace porttool
