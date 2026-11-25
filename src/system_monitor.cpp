#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>

#include "system_monitor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace porttool {

namespace {

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring TrimStr(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && std::iswspace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring AnsiToWide(const char* src, size_t maxLen = 256) {
    if (!src || !src[0]) return L"";
    size_t len = strnlen(src, maxLen);
    if (len == 0) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, src, static_cast<int>(len), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, src, static_cast<int>(len), out.data(), needed);
    return TrimStr(out);
}

const wchar_t* BusTypeToString(STORAGE_BUS_TYPE bus) {
    switch (bus) {
        case BusTypeNvme:  return L"NVMe SSD";
        case BusTypeSata:  return L"SATA";
        case BusTypeUsb:   return L"USB Storage";
        case BusTypeScsi:  return L"SCSI";
        case BusTypeSas:   return L"SAS";
        case BusTypeRAID:  return L"RAID";
        case BusTypeSd:    return L"SD Card";
        case BusTypeMmc:   return L"eMMC";
        case BusTypeAtapi: return L"ATAPI";
        default:           return L"Storage";
    }
}

std::wstring FormatUptime(uint64_t ms) {
    uint64_t totalSec = ms / 1000ULL;
    uint64_t days = totalSec / 86400ULL;
    uint64_t hrs  = (totalSec % 86400ULL) / 3600ULL;
    uint64_t mins = (totalSec % 3600ULL) / 60ULL;
    uint64_t secs = totalSec % 60ULL;
    wchar_t buf[64];
    swprintf_s(buf, L"%llu:%02llu:%02llu:%02llu", days, hrs, mins, secs);
    return buf;
}

std::wstring FormatCacheSize(uint64_t bytes) {
    if (bytes == 0) return L"N/A";
    wchar_t buf[32];
    if (bytes >= 1024ULL * 1024ULL) {
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%llu KB", bytes / 1024ULL);
    }
    return buf;
}

// Minimal WDDM D3DKMT definitions (read-only QueryAdapterInfo from gdi32.dll, same as Task Manager)
using NTSTATUS_KMT = LONG;
constexpr UINT kKmtQaiAdapterPerfData = 62;

struct KmtOpenAdapterFromLuid {
    LUID AdapterLuid;
    UINT hAdapter;
};

struct KmtCloseAdapter {
    UINT hAdapter;
};

struct KmtQueryAdapterInfo {
    UINT  hAdapter;
    UINT  Type;
    void* pPrivateDriverData;
    UINT  PrivateDriverDataSize;
};

struct KmtAdapterPerfData {
    UINT32 PhysicalAdapterIndex;
    UINT64 MemoryFrequency;      // Hz
    UINT64 MaxMemoryFrequency;   // Hz
    UINT64 MaxMemoryFrequencyOC; // Hz
    UINT64 MemoryBandwidth;      // Bytes/s
    UINT64 PCIEBandwidth;        // Bytes/s
    UINT32 FanRPM;
    UINT32 Power;                // Tenths of a percent of TDP (1000 = 100%)
    UINT32 Temperature;          // Tenths of a degree Celsius (450 = 45.0 C)
    UCHAR  PowerStateOverride;
};

using FnD3DKMTOpenAdapterFromLuid = NTSTATUS_KMT(WINAPI*)(KmtOpenAdapterFromLuid*);
using FnD3DKMTQueryAdapterInfo    = NTSTATUS_KMT(WINAPI*)(const KmtQueryAdapterInfo*);
using FnD3DKMTCloseAdapter        = NTSTATUS_KMT(WINAPI*)(const KmtCloseAdapter*);

struct GpuPerfTelemetry {
    bool     hasTemp      = false;
    double   tempC        = 0.0;
    bool     hasPowerW    = false;
    double   powerWatts   = 0.0;
    bool     hasPowerPct  = false;
    double   powerPct     = 0.0;
    uint32_t fanRpm       = 0;
    uint64_t memFreqHz    = 0;
    std::wstring tempSource;
    std::wstring powerSource;
};

GpuPerfTelemetry QueryGpuPerfTelemetry(LUID luid, UINT vendorId, UINT deviceId) {
    GpuPerfTelemetry out{};

    // 1. WDDM D3DKMTQueryAdapterInfo (built-in gdi32.dll — works on AMD, NVIDIA, Intel WDDM 2.4+)
    static HMODULE hGdi = GetModuleHandleW(L"gdi32.dll");
    static auto fnOpen  = hGdi ? reinterpret_cast<FnD3DKMTOpenAdapterFromLuid>(GetProcAddress(hGdi, "D3DKMTOpenAdapterFromLuid")) : nullptr;
    static auto fnQuery = hGdi ? reinterpret_cast<FnD3DKMTQueryAdapterInfo>(GetProcAddress(hGdi, "D3DKMTQueryAdapterInfo"))       : nullptr;
    static auto fnClose = hGdi ? reinterpret_cast<FnD3DKMTCloseAdapter>(GetProcAddress(hGdi, "D3DKMTCloseAdapter"))               : nullptr;

    if (fnOpen && fnQuery && fnClose) {
        KmtOpenAdapterFromLuid openArg{};
        openArg.AdapterLuid = luid;
        if (fnOpen(&openArg) == 0 && openArg.hAdapter != 0) {
            KmtAdapterPerfData perf{};
            perf.PhysicalAdapterIndex = 0;
            KmtQueryAdapterInfo q{};
            q.hAdapter              = openArg.hAdapter;
            q.Type                  = kKmtQaiAdapterPerfData;
            q.pPrivateDriverData    = &perf;
            q.PrivateDriverDataSize = sizeof(perf);

            if (fnQuery(&q) == 0) {
                if (perf.Temperature > 0 && perf.Temperature < 1500) {
                    out.hasTemp    = true;
                    out.tempC      = static_cast<double>(perf.Temperature) / 10.0;
                    out.tempSource = L"WDDM D3DKMT GPU Thermal Sensor";
                }
                if (perf.Power > 0 && perf.Power <= 2000) {
                    out.hasPowerPct = true;
                    out.powerPct    = static_cast<double>(perf.Power) / 10.0;
                }
                out.fanRpm    = perf.FanRPM;
                out.memFreqHz = perf.MemoryFrequency;
            }

            KmtCloseAdapter closeArg{ openArg.hAdapter };
            fnClose(&closeArg);
        }
    }

    // 2. Optional read-only NVML query for NVIDIA GPUs (only if driver's nvml.dll is already on system)
    if (vendorId == 0x10DE) {
        static bool nvmlInitAttempted = false;
        static bool nvmlReady = false;
        using FnNvmlInit     = int(*)(void);
        using FnNvmlGetCount = int(*)(unsigned int*);
        using FnNvmlGetByIdx = int(*)(unsigned int, void**);
        using FnNvmlGetPci   = int(*)(void*, void*);
        using FnNvmlGetTemp  = int(*)(void*, int, unsigned int*);
        using FnNvmlGetPower = int(*)(void*, unsigned int*);

        static FnNvmlGetCount fnCount = nullptr;
        static FnNvmlGetByIdx fnByIdx = nullptr;
        static FnNvmlGetPci   fnPci   = nullptr;
        static FnNvmlGetTemp  fnTemp  = nullptr;
        static FnNvmlGetPower fnPower = nullptr;

        if (!nvmlInitAttempted) {
            nvmlInitAttempted = true;
            HMODULE hNvml = LoadLibraryW(L"nvml.dll");
            if (hNvml) {
                auto fnInit = reinterpret_cast<FnNvmlInit>(GetProcAddress(hNvml, "nvmlInit_v2"));
                fnCount = reinterpret_cast<FnNvmlGetCount>(GetProcAddress(hNvml, "nvmlDeviceGetCount_v2"));
                fnByIdx = reinterpret_cast<FnNvmlGetByIdx>(GetProcAddress(hNvml, "nvmlDeviceGetHandleByIndex_v2"));
                fnPci   = reinterpret_cast<FnNvmlGetPci>(GetProcAddress(hNvml, "nvmlDeviceGetPciInfo_v3"));
                fnTemp  = reinterpret_cast<FnNvmlGetTemp>(GetProcAddress(hNvml, "nvmlDeviceGetTemperature"));
                fnPower = reinterpret_cast<FnNvmlGetPower>(GetProcAddress(hNvml, "nvmlDeviceGetPowerUsage"));
                if (fnInit && fnCount && fnByIdx && fnTemp && fnPower && fnInit() == 0) {
                    nvmlReady = true;
                }
            }
        }

        if (nvmlReady) {
            unsigned int count = 0;
            if (fnCount(&count) == 0 && count > 0) {
                for (unsigned int i = 0; i < count; ++i) {
                    void* dev = nullptr;
                    if (fnByIdx(i, &dev) != 0 || !dev) continue;

                    // Match PCI deviceId if multiple NVIDIA GPUs exist
                    if (count > 1 && fnPci) {
                        struct NvmlPciInfo {
                            char busIdLegacy[16];
                            unsigned int domain, bus, device, pciDeviceId, pciSubSystemId;
                            char busId[32];
                        } pci{};
                        if (fnPci(dev, &pci) == 0) {
                            UINT nvDevId = (pci.pciDeviceId >> 16) & 0xFFFF;
                            if (nvDevId != 0 && nvDevId != deviceId) continue;
                        }
                    }

                    unsigned int tC = 0;
                    if (fnTemp(dev, 0 /* NVML_TEMPERATURE_GPU */, &tC) == 0 && tC > 0 && tC < 130) {
                        out.hasTemp    = true;
                        out.tempC      = static_cast<double>(tC);
                        out.tempSource = L"NVIDIA GPU Thermal Sensor (NVML)";
                    }
                    unsigned int mw = 0;
                    if (fnPower(dev, &mw) == 0 && mw > 0) {
                        out.hasPowerW   = true;
                        out.powerWatts  = static_cast<double>(mw) / 1000.0;
                        out.powerSource = L"NVIDIA GPU Power Sensor (NVML)";
                    }
                    break;
                }
            }
        }
    }

    return out;
}

} // namespace

SystemMonitor::SystemMonitor() {
    startTickMs_ = GetTickCount64();
    InitStaticHardwareInfo();
    InitPdhCounters();
}

SystemMonitor::~SystemMonitor() {
    ShutdownPdhCounters();
}

void SystemMonitor::InitStaticHardwareInfo() {
    // 1. CPU Name & Base Speed from Registry (strictly KEY_READ)
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t nameBuf[256]{};
        DWORD sz = sizeof(nameBuf);
        if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(nameBuf), &sz) == ERROR_SUCCESS) {
            cpuName_ = TrimStr(nameBuf);
        }
        DWORD mhz = 0;
        DWORD mhzSz = sizeof(mhz);
        if (RegQueryValueExW(hKey, L"~MHz", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&mhz), &mhzSz) == ERROR_SUCCESS) {
            cpuBaseMhz_ = mhz;
        }
        RegCloseKey(hKey);
    }

    // 2. Logical / Physical Cores, Sockets, Caches via GetLogicalProcessorInformationEx
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    if (len > 0) {
        std::vector<uint8_t> buf(len, 0);
        if (GetLogicalProcessorInformationEx(
                RelationAll,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
                &len)) {
            uint32_t cores = 0, sockets = 0;
            uint64_t l1 = 0, l2 = 0, l3 = 0;
            DWORD offset = 0;
            while (offset < len) {
                auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + offset);
                if (info->Size == 0) break;
                if (info->Relationship == RelationProcessorCore) {
                    ++cores;
                } else if (info->Relationship == RelationProcessorPackage) {
                    ++sockets;
                } else if (info->Relationship == RelationCache) {
                    if (info->Cache.Level == 1)      l1 += info->Cache.CacheSize;
                    else if (info->Cache.Level == 2) l2 += info->Cache.CacheSize;
                    else if (info->Cache.Level == 3) l3 += info->Cache.CacheSize;
                }
                offset += info->Size;
            }
            if (cores > 0)   cpuPhysCores_ = cores;
            if (sockets > 0) cpuSockets_   = sockets;
            cpuL1Bytes_ = l1;
            cpuL2Bytes_ = l2;
            cpuL3Bytes_ = l3;
        }
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    cpuLogProcs_ = si.dwNumberOfProcessors;
    if (cpuPhysCores_ == 0) cpuPhysCores_ = cpuLogProcs_;

    cpuVirtEnabled_ = (IsProcessorFeaturePresent(PF_VIRT_FIRMWARE_ENABLED) != FALSE);

    ULONGLONG installedKb = 0;
    if (GetPhysicallyInstalledSystemMemory(&installedKb)) {
        physInstalledKb_ = installedKb;
    }
}

void SystemMonitor::InitPdhCounters() {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS || !q) return;
    pdhQuery_ = q;

    auto addCounter = [&](const wchar_t* path, void*& outHandle) {
        PDH_HCOUNTER h = nullptr;
        if (PdhAddEnglishCounterW(q, path, 0, &h) == ERROR_SUCCESS) {
            outHandle = h;
        }
    };

    addCounter(L"\\Processor Information(_Total)\\% Processor Utility",     pdhCpuUtil_);
    addCounter(L"\\Processor Information(_Total)\\% Processor Performance", pdhCpuPerf_);
    addCounter(L"\\Processor Information(_Total)\\% Privileged Time",       pdhCpuPriv_);
    addCounter(L"\\Energy Meter(RAPL_Package0_PKG)\\Power",                 pdhRaplPkg_);
    addCounter(L"\\Thermal Zone Information(*)\\Temperature",               pdhThermals_);
    addCounter(L"\\GPU Engine(*)\\Utilization Percentage",                  pdhGpuEngine_);

    PdhCollectQueryData(q);
}

void SystemMonitor::ShutdownPdhCounters() {
    if (pdhQuery_) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(pdhQuery_));
        pdhQuery_     = nullptr;
        pdhCpuUtil_   = nullptr;
        pdhCpuPerf_   = nullptr;
        pdhCpuPriv_   = nullptr;
        pdhRaplPkg_   = nullptr;
        pdhThermals_  = nullptr;
        pdhGpuEngine_ = nullptr;
    }
}

void SystemMonitor::PollOnce() {
    uint64_t nowMs = GetTickCount64();
    if (pdhQuery_) {
        PdhCollectQueryData(static_cast<PDH_HQUERY>(pdhQuery_));
    }

    std::vector<SysCompInfo> polled;
    polled.reserve(16);

    PollCpu(polled, nowMs);
    PollGpus(polled, nowMs);
    PollDisks(polled, nowMs);
    PollMemory(polled, nowMs);

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::wstring> newOrder;
    newOrder.reserve(polled.size());

    for (auto& c : polled) {
        newOrder.push_back(c.id);
        auto it = comps_.find(c.id);
        if (it == comps_.end()) {
            c.history.push_back(c.current);
            comps_.emplace(c.id, std::move(c));
        } else {
            auto& ex = it->second;
            ex.name               = c.name;
            ex.shortSubtitle      = c.shortSubtitle;
            ex.type               = c.type;
            ex.utilSummary        = c.utilSummary;
            ex.dataSummary        = c.dataSummary;
            ex.powerSummary       = c.powerSummary;
            ex.tempSummary        = c.tempSummary;
            ex.utilTitle          = c.utilTitle;
            ex.utilSeries1Name    = c.utilSeries1Name;
            ex.utilSeries2Name    = c.utilSeries2Name;
            ex.dataTitle          = c.dataTitle;
            ex.dataSeries1Name    = c.dataSeries1Name;
            ex.dataSeries2Name    = c.dataSeries2Name;
            ex.dataIsBytesNotRate = c.dataIsBytesNotRate;
            ex.powerNote          = c.powerNote;
            ex.tempNote           = c.tempNote;
            ex.details            = std::move(c.details);
            ex.current            = c.current;
            ex.history.push_back(c.current);
            if (ex.history.size() > kMaxHistorySamples) {
                ex.history.erase(ex.history.begin(),
                                 ex.history.begin() + (ex.history.size() - kMaxHistorySamples));
            }
        }
    }

    compOrder_ = std::move(newOrder);
}

std::vector<SysCompInfo> SystemMonitor::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SysCompInfo> res;
    res.reserve(compOrder_.size());
    for (const auto& id : compOrder_) {
        auto it = comps_.find(id);
        if (it != comps_.end()) res.push_back(it->second);
    }
    return res;
}

// ============================================================================
// 1. PROCESSOR (CPU)
// ============================================================================
void SystemMonitor::PollCpu(std::vector<SysCompInfo>& out, uint64_t nowMs) {
    SysCompInfo c{};
    c.id   = L"sys:cpu";
    c.name = cpuName_;
    c.type = SysCompType::CPU;
    c.shortSubtitle = std::to_wstring(cpuPhysCores_) + L" Cores / " + std::to_wstring(cpuLogProcs_) + L" Logical";
    c.current.timestampMs = nowMs;

    // Utilization (prefer PDH % Processor Utility, fallback to GetSystemTimes)
    double cpuPct = 0.0;
    bool hasCpuPct = false;
    if (pdhCpuUtil_) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(pdhCpuUtil_), PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS &&
            v.CStatus == ERROR_SUCCESS) {
            cpuPct = std::clamp(v.doubleValue, 0.0, 100.0);
            hasCpuPct = true;
        }
    }

    FILETIME ftIdle{}, ftKernel{}, ftUser{};
    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
        uint64_t idle   = FileTimeToU64(ftIdle);
        uint64_t kernel = FileTimeToU64(ftKernel);
        uint64_t user   = FileTimeToU64(ftUser);
        if (hasPrevCpu_) {
            uint64_t dIdle   = idle - prevIdle_;
            uint64_t dKernel = kernel - prevKernel_;
            uint64_t dUser   = user - prevUser_;
            uint64_t total   = dKernel + dUser;
            if (!hasCpuPct && total > 0) {
                cpuPct = std::clamp(static_cast<double>(total - dIdle) * 100.0 / static_cast<double>(total), 0.0, 100.0);
                hasCpuPct = true;
            }
        }
        prevIdle_ = idle; prevKernel_ = kernel; prevUser_ = user;
        hasPrevCpu_ = true;
    }

    double kernelPct = 0.0;
    bool hasKernel = false;
    if (pdhCpuPriv_) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(pdhCpuPriv_), PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS &&
            v.CStatus == ERROR_SUCCESS) {
            kernelPct = std::clamp(v.doubleValue, 0.0, 100.0);
            hasKernel = true;
        }
    }

    // Live Clock Speed via % Processor Performance * Base MHz
    double curGhz = (cpuBaseMhz_ > 0) ? (static_cast<double>(cpuBaseMhz_) / 1000.0) : 0.0;
    if (pdhCpuPerf_ && cpuBaseMhz_ > 0) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(pdhCpuPerf_), PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS &&
            v.CStatus == ERROR_SUCCESS && v.doubleValue > 0.0) {
            curGhz = (static_cast<double>(cpuBaseMhz_) * (v.doubleValue / 100.0)) / 1000.0;
        }
    }

    // Power (RAPL Package Power in mW -> W)
    if (pdhRaplPkg_) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(pdhRaplPkg_), PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS &&
            v.CStatus == ERROR_SUCCESS && v.doubleValue > 0.0) {
            c.current.hasPower   = true;
            c.current.powerWatts = v.doubleValue / 1000.0;
            c.powerNote = L"Hardware Energy Meter (RAPL Package)";
        }
    }

    // Temperature (ACPI Thermal Zone in Kelvin -> Celsius)
    if (pdhThermals_) {
        DWORD bufSize = 0, itemCount = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(
            static_cast<PDH_HCOUNTER>(pdhThermals_), PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
        if (st == PDH_MORE_DATA && bufSize > 0) {
            std::vector<uint8_t> buf(bufSize, 0);
            auto* items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
            if (PdhGetFormattedCounterArrayW(
                    static_cast<PDH_HCOUNTER>(pdhThermals_), PDH_FMT_DOUBLE, &bufSize, &itemCount, items) == ERROR_SUCCESS) {
                double maxTempC = -999.0;
                for (DWORD i = 0; i < itemCount; ++i) {
                    if (items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                        double val = items[i].FmtValue.doubleValue;
                        // Windows Thermal Zone Information reports Kelvin (e.g. 308 K = 34.85 C)
                        double tc = (val > 200.0) ? (val - 273.15) : val;
                        if (tc > 0.0 && tc < 130.0) {
                            maxTempC = std::max(maxTempC, tc);
                        }
                    }
                }
                if (maxTempC > 0.0) {
                    c.current.hasTemp = true;
                    c.current.tempC   = maxTempC;
                    c.tempNote = L"ACPI Thermal Zone Sensor";
                }
            }
        }
    }

    c.current.hasUtil  = hasCpuPct;
    c.current.utilPct  = cpuPct;
    c.current.hasUtil2 = hasKernel;
    c.current.util2Pct = kernelPct;

    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof(pi);
    GetPerformanceInfo(&pi, sizeof(pi));

    wchar_t spdBuf[48];
    if (curGhz > 0.0) swprintf_s(spdBuf, L"%.2f GHz", curGhz);
    else              swprintf_s(spdBuf, L"N/A");

    wchar_t baseBuf[48];
    if (cpuBaseMhz_ > 0) swprintf_s(baseBuf, L"%.2f GHz", static_cast<double>(cpuBaseMhz_) / 1000.0);
    else                 swprintf_s(baseBuf, L"N/A");

    c.utilSummary  = FormatPercent(cpuPct, hasCpuPct);
    c.dataSummary  = spdBuf;
    c.powerSummary = FormatPowerPlain(c.current.powerWatts, c.current.hasPower);
    c.tempSummary  = FormatTempC(c.current.tempC, c.current.hasTemp);

    c.utilTitle       = L"CPU Utilization vs Time (%)";
    c.utilSeries1Name = L"Total CPU";
    c.utilSeries2Name = L"Kernel Time";

    c.details = {
        { L"Utilization",        c.utilSummary },
        { L"Speed",              spdBuf },
        { L"Base Speed",         baseBuf },
        { L"Sockets",            std::to_wstring(cpuSockets_) },
        { L"Cores",              std::to_wstring(cpuPhysCores_) },
        { L"Logical Processors", std::to_wstring(cpuLogProcs_) },
        { L"Virtualization",     cpuVirtEnabled_ ? L"Enabled" : L"Disabled" },
        { L"L1 Cache",           FormatCacheSize(cpuL1Bytes_) },
        { L"L2 Cache",           FormatCacheSize(cpuL2Bytes_) },
        { L"L3 Cache",           FormatCacheSize(cpuL3Bytes_) },
        { L"Processes",          std::to_wstring(pi.ProcessCount) },
        { L"Threads",            std::to_wstring(pi.ThreadCount) },
        { L"Handles",            std::to_wstring(pi.HandleCount) },
        { L"Up Time",            FormatUptime(nowMs) }
    };

    out.push_back(std::move(c));
}

// ============================================================================
// 2. GPU(s) (DXGI 1.4 Adapters + PDH GPU Engine Utilization)
// ============================================================================
void SystemMonitor::PollGpus(std::vector<SysCompInfo>& out, uint64_t nowMs) {
    // Collect GPU Engine utilization per LUID from PDH
    std::unordered_map<std::wstring, double> gpuUtilByLuid;
    std::unordered_map<std::wstring, double> gpu3dByLuid;
    std::unordered_map<std::wstring, double> gpuCopyByLuid;
    std::unordered_map<std::wstring, double> gpuVidByLuid;

    if (pdhGpuEngine_) {
        DWORD bufSize = 0, itemCount = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(
            static_cast<PDH_HCOUNTER>(pdhGpuEngine_), PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
        if (st == PDH_MORE_DATA && bufSize > 0) {
            std::vector<uint8_t> buf(bufSize, 0);
            auto* items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
            if (PdhGetFormattedCounterArrayW(
                    static_cast<PDH_HCOUNTER>(pdhGpuEngine_), PDH_FMT_DOUBLE, &bufSize, &itemCount, items) == ERROR_SUCCESS) {
                for (DWORD i = 0; i < itemCount; ++i) {
                    if (items[i].FmtValue.CStatus != ERROR_SUCCESS || !items[i].szName) continue;
                    double val = items[i].FmtValue.doubleValue;
                    if (val <= 0.0) continue;

                    std::wstring inst = items[i].szName;
                    // Instance format: pid_1234_luid_0x00000000_0x00012345_phys_0_eng_0_engtype_3D
                    size_t pos = inst.find(L"luid_");
                    if (pos == std::wstring::npos) continue;
                    size_t physPos = inst.find(L"_phys_", pos);
                    if (physPos == std::wstring::npos) continue;
                    std::wstring luidKey = inst.substr(pos, physPos - pos); // e.g. luid_0x00000000_0x00012345
                    std::transform(luidKey.begin(), luidKey.end(), luidKey.begin(), ::towlower);

                    if (inst.find(L"engtype_3D") != std::wstring::npos) {
                        gpu3dByLuid[luidKey] += val;
                    } else if (inst.find(L"engtype_Copy") != std::wstring::npos) {
                        gpuCopyByLuid[luidKey] += val;
                    } else if (inst.find(L"engtype_VideoDecode") != std::wstring::npos ||
                               inst.find(L"engtype_VideoEncode") != std::wstring::npos) {
                        gpuVidByLuid[luidKey] += val;
                    }
                    gpuUtilByLuid[luidKey] = std::max({gpuUtilByLuid[luidKey],
                                                       gpu3dByLuid[luidKey],
                                                       gpuCopyByLuid[luidKey],
                                                       gpuVidByLuid[luidKey]});
                }
            }
        }
    }

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || !factory) {
        return;
    }

    UINT gpuDisplayIdx = 0;
    for (UINT idx = 0; ; ++idx) {
        IDXGIAdapter1* adapter1 = nullptr;
        if (factory->EnumAdapters1(idx, &adapter1) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter1) continue;

        DXGI_ADAPTER_DESC1 desc{};
        adapter1->GetDesc1(&desc);

        // Skip software WARP adapter (Microsoft Basic Render Driver)
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter1->Release();
            continue;
        }

        uint64_t dedUsed = 0, dedBudget = desc.DedicatedVideoMemory;
        uint64_t shrUsed = 0, shrBudget = desc.SharedSystemMemory;

        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(adapter1->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO locInfo{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &locInfo))) {
                dedUsed = locInfo.CurrentUsage;
            }
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocInfo{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocInfo))) {
                shrUsed = nonLocInfo.CurrentUsage;
            }
            adapter3->Release();
        }
        adapter1->Release();

        wchar_t luidKeyBuf[64];
        swprintf_s(luidKeyBuf, L"luid_0x%08lx_0x%08lx",
                   static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                   static_cast<unsigned long>(desc.AdapterLuid.LowPart));
        std::wstring luidKey = luidKeyBuf;

        double utilPct = std::clamp(gpuUtilByLuid[luidKey], 0.0, 100.0);
        double d3dPct  = std::clamp(gpu3dByLuid[luidKey],   0.0, 100.0);
        double copyPct = std::clamp(gpuCopyByLuid[luidKey], 0.0, 100.0);
        double vidPct  = std::clamp(gpuVidByLuid[luidKey],  0.0, 100.0);

        double vramPct = (dedBudget > 0) ? std::clamp(static_cast<double>(dedUsed) * 100.0 / static_cast<double>(dedBudget), 0.0, 100.0) : 0.0;

        SysCompInfo c{};
        c.id   = L"sys:gpu:" + luidKey;
        c.name = L"GPU " + std::to_wstring(gpuDisplayIdx++) + L" - " + std::wstring(desc.Description);
        c.type = SysCompType::GPU;
        c.shortSubtitle = L"Dedicated VRAM: " + FormatBytesSize(static_cast<double>(dedBudget));
        c.current.timestampMs = nowMs;

        c.current.hasUtil  = true;
        c.current.utilPct  = utilPct;
        c.current.hasUtil2 = (dedBudget > 0);
        c.current.util2Pct = vramPct;

        c.current.hasDataFro = true;
        c.current.dataFroVal = static_cast<double>(dedUsed);
        c.current.hasDataTo  = true;
        c.current.dataToVal  = static_cast<double>(shrUsed);
        c.dataIsBytesNotRate = true;

        GpuPerfTelemetry perf = QueryGpuPerfTelemetry(desc.AdapterLuid, desc.VendorId, desc.DeviceId);

        c.current.hasPower   = perf.hasPowerW;
        c.current.powerWatts = perf.powerWatts;
        c.current.hasTemp    = perf.hasTemp;
        c.current.tempC      = perf.tempC;
        c.powerNote = perf.hasPowerW ? perf.powerSource : L"Power wattage not exposed by GPU driver";
        c.tempNote  = perf.hasTemp   ? perf.tempSource  : L"GPU thermal sensor sleeping or not exposed";

        c.utilSummary  = FormatPercent(utilPct, true);
        c.dataSummary  = FormatBytesSize(static_cast<double>(dedUsed)) + L" / " + FormatBytesSize(static_cast<double>(dedBudget));
        c.powerSummary = FormatPowerPlain(perf.powerWatts, perf.hasPowerW);
        c.tempSummary  = FormatTempC(perf.tempC, perf.hasTemp);

        c.utilTitle       = L"GPU Utilization & VRAM vs Time (%)";
        c.utilSeries1Name = L"GPU Engine %";
        c.utilSeries2Name = L"Dedicated VRAM %";
        c.dataTitle       = L"GPU Memory Usage vs Time";
        c.dataSeries1Name = L"Shared GPU Mem";
        c.dataSeries2Name = L"Dedicated VRAM";

        wchar_t pciBuf[64];
        swprintf_s(pciBuf, L"VEN_%04X & DEV_%04X", desc.VendorId, desc.DeviceId);

        wchar_t memClkBuf[48];
        if (perf.memFreqHz > 0) swprintf_s(memClkBuf, L"%.0f MHz", static_cast<double>(perf.memFreqHz) / 1e6);
        else                    swprintf_s(memClkBuf, L"N/A");

        c.details = {
            { L"GPU Utilization",       FormatPercent(utilPct, true) },
            { L"3D Engine",             FormatPercent(d3dPct, true) },
            { L"Copy Engine",           FormatPercent(copyPct, true) },
            { L"Video Decode/Encode",   FormatPercent(vidPct, true) },
            { L"Dedicated GPU Memory",  FormatBytesSize(static_cast<double>(dedUsed)) + L" / " + FormatBytesSize(static_cast<double>(dedBudget)) },
            { L"Shared GPU Memory",     FormatBytesSize(static_cast<double>(shrUsed)) + L" / " + FormatBytesSize(static_cast<double>(shrBudget)) },
            { L"GPU Temperature",       c.tempSummary },
            { L"GPU Power",             perf.hasPowerW ? c.powerSummary : (perf.hasPowerPct ? FormatPercent(perf.powerPct, true) + L" TDP" : L"N/A") },
            { L"Memory Clock",          memClkBuf },
            { L"Fan Speed",             perf.fanRpm > 0 ? (std::to_wstring(perf.fanRpm) + L" RPM") : L"N/A" },
            { L"Hardware ID",           pciBuf }
        };

        out.push_back(std::move(c));
    }

    factory->Release();
}

// ============================================================================
// 3. PHYSICAL DISKS (Read-Only access=0 IOCTLs + NVMe/Storage Temperature)
// ============================================================================
void SystemMonitor::PollDisks(std::vector<SysCompInfo>& out, uint64_t nowMs) {
    for (int driveIdx = 0; driveIdx < 16; ++driveIdx) {
        wchar_t path[64];
        swprintf_s(path, L"\\\\.\\PhysicalDrive%d", driveIdx);

        // Strictly read-only metadata/perf access = 0 (works without admin!)
        HANDLE hDrive = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDrive == INVALID_HANDLE_VALUE) continue;

        // 1. Model & Bus Type
        std::wstring model = L"Physical Disk " + std::to_wstring(driveIdx);
        std::wstring busStr = L"Storage";
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType  = PropertyStandardQuery;
        std::vector<uint8_t> propBuf(1024, 0);
        DWORD bytesRet = 0;
        if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                            &q, sizeof(q), propBuf.data(), static_cast<DWORD>(propBuf.size()),
                            &bytesRet, nullptr)) {
            auto* desc = reinterpret_cast<PSTORAGE_DEVICE_DESCRIPTOR>(propBuf.data());
            busStr = BusTypeToString(desc->BusType);
            if (desc->ProductIdOffset > 0 && desc->ProductIdOffset < propBuf.size()) {
                std::wstring prod = AnsiToWide(reinterpret_cast<const char*>(propBuf.data() + desc->ProductIdOffset));
                if (!prod.empty()) model = prod;
            }
        }

        // 2. Capacity
        GET_LENGTH_INFORMATION lenInfo{};
        uint64_t capBytes = 0;
        if (DeviceIoControl(hDrive, IOCTL_DISK_GET_LENGTH_INFO,
                            nullptr, 0, &lenInfo, sizeof(lenInfo), &bytesRet, nullptr)) {
            capBytes = static_cast<uint64_t>(lenInfo.Length.QuadPart);
        }

        // 3. Live Disk Performance (Active %, Read/Write Speed, Response Time, IOPS)
        double readBps = 0.0, writeBps = 0.0, activePct = 0.0, avgRespMs = 0.0;
        double readIops = 0.0, writeIops = 0.0;
        DWORD queueDepth = 0;
        bool hasPerf = false;

        DISK_PERFORMANCE dp{};
        if (DeviceIoControl(hDrive, IOCTL_DISK_PERFORMANCE,
                            nullptr, 0, &dp, sizeof(dp), &bytesRet, nullptr)) {
            hasPerf = true;
            queueDepth = dp.QueueDepth;
            uint64_t rBytes = static_cast<uint64_t>(dp.BytesRead.QuadPart);
            uint64_t wBytes = static_cast<uint64_t>(dp.BytesWritten.QuadPart);
            uint64_t rCnt   = dp.ReadCount;
            uint64_t wCnt   = dp.WriteCount;
            int64_t  rTime  = dp.ReadTime.QuadPart;
            int64_t  wTime  = dp.WriteTime.QuadPart;
            int64_t  iTime  = dp.IdleTime.QuadPart;

            auto& prev = prevDisks_[driveIdx];
            if (prev.valid && nowMs > prev.tickMs) {
                double dt = static_cast<double>(nowMs - prev.tickMs) / 1000.0;
                if (rBytes >= prev.bytesRead)    readBps   = static_cast<double>(rBytes - prev.bytesRead) / dt;
                if (wBytes >= prev.bytesWritten) writeBps  = static_cast<double>(wBytes - prev.bytesWritten) / dt;
                if (rCnt   >= prev.readCount)    readIops  = static_cast<double>(rCnt - prev.readCount) / dt;
                if (wCnt   >= prev.writeCount)   writeIops = static_cast<double>(wCnt - prev.writeCount) / dt;

                // IdleTime is in 100ns units (1e7 ticks/sec)
                int64_t dIdle = iTime - prev.idleTime;
                double elapsed100ns = dt * 1e7;
                if (elapsed100ns > 0.0 && dIdle >= 0) {
                    double idleFrac = std::clamp(static_cast<double>(dIdle) / elapsed100ns, 0.0, 1.0);
                    activePct = (1.0 - idleFrac) * 100.0;
                }
                uint64_t totalOps = (rCnt >= prev.readCount ? rCnt - prev.readCount : 0) +
                                    (wCnt >= prev.writeCount ? wCnt - prev.writeCount : 0);
                int64_t totalIoTime100ns = std::max<int64_t>(0, (rTime - prev.readTime) + (wTime - prev.writeTime));
                if (totalOps > 0) {
                    avgRespMs = (static_cast<double>(totalIoTime100ns) / 10000.0) / static_cast<double>(totalOps);
                }
            }
            prev.bytesRead    = rBytes;
            prev.bytesWritten = wBytes;
            prev.readCount    = rCnt;
            prev.writeCount   = wCnt;
            prev.readTime     = rTime;
            prev.writeTime    = wTime;
            prev.idleTime     = iTime;
            prev.tickMs       = nowMs;
            prev.valid        = true;
        }

        // 4. Live Drive Temperature (NVMe / StorageDeviceTemperatureProperty)
        bool hasTemp = false;
        double tempC = 0.0;
        STORAGE_PROPERTY_QUERY tq{};
        tq.PropertyId = StorageDeviceTemperatureProperty;
        tq.QueryType  = PropertyStandardQuery;
        std::vector<uint8_t> tempBuf(512, 0);
        if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                            &tq, sizeof(tq), tempBuf.data(), static_cast<DWORD>(tempBuf.size()),
                            &bytesRet, nullptr) &&
            bytesRet >= sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR)) {
            auto* td = reinterpret_cast<PSTORAGE_TEMPERATURE_DATA_DESCRIPTOR>(tempBuf.data());
            if (td->InfoCount > 0) {
                SHORT tVal = td->TemperatureInfo[0].Temperature;
                if (tVal > 0 && tVal < 120) {
                    hasTemp = true;
                    tempC   = static_cast<double>(tVal);
                }
            }
        }

        CloseHandle(hDrive);

        SysCompInfo c{};
        c.id   = L"sys:disk:" + std::to_wstring(driveIdx);
        c.name = L"Disk " + std::to_wstring(driveIdx) + L" - " + model;
        c.type = SysCompType::Disk;
        c.shortSubtitle = busStr + (capBytes > 0 ? (L"  |  " + FormatBytesSize(static_cast<double>(capBytes))) : L"");
        c.current.timestampMs = nowMs;

        c.current.hasUtil    = hasPerf;
        c.current.utilPct    = activePct;
        c.current.hasDataTo  = hasPerf;
        c.current.dataToVal  = writeBps;
        c.current.hasDataFro = hasPerf;
        c.current.dataFroVal = readBps;
        c.dataIsBytesNotRate = false;
        c.current.hasPower   = false;
        c.current.hasTemp    = hasTemp;
        c.current.tempC      = tempC;

        c.powerNote = L"Not exposed by storage controller";
        c.tempNote  = hasTemp ? L"Live Drive SMART / NVMe Thermal Sensor" : L"Temperature sensor not exposed";

        c.utilSummary  = FormatPercent(activePct, hasPerf);
        c.dataSummary  = L"R: " + FormatBytesPerSec(readBps, hasPerf) + L"  W: " + FormatBytesPerSec(writeBps, hasPerf);
        c.powerSummary = L"N/A";
        c.tempSummary  = FormatTempC(tempC, hasTemp);

        c.utilTitle       = L"Disk Active Time vs Time (%)";
        c.utilSeries1Name = L"Active Time";
        c.dataTitle       = L"Disk Read / Write Speed vs Time";
        c.dataSeries1Name = L"Write (To)";
        c.dataSeries2Name = L"Read (Fro)";

        wchar_t respBuf[32], iopsBuf[48];
        swprintf_s(respBuf, L"%.2f ms", avgRespMs);
        swprintf_s(iopsBuf, L"R: %.0f / W: %.0f", readIops, writeIops);

        c.details = {
            { L"Active Time",           c.utilSummary },
            { L"Read Speed",            FormatBytesPerSec(readBps, hasPerf) },
            { L"Write Speed",           FormatBytesPerSec(writeBps, hasPerf) },
            { L"Avg Response Time",     respBuf },
            { L"IOPS (Read / Write)",   iopsBuf },
            { L"Queue Depth",           std::to_wstring(queueDepth) },
            { L"Capacity",              capBytes > 0 ? FormatBytesSize(static_cast<double>(capBytes)) : L"N/A" },
            { L"Bus Type",              busStr },
            { L"Temperature",           c.tempSummary }
        };

        out.push_back(std::move(c));
    }
}

// ============================================================================
// 4. MEMORY (RAM)
// ============================================================================
void SystemMonitor::PollMemory(std::vector<SysCompInfo>& out, uint64_t nowMs) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof(pi);
    GetPerformanceInfo(&pi, sizeof(pi));

    double pageBytes   = static_cast<double>(pi.PageSize > 0 ? pi.PageSize : 4096);
    double totalPhys   = static_cast<double>(ms.ullTotalPhys);
    double availPhys   = static_cast<double>(ms.ullAvailPhys);
    double inUsePhys   = std::max(0.0, totalPhys - availPhys);
    double commitTotal = static_cast<double>(pi.CommitTotal) * pageBytes;
    double commitLimit = static_cast<double>(pi.CommitLimit) * pageBytes;
    double cachedBytes = static_cast<double>(pi.SystemCache) * pageBytes;
    double pagedPool   = static_cast<double>(pi.KernelPaged) * pageBytes;
    double nonPaged    = static_cast<double>(pi.KernelNonpaged) * pageBytes;
    double memPct      = (totalPhys > 0.0) ? std::clamp(inUsePhys * 100.0 / totalPhys, 0.0, 100.0) : 0.0;
    double commitPct   = (commitLimit > 0.0) ? std::clamp(commitTotal * 100.0 / commitLimit, 0.0, 100.0) : 0.0;

    SysCompInfo c{};
    c.id   = L"sys:mem";
    c.name = L"System Memory (RAM)";
    c.type = SysCompType::Memory;
    double installedBytes = (physInstalledKb_ > 0) ? (static_cast<double>(physInstalledKb_) * 1024.0) : totalPhys;
    c.shortSubtitle = FormatBytesSize(installedBytes) + L" Installed";
    c.current.timestampMs = nowMs;

    c.current.hasUtil    = true;
    c.current.utilPct    = memPct;
    c.current.hasUtil2   = true;
    c.current.util2Pct   = commitPct;
    c.current.hasDataFro = true;
    c.current.dataFroVal = inUsePhys;
    c.current.hasDataTo  = true;
    c.current.dataToVal  = commitTotal;
    c.dataIsBytesNotRate = true;
    c.current.hasPower   = false;
    c.current.hasTemp    = false;

    c.powerNote = L"Not exposed by standard Windows APIs";
    c.tempNote  = L"Not exposed by standard Windows APIs";

    c.utilSummary  = FormatPercent(memPct, true);
    c.dataSummary  = FormatBytesSize(inUsePhys) + L" / " + FormatBytesSize(totalPhys);
    c.powerSummary = L"N/A";
    c.tempSummary  = L"N/A";

    c.utilTitle       = L"Memory & Commit Utilization vs Time (%)";
    c.utilSeries1Name = L"Physical RAM %";
    c.utilSeries2Name = L"Committed %";
    c.dataTitle       = L"Memory Usage vs Time";
    c.dataSeries1Name = L"Committed";
    c.dataSeries2Name = L"In Use";

    c.details = {
        { L"In Use",            FormatBytesSize(inUsePhys) + L" (" + FormatPercent(memPct, true) + L")" },
        { L"Available",         FormatBytesSize(availPhys) },
        { L"Total Usable RAM",  FormatBytesSize(totalPhys) },
        { L"Installed RAM",     FormatBytesSize(installedBytes) },
        { L"Committed",         FormatBytesSize(commitTotal) + L" / " + FormatBytesSize(commitLimit) },
        { L"Cached",            FormatBytesSize(cachedBytes) },
        { L"Paged Pool",        FormatBytesSize(pagedPool) },
        { L"Non-Paged Pool",    FormatBytesSize(nonPaged) }
    };

    out.push_back(std::move(c));
}

} // namespace porttool

