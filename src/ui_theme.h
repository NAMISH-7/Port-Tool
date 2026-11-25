#pragma once

#include <d2d1.h>

namespace porttool {

struct ThemePalette {
    bool isDark = true;

    D2D1_COLOR_F bgWindow;
    D2D1_COLOR_F bgSurface;
    D2D1_COLOR_F bgSurfaceHover;
    D2D1_COLOR_F bgHeader;
    D2D1_COLOR_F bgChart;
    D2D1_COLOR_F bgBadgeActive;
    D2D1_COLOR_F bgBadgeUnused;
    D2D1_COLOR_F bgPillActive;
    D2D1_COLOR_F bgPillIdle;

    D2D1_COLOR_F border;
    D2D1_COLOR_F borderStrong;
    D2D1_COLOR_F gridLine;

    D2D1_COLOR_F textPrimary;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F textMuted;
    D2D1_COLOR_F textAccent;
    D2D1_COLOR_F textOnAccent;

    D2D1_COLOR_F statusActive;
    D2D1_COLOR_F statusUnused;

    // Graph series colors
    D2D1_COLOR_F seriesTo;           // Outgoing / TX
    D2D1_COLOR_F seriesToFill;
    D2D1_COLOR_F seriesFro;          // Incoming / RX
    D2D1_COLOR_F seriesFroFill;
    D2D1_COLOR_F seriesPowerDel;     // Power Delivered (Out)
    D2D1_COLOR_F seriesPowerDelFill;
    D2D1_COLOR_F seriesPowerAcc;     // Power Accepted (In / Charging)
    D2D1_COLOR_F seriesPowerAccFill;

    D2D1_COLOR_F tooltipBg;
    D2D1_COLOR_F tooltipBorder;
};

inline D2D1_COLOR_F Rgb(uint32_t hex, float a = 1.0f) {
    return D2D1::ColorF(
        ((hex >> 16) & 0xFF) / 255.0f,
        ((hex >> 8)  & 0xFF) / 255.0f,
        ( hex        & 0xFF) / 255.0f,
        a
    );
}

inline ThemePalette MakeDarkTheme() {
    ThemePalette t{};
    t.isDark = true;
    t.bgWindow           = Rgb(0x0B0F19);
    t.bgSurface          = Rgb(0x131A2A);
    t.bgSurfaceHover     = Rgb(0x1B253B);
    t.bgHeader           = Rgb(0x0F1524);
    t.bgChart            = Rgb(0x0E1422);
    t.bgBadgeActive      = Rgb(0x10B981, 0.16f);
    t.bgBadgeUnused      = Rgb(0x64748B, 0.16f);
    t.bgPillActive       = Rgb(0x3B82F6);
    t.bgPillIdle         = Rgb(0x192236);

    t.border             = Rgb(0x23304A);
    t.borderStrong       = Rgb(0x33466B);
    t.gridLine           = Rgb(0x1E293B);

    t.textPrimary        = Rgb(0xF1F5F9);
    t.textSecondary      = Rgb(0x94A3B8);
    t.textMuted          = Rgb(0x64748B);
    t.textAccent         = Rgb(0x60A5FA);
    t.textOnAccent       = Rgb(0xFFFFFF);

    t.statusActive       = Rgb(0x34D399);
    t.statusUnused       = Rgb(0x94A3B8);

    t.seriesTo           = Rgb(0x38BDF8); // Sky Blue
    t.seriesToFill       = Rgb(0x38BDF8, 0.18f);
    t.seriesFro          = Rgb(0x34D399); // Emerald
    t.seriesFroFill      = Rgb(0x34D399, 0.18f);
    t.seriesPowerDel     = Rgb(0xFB923C); // Orange
    t.seriesPowerDelFill = Rgb(0xFB923C, 0.20f);
    t.seriesPowerAcc     = Rgb(0xA78BFA); // Violet
    t.seriesPowerAccFill = Rgb(0xA78BFA, 0.20f);

    t.tooltipBg          = Rgb(0x0F172A, 0.95f);
    t.tooltipBorder      = Rgb(0x3B82F6, 0.70f);
    return t;
}

inline ThemePalette MakeLightTheme() {
    ThemePalette t{};
    t.isDark = false;
    t.bgWindow           = Rgb(0xF4F6FA);
    t.bgSurface          = Rgb(0xFFFFFF);
    t.bgSurfaceHover     = Rgb(0xEEF2F9);
    t.bgHeader           = Rgb(0xFFFFFF);
    t.bgChart            = Rgb(0xF8FAFC);
    t.bgBadgeActive      = Rgb(0x10B981, 0.14f);
    t.bgBadgeUnused      = Rgb(0x64748B, 0.12f);
    t.bgPillActive       = Rgb(0x2563EB);
    t.bgPillIdle         = Rgb(0xE8EEF6);

    t.border             = Rgb(0xDCE3EE);
    t.borderStrong       = Rgb(0xCBD5E1);
    t.gridLine           = Rgb(0xE2E8F0);

    t.textPrimary        = Rgb(0x0F172A);
    t.textSecondary      = Rgb(0x475569);
    t.textMuted          = Rgb(0x64748B);
    t.textAccent         = Rgb(0x2563EB);
    t.textOnAccent       = Rgb(0xFFFFFF);

    t.statusActive       = Rgb(0x059669);
    t.statusUnused       = Rgb(0x64748B);

    t.seriesTo           = Rgb(0x0284C7); // Deep Sky
    t.seriesToFill       = Rgb(0x0284C7, 0.15f);
    t.seriesFro          = Rgb(0x059669); // Deep Emerald
    t.seriesFroFill      = Rgb(0x059669, 0.15f);
    t.seriesPowerDel     = Rgb(0xEA580C); // Deep Orange
    t.seriesPowerDelFill = Rgb(0xEA580C, 0.16f);
    t.seriesPowerAcc     = Rgb(0x7C3AED); // Deep Violet
    t.seriesPowerAccFill = Rgb(0x7C3AED, 0.16f);

    t.tooltipBg          = Rgb(0xFFFFFF, 0.96f);
    t.tooltipBorder      = Rgb(0x2563EB, 0.65f);
    return t;
}

} // namespace porttool
