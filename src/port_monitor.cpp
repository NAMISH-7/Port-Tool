#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <usbioctl.h>
#include <usbiodef.h>
#include <usb100.h>
#include <batclass.h>
#include <powrprof.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include "port_monitor.h"

#include <algorithm>
#include <cwctype>
#include <cmath>
#include <cstring>

namespace porttool {

namespace {

std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return r;
}

std::wstring Trim(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

std::wstring AnsiToWide(const char* src, size_t maxLen = 256) {
    if (!src || !src[0]) return L"";
    size_t len = strnlen(src, maxLen);
    if (len == 0) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, src, static_cast<int>(len), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, src, static_cast<int>(len), out.data(), needed);
    return Trim(out);
}

// Read-only USB string descriptor query
std::wstring QueryUsbStringDescriptor(HANDLE hHub, ULONG portIndex, UCHAR stringIndex) {
    if (stringIndex == 0 || hHub == INVALID_HANDLE_VALUE) return L"";

    constexpr size_t kBufSize = sizeof(USB_DESCRIPTOR_REQUEST) + MAXIMUM_USB_STRING_LENGTH;
    std::vector<uint8_t> buf(kBufSize, 0);
    auto* req = reinterpret_cast<PUSB_DESCRIPTOR_REQUEST>(buf.data());
    req->ConnectionIndex = portIndex;
    req->SetupPacket.wValue = static_cast<USHORT>((USB_STRING_DESCRIPTOR_TYPE << 8) | stringIndex);
    req->SetupPacket.wIndex = 0x0409; // English (US)
    req->SetupPacket.wLength = MAXIMUM_USB_STRING_LENGTH;

    DWORD bytesReturned = 0;
    if (!DeviceIoControl(
            hHub,
            IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
            buf.data(), static_cast<DWORD>(buf.size()),
            buf.data(), static_cast<DWORD>(buf.size()),
            &bytesReturned, nullptr)) {
        return L"";
    }

    auto* desc = reinterpret_cast<PUSB_STRING_DESCRIPTOR>(req->Data);
    if (desc->bLength <= 2 || desc->bDescriptorType != USB_STRING_DESCRIPTOR_TYPE) {
        return L"";
    }
    size_t charCount = (desc->bLength - 2) / sizeof(wchar_t);
    if (charCount == 0 || charCount > 126) return L"";
    return Trim(std::wstring(desc->bString, charCount));
}

// Read-only USB configuration descriptor MaxPower (in Watts at 5V)
bool QueryUsbConfigPowerWatts(HANDLE hHub, ULONG portIndex, UCHAR speed, double& outWatts, uint32_t& outMilliamps) {
    outWatts = 0.0;
    outMilliamps = 0;
    if (hHub == INVALID_HANDLE_VALUE) return false;

    constexpr size_t kBufSize = sizeof(USB_DESCRIPTOR_REQUEST) + sizeof(USB_CONFIGURATION_DESCRIPTOR) + 64;
    std::vector<uint8_t> buf(kBufSize, 0);
    auto* req = reinterpret_cast<PUSB_DESCRIPTOR_REQUEST>(buf.data());
    req->ConnectionIndex = portIndex;
    req->SetupPacket.wValue = static_cast<USHORT>((USB_CONFIGURATION_DESCRIPTOR_TYPE << 8) | 0);
    req->SetupPacket.wIndex = 0;
    req->SetupPacket.wLength = sizeof(USB_CONFIGURATION_DESCRIPTOR) + 64;

    DWORD bytesReturned = 0;
    if (!DeviceIoControl(
            hHub,
            IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
            buf.data(), static_cast<DWORD>(buf.size()),
            buf.data(), static_cast<DWORD>(buf.size()),
            &bytesReturned, nullptr)) {
        return false;
    }

    auto* cfg = reinterpret_cast<PUSB_CONFIGURATION_DESCRIPTOR>(req->Data);
    if (cfg->bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE || cfg->bLength < sizeof(USB_CONFIGURATION_DESCRIPTOR)) {
        return false;
    }

    // USB 3.0+ SuperSpeed uses 8mA units; USB 1.x/2.0 uses 2mA units.
    uint32_t unitMa = (speed >= 3) ? 8u : 2u;
    outMilliamps = static_cast<uint32_t>(cfg->MaxPower) * unitMa;
    if (outMilliamps == 0) {
        // Self-powered or unadvertised bus draw; still valid 0mA bus draw if connected
        outWatts = 0.0;
        return false;
    }
    outWatts = (static_cast<double>(outMilliamps) * 5.0) / 1000.0; // 5V VBUS
    return true;
}

const wchar_t* UsbSpeedToString(UCHAR speed) {
    switch (speed) {
        case 0: return L"USB 1.1 Low Speed (1.5 Mbps)";
        case 1: return L"USB 1.1 Full Speed (12 Mbps)";
        case 2: return L"USB 2.0 High Speed (480 Mbps)";
        case 3: return L"USB 3.x SuperSpeed (5+ Gbps)";
        default: return L"USB Connected";
    }
}

const wchar_t* DisplayTechToName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech) {
    switch (tech) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:                   return L"HDMI Port";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL:   return L"DisplayPort (External)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:   return L"Internal Display (eDP)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:                    return L"DVI Port";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15:                   return L"VGA Port (HD15)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:                   return L"Internal Display (LVDS)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL:           return L"UDI Display Port";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDI:                    return L"SDI Video Port";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST:               return L"Wireless Display (Miracast)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:               return L"Internal Display";
        default:                                                     return L"Display Output";
    }
}

std::wstring FormatLinkSpeed(uint64_t bitsPerSec) {
    if (bitsPerSec == 0) return L"Unknown link";
    wchar_t buf[64];
    if (bitsPerSec >= 1000000000ULL) {
        swprintf_s(buf, L"%.1f Gbps", static_cast<double>(bitsPerSec) / 1e9);
    } else if (bitsPerSec >= 1000000ULL) {
        swprintf_s(buf, L"%.0f Mbps", static_cast<double>(bitsPerSec) / 1e6);
    } else if (bitsPerSec >= 1000ULL) {
        swprintf_s(buf, L"%.0f Kbps", static_cast<double>(bitsPerSec) / 1e3);
    } else {
        swprintf_s(buf, L"%llu bps", bitsPerSec);
    }
    return buf;
}

} // namespace

PortMonitor::PortMonitor() {
    startTickMs_ = GetTickCount64();
}

PortMonitor::~PortMonitor() = default;

void PortMonitor::PollOnce() {
    uint64_t nowMs  = GetTickCount64();
    uint64_t nowSec = (nowMs - startTickMs_) / 1000ULL;

    std::vector<PortInfo> polled;
    polled.reserve(64);

    PollPowerPorts(polled, nowSec);
    PollUsbPorts(polled, nowSec, nowMs);
    PollNetworkPorts(polled, nowSec, nowMs);
    PollDisplayPorts(polled, nowSec);
    PollAudioPorts(polled, nowSec);
    PollSerialPorts(polled, nowSec);

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::wstring> newOrder;
    newOrder.reserve(polled.size());

    for (auto& p : polled) {
        newOrder.push_back(p.id);
        auto it = ports_.find(p.id);
        if (it == ports_.end()) {
            p.history.push_back(p.current);
            ports_.emplace(p.id, std::move(p));
        } else {
            auto& existing = it->second;
            existing.name            = p.name;
            existing.category        = p.category;
            existing.active          = p.active;
            existing.connectedDevice = p.connectedDevice;
            existing.hardwareSpec    = p.hardwareSpec;
            existing.statusText      = p.statusText;
            existing.powerNote       = p.powerNote;
            existing.dataNote        = p.dataNote;
            existing.current         = p.current;
            existing.history.push_back(p.current);
            if (existing.history.size() > kMaxHistorySamples) {
                existing.history.erase(existing.history.begin(),
                                       existing.history.begin() + (existing.history.size() - kMaxHistorySamples));
            }
        }
    }

    portOrder_ = std::move(newOrder);
}

std::vector<PortInfo> PortMonitor::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PortInfo> result;
    result.reserve(portOrder_.size());
    for (const auto& id : portOrder_) {
        auto it = ports_.find(id);
        if (it != ports_.end()) {
            result.push_back(it->second);
        }
    }
    return result;
}

bool PortMonitor::GetPortById(const std::wstring& id, PortInfo& outPort) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ports_.find(id);
    if (it == ports_.end()) return false;
    outPort = it->second;
    return true;
}

// ============================================================================
// 1. POWER / CHARGING PORTS (Read-Only Battery IOCTL & SystemBatteryState)
// ============================================================================
void PortMonitor::PollPowerPorts(std::vector<PortInfo>& out, uint64_t nowSec) {
    bool foundBattery = false;

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_BATTERY, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (hDevInfo != INVALID_HANDLE_VALUE) {
        for (DWORD idx = 0; idx < 8; ++idx) {
            SP_DEVICE_INTERFACE_DATA did{};
            did.cbSize = sizeof(did);
            if (!SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &GUID_DEVCLASS_BATTERY, idx, &did)) {
                break;
            }

            DWORD reqSize = 0;
            SetupDiGetDeviceInterfaceDetailW(hDevInfo, &did, nullptr, 0, &reqSize, nullptr);
            if (reqSize == 0) continue;

            std::vector<uint8_t> detailBuf(reqSize, 0);
            auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &did, detail, reqSize, nullptr, nullptr)) {
                continue;
            }

            // Strictly READ-ONLY handle
            HANDLE hBat = CreateFileW(
                detail->DevicePath,
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (hBat == INVALID_HANDLE_VALUE) continue;

            ULONG batteryTag = 0;
            DWORD wait = 0;
            DWORD bytesRet = 0;
            if (DeviceIoControl(hBat, IOCTL_BATTERY_QUERY_TAG,
                                &wait, sizeof(wait),
                                &batteryTag, sizeof(batteryTag),
                                &bytesRet, nullptr) && batteryTag != 0) {
                BATTERY_WAIT_STATUS bws{};
                bws.BatteryTag = batteryTag;
                BATTERY_STATUS bs{};
                if (DeviceIoControl(hBat, IOCTL_BATTERY_QUERY_STATUS,
                                    &bws, sizeof(bws),
                                    &bs, sizeof(bs),
                                    &bytesRet, nullptr)) {
                    BATTERY_QUERY_INFORMATION bqi{};
                    bqi.BatteryTag = batteryTag;
                    bqi.InformationLevel = BatteryInformation;
                    BATTERY_INFORMATION bi{};
                    bool hasInfo = DeviceIoControl(hBat, IOCTL_BATTERY_QUERY_INFORMATION,
                                                   &bqi, sizeof(bqi),
                                                   &bi, sizeof(bi),
                                                   &bytesRet, nullptr) != FALSE;

                    foundBattery = true;

                    PortInfo p{};
                    wchar_t idBuf[64];
                    swprintf_s(idBuf, L"power:battery:%lu", idx);
                    p.id = idBuf;
                    p.name = (idx == 0) ? L"Power / Charging Port (AC & Battery)" : (L"Power / Battery #" + std::to_wstring(idx + 1));
                    p.category = PortCategory::Power;
                    p.active = true;

                    bool onAc        = (bs.PowerState & BATTERY_POWER_ON_LINE) != 0;
                    bool charging    = (bs.PowerState & BATTERY_CHARGING) != 0;
                    bool discharging = (bs.PowerState & BATTERY_DISCHARGING) != 0;

                    p.connectedDevice = onAc ? L"AC / USB-C Power Adapter" : L"Internal Battery";

                    int pct = -1;
                    if (hasInfo && bi.FullChargedCapacity > 0 && bs.Capacity != BATTERY_UNKNOWN_CAPACITY) {
                        pct = static_cast<int>(std::min(100.0, (static_cast<double>(bs.Capacity) * 100.0) / bi.FullChargedCapacity));
                    }

                    wchar_t specBuf[128];
                    double volts = (bs.Voltage != BATTERY_UNKNOWN_VOLTAGE && bs.Voltage > 0) ? (bs.Voltage / 1000.0) : 0.0;
                    if (pct >= 0 && volts > 0.0) {
                        swprintf_s(specBuf, L"Battery %d%% | %.2f V", pct, volts);
                    } else if (pct >= 0) {
                        swprintf_s(specBuf, L"Battery %d%%", pct);
                    } else if (volts > 0.0) {
                        swprintf_s(specBuf, L"%.2f V", volts);
                    } else {
                        swprintf_s(specBuf, L"System Power Telemetry");
                    }
                    p.hardwareSpec = specBuf;

                    if (charging)         p.statusText = L"Charging";
                    else if (discharging) p.statusText = L"Discharging";
                    else if (onAc)        p.statusText = L"AC Online";
                    else                  p.statusText = L"Active";

                    p.current.timestampSec = nowSec;
                    p.current.hasDataTo    = false;
                    p.current.hasDataFro   = false;
                    p.dataNote = L"Power-only port";

                    if (bs.Rate != static_cast<LONG>(BATTERY_UNKNOWN_RATE)) {
                        double watts = std::fabs(static_cast<double>(bs.Rate)) / 1000.0;
                        p.current.hasPower = true;
                        p.current.powerWatts = watts;
                        if (charging || bs.Rate > 0) {
                            p.current.powerDir = PowerDirection::Accepted;
                            p.powerNote = L"Live Power Accepted (Charging)";
                        } else if (discharging || bs.Rate < 0) {
                            p.current.powerDir = PowerDirection::Delivered;
                            p.powerNote = L"Live Power Delivered (System Draw)";
                        } else {
                            p.current.powerDir = onAc ? PowerDirection::Accepted : PowerDirection::Delivered;
                            p.powerNote = L"Idle / Trickle";
                        }
                    } else {
                        p.current.hasPower = false;
                        p.current.powerDir = PowerDirection::None;
                        p.powerNote = L"Wattage sensor not reported";
                    }

                    out.push_back(std::move(p));
                }
            }
            CloseHandle(hBat);
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    if (!foundBattery) {
        // Desktop PC or fallback via SystemPowerStatus / CallNtPowerInformation
        SYSTEM_POWER_STATUS sps{};
        SYSTEM_BATTERY_STATE sbs{};
        bool hasNt = (CallNtPowerInformation(SystemBatteryState, nullptr, 0, &sbs, sizeof(sbs)) == 0);
        GetSystemPowerStatus(&sps);

        PortInfo p{};
        p.id = L"power:ac:0";
        p.name = L"AC Power Input (PSU)";
        p.category = PortCategory::Power;
        p.active = (sps.ACLineStatus == 1);
        p.connectedDevice = p.active ? L"Mains AC Power Supply" : L"N/A";
        p.statusText = p.active ? L"AC Online" : L"Unused";
        p.hardwareSpec = L"Desktop PSU (No Wattage Sensor)";
        p.current.timestampSec = nowSec;

        if (hasNt && sbs.BatteryPresent && sbs.Rate != 0 && sbs.Rate != static_cast<DWORD>(BATTERY_UNKNOWN_RATE)) {
            LONG signedRate = static_cast<LONG>(sbs.Rate);
            p.current.hasPower = true;
            p.current.powerWatts = std::fabs(static_cast<double>(signedRate)) / 1000.0;
            p.current.powerDir = (signedRate > 0 || sbs.Charging) ? PowerDirection::Accepted : PowerDirection::Delivered;
            p.powerNote = L"Live System Battery Sensor";
        } else {
            p.current.hasPower = false;
            p.current.powerDir = PowerDirection::None;
            p.powerNote = L"Desktop PSU does not expose wattage telemetry";
        }
        p.dataNote = L"Power-only port";
        out.push_back(std::move(p));
    }
}

// ============================================================================
// 2. USB STORAGE DISK TRAFFIC HELPER (Read-Only access=0 IOCTL_DISK_PERFORMANCE)
// ============================================================================
std::vector<PortMonitor::UsbDiskTraffic> PortMonitor::CollectUsbDiskTraffic(uint64_t nowMs) {
    std::vector<UsbDiskTraffic> result;

    for (int driveIdx = 0; driveIdx < 16; ++driveIdx) {
        wchar_t path[64];
        swprintf_s(path, L"\\\\.\\PhysicalDrive%d", driveIdx);

        // Access = 0 (no read/write data permissions needed; strictly metadata/perf query)
        HANDLE hDrive = CreateFileW(
            path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDrive == INVALID_HANDLE_VALUE) continue;

        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType  = PropertyStandardQuery;

        std::vector<uint8_t> propBuf(1024, 0);
        DWORD bytesRet = 0;
        if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                            &query, sizeof(query),
                            propBuf.data(), static_cast<DWORD>(propBuf.size()),
                            &bytesRet, nullptr)) {
            auto* desc = reinterpret_cast<PSTORAGE_DEVICE_DESCRIPTOR>(propBuf.data());
            if (desc->BusType == BusTypeUsb) {
                DISK_PERFORMANCE dp{};
                if (DeviceIoControl(hDrive, IOCTL_DISK_PERFORMANCE,
                                    nullptr, 0, &dp, sizeof(dp), &bytesRet, nullptr)) {
                    std::wstring key = L"usbdisk:" + std::to_wstring(driveIdx);
                    uint64_t totalRead  = static_cast<uint64_t>(dp.BytesRead.QuadPart);
                    uint64_t totalWrite = static_cast<uint64_t>(dp.BytesWritten.QuadPart);

                    double readBps = 0.0, writeBps = 0.0;
                    auto& prev = prevCounters_[key];
                    if (prev.valid && nowMs > prev.tickMs) {
                        double dt = static_cast<double>(nowMs - prev.tickMs) / 1000.0;
                        if (totalRead >= prev.bytesFro)  readBps  = static_cast<double>(totalRead - prev.bytesFro) / dt;
                        if (totalWrite >= prev.bytesTo)  writeBps = static_cast<double>(totalWrite - prev.bytesTo) / dt;
                    }
                    prev.bytesFro = totalRead;
                    prev.bytesTo  = totalWrite;
                    prev.tickMs   = nowMs;
                    prev.valid    = true;

                    std::wstring prod;
                    if (desc->ProductIdOffset > 0 && desc->ProductIdOffset < propBuf.size()) {
                        prod = ToLower(AnsiToWide(reinterpret_cast<const char*>(propBuf.data() + desc->ProductIdOffset)));
                    }
                    UsbDiskTraffic t{};
                    t.productHint = prod;
                    t.readBps     = readBps;
                    t.writeBps    = writeBps;
                    result.push_back(std::move(t));
                }
            }
        }
        CloseHandle(hDrive);
    }
    return result;
}

// ============================================================================
// 3. USB PORTS (Read-Only Root Hub & External Hub Enumeration)
// ============================================================================
void PortMonitor::PollUsbPorts(std::vector<PortInfo>& out, uint64_t nowSec, uint64_t nowMs) {
    auto usbDisks = CollectUsbDiskTraffic(nowMs);

    static const GUID kGuidUsbHostController = {
        0x3abf6f2d, 0x71c4, 0x462a, { 0x8a, 0x92, 0x1e, 0x68, 0x61, 0xe6, 0xaf, 0x27 }
    };

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        &kGuidUsbHostController, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    int globalPortCounter = 1;
    for (DWORD hcIdx = 0; hcIdx < 16; ++hcIdx) {
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &kGuidUsbHostController, hcIdx, &did)) {
            break;
        }

        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &did, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0) continue;

        std::vector<uint8_t> detailBuf(reqSize, 0);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &did, detail, reqSize, nullptr, nullptr)) {
            continue;
        }

        // Open host controller with 0 / GENERIC_READ only (read-only)
        HANDLE hHC = CreateFileW(
            detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (hHC == INVALID_HANDLE_VALUE) {
            hHC = CreateFileW(
                detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (hHC == INVALID_HANDLE_VALUE) continue;

        USB_ROOT_HUB_NAME rootNameSize{};
        DWORD bytesRet = 0;
        if (DeviceIoControl(hHC, IOCTL_USB_GET_ROOT_HUB_NAME,
                            &rootNameSize, sizeof(rootNameSize),
                            &rootNameSize, sizeof(rootNameSize),
                            &bytesRet, nullptr) && rootNameSize.ActualLength > 0) {
            std::vector<uint8_t> nameBuf(rootNameSize.ActualLength + 4, 0);
            auto* rootName = reinterpret_cast<PUSB_ROOT_HUB_NAME>(nameBuf.data());
            if (DeviceIoControl(hHC, IOCTL_USB_GET_ROOT_HUB_NAME,
                                rootName, static_cast<DWORD>(nameBuf.size()),
                                rootName, static_cast<DWORD>(nameBuf.size()),
                                &bytesRet, nullptr)) {
                std::wstring hubPath = L"\\\\.\\" + std::wstring(rootName->RootHubName);
                std::wstring hubLabel = L"Controller " + std::to_wstring(hcIdx + 1);
                EnumerateUsbHub(hubPath, hubLabel, 0, out, usbDisks, nowSec, globalPortCounter);
            }
        }
        CloseHandle(hHC);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
}

void PortMonitor::EnumerateUsbHub(
    const std::wstring& hubPath,
    const std::wstring& hubLabel,
    int depth,
    std::vector<PortInfo>& out,
    std::vector<UsbDiskTraffic>& usbDisks,
    uint64_t nowSec,
    int& globalPortCounter)
{
    if (depth > 3) return; // Prevent excessive recursion

    HANDLE hHub = CreateFileW(
        hubPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hHub == INVALID_HANDLE_VALUE) {
        hHub = CreateFileW(
            hubPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (hHub == INVALID_HANDLE_VALUE) return;

    USB_NODE_INFORMATION nodeInfo{};
    DWORD bytesRet = 0;
    if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_INFORMATION,
                         &nodeInfo, sizeof(nodeInfo),
                         &nodeInfo, sizeof(nodeInfo),
                         &bytesRet, nullptr)) {
        CloseHandle(hHub);
        return;
    }

    ULONG numPorts = nodeInfo.u.HubInformation.HubDescriptor.bNumberOfPorts;
    if (numPorts > 32) numPorts = 32;

    for (ULONG portIdx = 1; portIdx <= numPorts; ++portIdx) {
        constexpr size_t kConnBufSize = sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + sizeof(USB_PIPE_INFO) * 30;
        std::vector<uint8_t> connBuf(kConnBufSize, 0);
        auto* conn = reinterpret_cast<PUSB_NODE_CONNECTION_INFORMATION_EX>(connBuf.data());
        conn->ConnectionIndex = portIdx;

        if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                             conn, static_cast<DWORD>(connBuf.size()),
                             conn, static_cast<DWORD>(connBuf.size()),
                             &bytesRet, nullptr)) {
            continue;
        }

        // If this port hosts an external hub, also recurse into it
        if (conn->ConnectionStatus == DeviceConnected && conn->DeviceIsHub) {
            USB_NODE_CONNECTION_NAME extHubSize{};
            extHubSize.ConnectionIndex = portIdx;
            if (DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_NAME,
                                &extHubSize, sizeof(extHubSize),
                                &extHubSize, sizeof(extHubSize),
                                &bytesRet, nullptr) && extHubSize.ActualLength > 0) {
                std::vector<uint8_t> extBuf(extHubSize.ActualLength + 4, 0);
                auto* extName = reinterpret_cast<PUSB_NODE_CONNECTION_NAME>(extBuf.data());
                extName->ConnectionIndex = portIdx;
                if (DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_NAME,
                                    extName, static_cast<DWORD>(extBuf.size()),
                                    extName, static_cast<DWORD>(extBuf.size()),
                                    &bytesRet, nullptr)) {
                    std::wstring subHubPath = L"\\\\.\\" + std::wstring(extName->NodeName);
                    std::wstring subLabel = hubLabel + L" > Hub P" + std::to_wstring(portIdx);
                    EnumerateUsbHub(subHubPath, subLabel, depth + 1, out, usbDisks, nowSec, globalPortCounter);
                }
            }
        }

        PortInfo p{};
        wchar_t idBuf[128];
        swprintf_s(idBuf, L"usb:%s:p%lu", hubLabel.c_str(), portIdx);
        p.id = idBuf;

        wchar_t nameBuf[128];
        swprintf_s(nameBuf, L"USB Port %d (%s, P%lu)", globalPortCounter++, hubLabel.c_str(), portIdx);
        p.name = nameBuf;
        p.category = PortCategory::USB;
        p.current.timestampSec = nowSec;

        bool connected = (conn->ConnectionStatus == DeviceConnected);
        p.active = connected;

        if (!connected) {
            p.statusText       = L"Unused";
            p.connectedDevice  = L"N/A";
            p.hardwareSpec     = L"No device attached";
            p.current.hasDataTo  = false;
            p.current.hasDataFro = false;
            p.current.hasPower   = false;
            p.current.powerDir   = PowerDirection::None;
            p.dataNote  = L"Port unused";
            p.powerNote = L"Port unused";
        } else {
            p.statusText = conn->DeviceIsHub ? L"Active (Hub)" : L"Active";

            std::wstring prod = QueryUsbStringDescriptor(hHub, portIdx, conn->DeviceDescriptor.iProduct);
            std::wstring mfg  = QueryUsbStringDescriptor(hHub, portIdx, conn->DeviceDescriptor.iManufacturer);
            if (!prod.empty() && !mfg.empty() && ToLower(prod).find(ToLower(mfg)) == std::wstring::npos) {
                p.connectedDevice = mfg + L" " + prod;
            } else if (!prod.empty()) {
                p.connectedDevice = prod;
            } else if (!mfg.empty()) {
                p.connectedDevice = mfg + L" USB Device";
            } else {
                wchar_t vidPid[64];
                swprintf_s(vidPid, L"USB Device (VID_%04X PID_%04X)",
                           conn->DeviceDescriptor.idVendor, conn->DeviceDescriptor.idProduct);
                p.connectedDevice = vidPid;
            }

            p.hardwareSpec = UsbSpeedToString(conn->Speed);

            // Power budget from USB Configuration Descriptor
            double watts = 0.0;
            uint32_t ma = 0;
            if (QueryUsbConfigPowerWatts(hHub, portIdx, conn->Speed, watts, ma)) {
                p.current.hasPower   = true;
                p.current.powerWatts = watts;
                p.current.powerDir   = PowerDirection::Delivered;
                wchar_t noteBuf[96];
                swprintf_s(noteBuf, L"Negotiated USB VBUS (%u mA @ 5V)", ma);
                p.powerNote = noteBuf;
            } else {
                p.current.hasPower = false;
                p.current.powerDir = PowerDirection::None;
                p.powerNote = L"Self-powered or no VBUS draw reported";
            }

            // Try to match real-time USB Storage disk throughput
            std::wstring devLower = ToLower(p.connectedDevice);
            bool matchedDisk = false;
            for (auto& d : usbDisks) {
                if (!d.used && !d.productHint.empty() &&
                    (devLower.find(d.productHint) != std::wstring::npos ||
                     d.productHint.find(devLower) != std::wstring::npos)) {
                    d.used = true;
                    p.current.hasDataTo   = true;
                    p.current.hasDataFro  = true;
                    p.current.bytesToSec  = d.writeBps;
                    p.current.bytesFroSec = d.readBps;
                    p.dataNote = L"Live USB Storage Throughput";
                    matchedDisk = true;
                    break;
                }
            }
            if (!matchedDisk) {
                p.current.hasDataTo  = false;
                p.current.hasDataFro = false;
                p.dataNote = L"OS does not expose per-port raw USB byte counters for this device class";
            }
        }

        out.push_back(std::move(p));
    }

    CloseHandle(hHub);
}

// ============================================================================
// 4. NETWORK PORTS (Read-Only GetIfTable2)
// ============================================================================
void PortMonitor::PollNetworkPorts(std::vector<PortInfo>& out, uint64_t nowSec, uint64_t nowMs) {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return;

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];

        // Skip loopback, tunnel, and software-only virtual miniports
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) continue;
        if (!row.InterfaceAndOperStatusFlags.HardwareInterface) continue;

        // Only Ethernet, Wi-Fi, Bluetooth/PAN, WWAN hardware adapters
        bool isPhysical = (row.Type == IF_TYPE_ETHERNET_CSMACD ||
                           row.Type == IF_TYPE_IEEE80211 ||
                           row.Type == IF_TYPE_WWANPP ||
                           row.Type == IF_TYPE_WWANPP2);
        if (!isPhysical) continue;

        PortInfo p{};
        wchar_t idBuf[96];
        swprintf_s(idBuf, L"net:%llu", static_cast<unsigned long long>(row.InterfaceLuid.Value));
        p.id = idBuf;

        std::wstring alias(row.Alias);
        std::wstring desc(row.Description);
        p.name = alias.empty() ? desc : alias;
        p.category = PortCategory::Network;
        p.current.timestampSec = nowSec;

        bool isUp = (row.OperStatus == IfOperStatusUp);
        p.active = isUp;

        if (!isUp) {
            p.statusText      = L"Unused";
            p.connectedDevice = L"N/A";
            p.hardwareSpec    = desc;
            p.current.hasDataTo  = false;
            p.current.hasDataFro = false;
            p.dataNote = L"Link disconnected";
        } else {
            p.statusText      = L"Active";
            p.connectedDevice = desc;
            p.hardwareSpec    = FormatLinkSpeed(std::max(row.TransmitLinkSpeed, row.ReceiveLinkSpeed));

            double toBps = 0.0, froBps = 0.0;
            auto& prev = prevCounters_[p.id];
            if (prev.valid && nowMs > prev.tickMs) {
                double dt = static_cast<double>(nowMs - prev.tickMs) / 1000.0;
                if (row.OutOctets >= prev.bytesTo)  toBps  = static_cast<double>(row.OutOctets - prev.bytesTo) / dt;
                if (row.InOctets  >= prev.bytesFro) froBps = static_cast<double>(row.InOctets  - prev.bytesFro) / dt;
            }
            prev.bytesTo  = row.OutOctets;
            prev.bytesFro = row.InOctets;
            prev.tickMs   = nowMs;
            prev.valid    = true;

            p.current.hasDataTo   = true;
            p.current.hasDataFro  = true;
            p.current.bytesToSec  = toBps;
            p.current.bytesFroSec = froBps;
            p.dataNote = L"Live Network Interface Counter (TX / RX)";
        }

        // Network ports do not expose power telemetry via standard Windows APIs
        p.current.hasPower = false;
        p.current.powerDir = PowerDirection::None;
        p.powerNote = L"Not reported by NIC hardware";

        out.push_back(std::move(p));
    }

    FreeMibTable(table);
}

// ============================================================================
// 5. DISPLAY / VIDEO PORTS (Read-Only QueryDisplayConfig)
// ============================================================================
void PortMonitor::PollDisplayPorts(std::vector<PortInfo>& out, uint64_t nowSec) {
    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes) != ERROR_SUCCESS) return;
    if (numPaths == 0) return;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &numPaths, paths.data(), &numModes, modes.data(), nullptr) != ERROR_SUCCESS) {
        return;
    }

    // Deduplicate targets by (adapterId, targetId)
    std::unordered_map<uint64_t, size_t> seenTarget;

    for (UINT32 i = 0; i < numPaths; ++i) {
        const auto& path = paths[i];
        uint64_t targetKey = (static_cast<uint64_t>(path.targetInfo.adapterId.LowPart) << 32) | path.targetInfo.id;
        bool isActive = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;

        auto it = seenTarget.find(targetKey);
        if (it != seenTarget.end()) {
            // If we already recorded an inactive path for this target, upgrade if this path is active
            if (!isActive || out[it->second].active) continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size      = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id        = path.targetInfo.id;

        std::wstring monitorName;
        DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech = path.targetInfo.outputTechnology;
        if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS) {
            tech = targetName.outputTechnology;
            if (targetName.monitorFriendlyDeviceName[0] != L'\0') {
                monitorName = targetName.monitorFriendlyDeviceName;
            }
        }

        PortInfo p{};
        wchar_t idBuf[96];
        swprintf_s(idBuf, L"display:%08lx:%u", path.targetInfo.adapterId.LowPart, path.targetInfo.id);
        p.id = idBuf;

        wchar_t nameBuf[128];
        swprintf_s(nameBuf, L"%s #%u", DisplayTechToName(tech), path.targetInfo.id & 0xFF);
        p.name = nameBuf;
        p.category = PortCategory::Display;
        p.active = isActive;
        p.current.timestampSec = nowSec;

        if (!isActive) {
            p.statusText      = L"Unused";
            p.connectedDevice = L"N/A";
            p.hardwareSpec    = L"No display signal";
            p.current.hasDataTo  = false;
            p.current.hasDataFro = false;
            p.dataNote = L"Port unused";
        } else {
            p.statusText      = L"Active";
            p.connectedDevice = monitorName.empty() ? L"Connected Display" : monitorName;

            double refreshHz = 60.0;
            if (path.targetInfo.refreshRate.Denominator > 0) {
                refreshHz = static_cast<double>(path.targetInfo.refreshRate.Numerator) /
                            static_cast<double>(path.targetInfo.refreshRate.Denominator);
            }

            UINT32 width = 0, height = 0;
            if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
                path.sourceInfo.modeInfoIdx < numModes &&
                modes[path.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                width  = modes[path.sourceInfo.modeInfoIdx].sourceMode.width;
                height = modes[path.sourceInfo.modeInfoIdx].sourceMode.height;
            }

            if (width > 0 && height > 0) {
                wchar_t specBuf[96];
                swprintf_s(specBuf, L"%ux%u @ %.0f Hz (24-bit RGB)", width, height, refreshHz);
                p.hardwareSpec = specBuf;
                // Uncompressed pixel stream bandwidth (Bytes/sec Outgoing To Display)
                double bytesPerSec = static_cast<double>(width) * static_cast<double>(height) * 3.0 * refreshHz;
                p.current.hasDataTo   = true;
                p.current.bytesToSec  = bytesPerSec;
                p.current.hasDataFro  = false; // Display video is unidirectional
                p.dataNote = L"Video Stream Output Rate (Width x Height x 3B x Hz)";
            } else {
                p.hardwareSpec = L"Active Video Signal";
                p.current.hasDataTo  = false;
                p.current.hasDataFro = false;
            }
        }

        p.current.hasPower = false;
        p.current.powerDir = PowerDirection::None;
        p.powerNote = L"Not reported by display interface";

        if (it != seenTarget.end()) {
            out[it->second] = std::move(p);
        } else {
            seenTarget[targetKey] = out.size();
            out.push_back(std::move(p));
        }
    }
}

// ============================================================================
// 6. AUDIO PORTS (Read-Only Core Audio MMDevice API)
// ============================================================================
void PortMonitor::PollAudioPorts(std::vector<PortInfo>& out, uint64_t nowSec) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return;

    auto enumFlow = [&](EDataFlow flow, bool isOutput) {
        IMMDeviceCollection* coll = nullptr;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &coll)) || !coll) {
            return;
        }
        UINT count = 0;
        coll->GetCount(&count);

        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev)) || !dev) continue;

            LPWSTR devId = nullptr;
            dev->GetId(&devId);
            DWORD state = 0;
            dev->GetState(&state);

            std::wstring friendlyName = isOutput ? L"Audio Output Port" : L"Audio Input Port";
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                    friendlyName = pv.pwszVal;
                }
                PropVariantClear(&pv);
                props->Release();
            }

            PortInfo p{};
            p.id = std::wstring(L"audio:") + (devId ? devId : std::to_wstring(i));
            p.name = (isOutput ? L"Audio Out: " : L"Audio In: ") + friendlyName;
            p.category = PortCategory::Audio;
            p.current.timestampSec = nowSec;

            bool active = (state == DEVICE_STATE_ACTIVE);
            p.active = active;

            if (!active) {
                p.statusText      = L"Unused";
                p.connectedDevice = L"N/A";
                p.hardwareSpec    = L"Unplugged";
                p.current.hasDataTo  = false;
                p.current.hasDataFro = false;
                p.dataNote = L"Port unplugged";
            } else {
                p.statusText      = L"Active";
                p.connectedDevice = friendlyName;

                double streamBps = 0.0;
                IAudioClient* audioClient = nullptr;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient))) && audioClient) {
                    WAVEFORMATEX* wfx = nullptr;
                    if (SUCCEEDED(audioClient->GetMixFormat(&wfx)) && wfx) {
                        wchar_t specBuf[96];
                        swprintf_s(specBuf, L"%lu Hz, %u-bit, %u ch",
                                   wfx->nSamplesPerSec, wfx->wBitsPerSample, wfx->nChannels);
                        p.hardwareSpec = specBuf;
                        streamBps = static_cast<double>(wfx->nAvgBytesPerSec);
                        CoTaskMemFree(wfx);
                    }
                    audioClient->Release();
                }

                float peak = 0.0f;
                IAudioMeterInformation* meter = nullptr;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&meter))) && meter) {
                    meter->GetPeakValue(&peak);
                    meter->Release();
                }

                double liveBps = (peak > 0.0005f) ? streamBps : 0.0;
                if (isOutput) {
                    p.current.hasDataTo  = (streamBps > 0.0);
                    p.current.bytesToSec = liveBps;
                    p.current.hasDataFro = false;
                    p.dataNote = (liveBps > 0.0) ? L"Active Audio Playback Stream" : L"Idle Audio Output";
                } else {
                    p.current.hasDataTo   = false;
                    p.current.hasDataFro  = (streamBps > 0.0);
                    p.current.bytesFroSec = liveBps;
                    p.dataNote = (liveBps > 0.0) ? L"Active Audio Capture Stream" : L"Idle Audio Input";
                }
            }

            p.current.hasPower = false;
            p.current.powerDir = PowerDirection::None;
            p.powerNote = L"Not reported by audio hardware";

            if (devId) CoTaskMemFree(devId);
            dev->Release();
            out.push_back(std::move(p));
        }
        coll->Release();
    };

    enumFlow(eRender, true);
    enumFlow(eCapture, false);
    enumerator->Release();
}

// ============================================================================
// 7. SERIAL / COM PORTS (Read-Only Registry KEY_READ)
// ============================================================================
void PortMonitor::PollSerialPorts(std::vector<PortInfo>& out, uint64_t nowSec) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    DWORD idx = 0;
    wchar_t valName[256];
    wchar_t data[256];
    for (;;) {
        DWORD valNameLen = 256;
        DWORD dataBytes  = sizeof(data);
        DWORD type = 0;
        LONG res = RegEnumValueW(hKey, idx++, valName, &valNameLen, nullptr, &type,
                                 reinterpret_cast<LPBYTE>(data), &dataBytes);
        if (res != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;

        PortInfo p{};
        p.id = std::wstring(L"serial:") + data;
        p.name = std::wstring(L"Serial Port ") + data;
        p.category = PortCategory::Serial;
        p.active = true;
        p.statusText = L"Available";
        p.connectedDevice = valName;
        p.hardwareSpec = L"RS-232 / Virtual COM";
        p.current.timestampSec = nowSec;
        p.current.hasDataTo  = false;
        p.current.hasDataFro = false;
        p.current.hasPower   = false;
        p.current.powerDir   = PowerDirection::None;
        p.dataNote  = L"Read-only mode (does not open COM port)";
        p.powerNote = L"Not applicable";

        out.push_back(std::move(p));
    }

    RegCloseKey(hKey);
}

} // namespace porttool
