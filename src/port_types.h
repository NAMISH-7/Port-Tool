#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cmath>

namespace porttool {

constexpr size_t kMaxHistorySamples = 60; // 60 seconds at 1 Hz

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
    uint64_t timestampSec = 0;  // Monotonic seconds
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
    std::wstring name;             // Display name (e.g. "USB Port 1 (Hub 1)", "Ethernet", "USB-C / AC Power")
    PortCategory category = PortCategory::USB;
    bool         active   = false; // True if connected / in-use

    std::wstring connectedDevice;  // Connected device name or "N/A"
    std::wstring hardwareSpec;     // e.g. "USB 3.0 SuperSpeed (5 Gbps)", "1 Gbps Full Duplex", "1920x1080 @ 60Hz"
    std::wstring statusText;       // e.g. "Active", "Unused", "Charging", "Discharging", "Disconnected"
    std::wstring powerNote;        // Optional note like "Negotiated USB Budget (5V)" or "Live Battery Sensor"
    std::wstring dataNote;         // Optional note like "Live NIC Counter" or "Estimated Video Stream"

    // Latest instant values
    PortSample current{};

    // Rolling history (oldest -> newest, max kMaxHistorySamples)
    std::vector<PortSample> history;
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

inline std::wstring FormatPowerShort(double watts, PowerDirection dir, bool available) {
    if (!available || dir == PowerDirection::None) return L"N/A";
    wchar_t buf[64];
    if (watts < 1.0 && watts > 0.0) {
        swprintf_s(buf, L"%.0f mW", watts * 1000.0);
    } else {
        swprintf_s(buf, L"%.2f W", watts);
    }
    return buf;
}

} // namespace porttool
