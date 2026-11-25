#pragma once

#include "port_types.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

namespace porttool {

class PortMonitor {
public:
    PortMonitor();
    ~PortMonitor();

    // Poll all hardware ports (read-only) and append a new sample to history.
    void PollOnce();

    // Thread-safe snapshot of all ports for UI rendering.
    std::vector<PortInfo> GetSnapshot() const;

    // Thread-safe lookup of a single port by ID.
    bool GetPortById(const std::wstring& id, PortInfo& outPort) const;

private:
    struct ByteCounterPrev {
        uint64_t bytesTo   = 0;
        uint64_t bytesFro  = 0;
        uint64_t tickMs    = 0;
        bool     valid     = false;
    };

    struct UsbDiskTraffic {
        std::wstring productHint; // lowercase product/serial hint for matching
        double       readBps  = 0.0; // Fro (from USB drive to PC)
        double       writeBps = 0.0; // To  (from PC to USB drive)
        bool         used     = false;
    };

    void PollPowerPorts(std::vector<PortInfo>& out, uint64_t nowSec);
    void PollNetworkPorts(std::vector<PortInfo>& out, uint64_t nowSec, uint64_t nowMs);
    void PollUsbPorts(std::vector<PortInfo>& out, uint64_t nowSec, uint64_t nowMs);
    void PollDisplayPorts(std::vector<PortInfo>& out, uint64_t nowSec);
    void PollAudioPorts(std::vector<PortInfo>& out, uint64_t nowSec);
    void PollSerialPorts(std::vector<PortInfo>& out, uint64_t nowSec);

    std::vector<UsbDiskTraffic> CollectUsbDiskTraffic(uint64_t nowMs);

    void EnumerateUsbHub(
        const std::wstring& hubPath,
        const std::wstring& hubLabel,
        int depth,
        std::vector<PortInfo>& out,
        std::vector<UsbDiskTraffic>& usbDisks,
        uint64_t nowSec,
        int& globalPortCounter
    );

    void InitPowerCounters();
    void ShutdownPowerCounters();
    void SamplePowerMeters(double& outAcpiMeterWatts, double& outPkgWatts);

    mutable std::mutex mutex_;
    std::vector<std::wstring> portOrder_;
    std::unordered_map<std::wstring, PortInfo> ports_;
    std::unordered_map<std::wstring, ByteCounterPrev> prevCounters_;
    uint64_t startTickMs_ = 0;

    // Read-only PDH handles for ACPI Power Meter & Hardware Energy Meter (RAPL)
    void* pdhQuery_     = nullptr;
    void* pdhAcpiPower_ = nullptr;
    void* pdhRaplPkg_   = nullptr;
    bool  pdhPrimed_    = false;
};

} // namespace porttool

