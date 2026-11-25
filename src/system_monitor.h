#pragma once

#include "port_types.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

namespace porttool {

class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor();

    // Poll CPU, GPUs, Disks, and Memory (strictly read-only) and append to 0.5s history.
    void PollOnce();

    std::vector<SysCompInfo> GetSnapshot() const;

private:
    struct DiskPrev {
        uint64_t bytesRead    = 0;
        uint64_t bytesWritten = 0;
        uint64_t readCount    = 0;
        uint64_t writeCount   = 0;
        int64_t  readTime     = 0;
        int64_t  writeTime    = 0;
        int64_t  idleTime     = 0;
        uint64_t tickMs       = 0;
        bool     valid        = false;
    };

    void InitStaticHardwareInfo();
    void InitPdhCounters();
    void ShutdownPdhCounters();

    void PollCpu(std::vector<SysCompInfo>& out, uint64_t nowMs);
    void PollGpus(std::vector<SysCompInfo>& out, uint64_t nowMs);
    void PollDisks(std::vector<SysCompInfo>& out, uint64_t nowMs);
    void PollMemory(std::vector<SysCompInfo>& out, uint64_t nowMs);

    // Static CPU info cached once (read-only)
    std::wstring cpuName_        = L"Processor";
    uint32_t     cpuBaseMhz_     = 0;
    uint32_t     cpuPhysCores_   = 0;
    uint32_t     cpuLogProcs_    = 0;
    uint32_t     cpuSockets_     = 1;
    uint64_t     cpuL1Bytes_     = 0;
    uint64_t     cpuL2Bytes_     = 0;
    uint64_t     cpuL3Bytes_     = 0;
    bool         cpuVirtEnabled_ = false;
    uint64_t     physInstalledKb_= 0;

    // CPU GetSystemTimes delta fallback
    uint64_t prevIdle_   = 0;
    uint64_t prevKernel_ = 0;
    uint64_t prevUser_   = 0;
    bool     hasPrevCpu_ = false;

    // Read-only PDH query & counters
    void* pdhQuery_     = nullptr;
    void* pdhCpuUtil_   = nullptr;
    void* pdhCpuPerf_   = nullptr;
    void* pdhCpuPriv_   = nullptr;
    void* pdhRaplPkg_   = nullptr;
    void* pdhThermals_  = nullptr;
    void* pdhGpuEngine_ = nullptr;

    std::unordered_map<int, DiskPrev> prevDisks_;

    mutable std::mutex mutex_;
    std::vector<std::wstring> compOrder_;
    std::unordered_map<std::wstring, SysCompInfo> comps_;
    uint64_t startTickMs_ = 0;
};

} // namespace porttool

