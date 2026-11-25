#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cmath>

namespace porttool {

constexpr uint32_t kSampleIntervalMs  = 500; // 0.5 second sampling frequency
constexpr size_t   kMaxHistorySamples = 120; // 60 seconds at 0.5s intervals

enum class PortCategory : uint8_t {
    USB = 0,
    Network,
    Power,
    Display,
    Audio,
    Serial
};

enum class PowerDirection : uint8_t {
    None = 0,      // N/A (unused or not reported by hardware)
    Delivered,     // Power delivered by this port/system (Out)
    Accepted       // Power accepted into this port/system (In / Charging)
};

struct PortSample {
    uint64_t timestampMs  = 0;  // Monotonic ms
    bool     hasDataTo    = false;
    bool     hasDataFro   = false;
    double   bytesToSec   = 0.0; // Outgoing (To device / TX) in Bytes/sec
    double   bytesFroSec  = 0.0; // Incoming (Fro device / RX) in Bytes/sec

    bool           hasPower   = false;
    double         powerWatts = 0.0; // Always >= 0 magnitude; direction indicates Delivered vs Accepted
    PowerDirection powerDir   = PowerDirection::None;
};

struct PortInfo {
    std::wstring id;               // Stable unique identifier across polls
    std::wstring name;             // Display name
    PortCategory category = PortCategory::USB;
    bool         active   = false; // True if connected / in-use

    std::wstring connectedDevice;  // Connected device name or "N/A"
    std::wstring hardwareSpec;     // Link speed, resolution, etc.
    std::wstring statusText;       // "Active", "Unused", "Charging", etc.
    std::wstring powerNote;
    std::wstring dataNote;

    PortSample current{};
    std::vector<PortSample> history;
};

// ============================================================================
// SYSTEM HARDWARE TAB TYPES (CPU, GPU(s), Disk(s), Memory)
// ============================================================================

enum class SysCompType : uint8_t {
    CPU = 0,
    GPU,
    Disk,
    Memory
};

struct KeyValueStat {
    std::wstring label;
    std::wstring value;
};

struct SysCompSample {
    uint64_t timestampMs = 0;

    // Utilization (0..100%)
    bool   hasUtil       = false;
    double utilPct       = 0.0;
    bool   hasUtil2      = false; // Secondary series (e.g. Kernel %, VRAM %, Write Active)
    double util2Pct      = 0.0;

    // Data / Throughput (Bytes/sec — e.g. Disk Write/Read, or Memory In-Use/Committed in bytes)
    bool   hasDataTo     = false; // Write / Out / Committed
    bool   hasDataFro    = false; // Read  / In  / In-Use
    double dataToVal     = 0.0;
    double dataFroVal    = 0.0;

    // Power (Watts)
    bool   hasPower      = false;
    double powerWatts    = 0.0;

    // Temperature (Celsius)
    bool   hasTemp       = false;
    double tempC         = 0.0;
};

struct SysCompInfo {
    std::wstring id;            // Stable ID ("sys:cpu", "sys:gpu:0", "sys:disk:0", "sys:mem")
    std::wstring name;          // Display name (e.g. "AMD Ryzen 7 ...", "Disk 0 (NVMe ...)")
    std::wstring shortSubtitle; // e.g. "8 Cores / 16 Threads", "NVMe SSD 512 GB", "16.0 GB RAM"
    SysCompType  type = SysCompType::CPU;

    // Summary strings for the System Main Page table
    std::wstring utilSummary;   // e.g. "12.4%"
    std::wstring dataSummary;   // e.g. "3.42 GHz" / "1.2 / 8.0 GB VRAM" / "R: 12 MB/s W: 4 MB/s" / "9.4 / 16.0 GB"
    std::wstring powerSummary;  // e.g. "5.42 W" or "N/A"
    std::wstring tempSummary;   // e.g. "42.0 C" or "N/A"

    // Graph labels & modes
    std::wstring utilTitle;
    std::wstring utilSeries1Name;
    std::wstring utilSeries2Name;
    std::wstring dataTitle;
    std::wstring dataSeries1Name;
    std::wstring dataSeries2Name;
    bool         dataIsBytesNotRate = false; // true for Memory/VRAM GB, false for B/s rate
    std::wstring powerNote;
    std::wstring tempNote;

    // Detailed Task-Manager-style key/value specs
    std::vector<KeyValueStat> details;

    SysCompSample current{};
    std::vector<SysCompSample> history;
};

inline const wchar_t* CategoryToString(PortCategory cat) {
    switch (cat) {
        case PortCategory::USB:     return L"USB";
        case PortCategory::Network: return L"Network";
        case PortCategory::Power:   return L"Power";
        case PortCategory::Display: return L"Display";
        case PortCategory::Audio:   return L"Audio";
        case PortCategory::Serial:  return L"Serial";
    }
    return L"Other";
}

inline const wchar_t* SysCompTypeToString(SysCompType t) {
    switch (t) {
        case SysCompType::CPU:    return L"Processor";
        case SysCompType::GPU:    return L"GPU";
        case SysCompType::Disk:   return L"Disk";
        case SysCompType::Memory: return L"Memory";
    }
    return L"Hardware";
}

inline std::wstring FormatBytesPerSec(double bps, bool available) {
    if (!available) return L"N/A";
    wchar_t buf[64];
    if (bps < 0.0) bps = 0.0;
    if (bps < 1024.0) {
        swprintf_s(buf, L"%.0f B/s", bps);
    } else if (bps < 1024.0 * 1024.0) {
        swprintf_s(buf, L"%.1f KB/s", bps / 1024.0);
    } else if (bps < 1024.0 * 1024.0 * 1024.0) {
        swprintf_s(buf, L"%.2f MB/s", bps / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%.2f GB/s", bps / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

inline std::wstring FormatBytesSize(double bytes, bool available = true) {
    if (!available) return L"N/A";
    wchar_t buf[64];
    if (bytes < 0.0) bytes = 0.0;
    if (bytes < 1024.0) {
        swprintf_s(buf, L"%.0f B", bytes);
    } else if (bytes < 1024.0 * 1024.0) {
        swprintf_s(buf, L"%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024.0 * 1024.0 * 1024.0) {
        swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes < 1024.0 * 1024.0 * 1024.0 * 1024.0) {
        swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%.2f TB", bytes / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

inline std::wstring FormatPower(double watts, PowerDirection dir, bool available) {
    if (!available || dir == PowerDirection::None) return L"N/A";
    wchar_t buf[64];
    const wchar_t* tag = (dir == PowerDirection::Accepted) ? L"In" : L"Out";
    if (watts < 1.0 && watts > 0.0) {
        swprintf_s(buf, L"%.0f mW (%s)", watts * 1000.0, tag);
    } else {
        swprintf_s(buf, L"%.2f W (%s)", watts, tag);
    }
    return buf;
}

inline std::wstring FormatPowerPlain(double watts, bool available) {
    if (!available) return L"N/A";
    wchar_t buf[64];
    if (watts < 1.0 && watts > 0.0) {
        swprintf_s(buf, L"%.0f mW", watts * 1000.0);
    } else {
        swprintf_s(buf, L"%.2f W", watts);
    }
    return buf;
}

inline std::wstring FormatPowerShort(double watts, PowerDirection dir, bool available) {
    if (!available || dir == PowerDirection::None) return L"N/A";
    return FormatPowerPlain(watts, true);
}

inline std::wstring FormatTempC(double tempC, bool available) {
    if (!available) return L"N/A";
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f \u00B0C", tempC);
    return buf;
}

inline std::wstring FormatPercent(double pct, bool available) {
    if (!available) return L"N/A";
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f%%", pct);
    return buf;
}

} // namespace porttool
