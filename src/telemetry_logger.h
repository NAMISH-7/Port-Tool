#pragma once

#include "port_types.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace porttool {

// ============================================================================
// LOADED HISTORICAL LOG DATA STRUCTURES (for History & Logs tab playback)
// ============================================================================

struct LogSeriesPoint {
    int64_t  epochSec   = 0;
    double   utilPct    = 0.0;
    bool     hasUtil    = false;
    double   dataToBps  = 0.0;
    bool     hasDataTo  = false;
    double   dataFroBps = 0.0;
    bool     hasDataFro = false;
    double   powerW     = 0.0;
    bool     hasPower   = false;
    double   tempC      = 0.0;
    bool     hasTemp    = false;
};

struct LogEntitySeries {
    std::wstring id;
    std::wstring name;
    bool         isPort   = false; // true = Port, false = System Hardware
    uint8_t      subType  = 0;     // PortCategory or SysCompType

    // Aggregated stats across the whole recorded file
    double   avgUtilPct    = 0.0;
    double   peakUtilPct   = 0.0;
    double   totalToBytes  = 0.0;
    double   totalFroBytes = 0.0;
    double   peakToBps     = 0.0;
    double   peakFroBps    = 0.0;
    double   avgPowerW     = 0.0;
    double   peakPowerW    = 0.0;
    double   peakTempC     = 0.0;
    bool     hasAnyUtil    = false;
    bool     hasAnyData    = false;
    bool     hasAnyPower   = false;
    bool     hasAnyTemp    = false;

    std::vector<LogSeriesPoint> points;
};

struct LoadedLogSession {
    std::wstring filePath;
    std::wstring fileName;
    int64_t      startEpochSec = 0;
    int64_t      endEpochSec   = 0;
    size_t       snapshotCount = 0;
    double       peakCpuPct    = 0.0;
    double       peakTempC     = 0.0;
    bool         hasPeakTemp   = false;
    double       peakPowerW    = 0.0;
    bool         hasPeakPower  = false;
    double       totalPortToB  = 0.0;
    double       totalPortFroB = 0.0;

    // Overview timeline (aggregate across all active ports + CPU/System)
    std::vector<LogSeriesPoint> overviewPoints;
    // Per-component / per-port series
    std::vector<LogEntitySeries> entities;
};

struct LogFileEntry {
    std::wstring fullPath;
    std::wstring fileName;
    uint64_t     sizeBytes = 0;
};

// ============================================================================
// CRASH-SAFE CONTINUOUS TELEMETRY RECORDER (Append-only, Flushed, Share-Read)
// ============================================================================
class TelemetryRecorder {
public:
    TelemetryRecorder();
    ~TelemetryRecorder();

    static std::wstring GetDefaultStorageDir();

    // Update configuration (thread-safe; takes effect immediately)
    void Configure(bool enabled, const std::wstring& folderPath, uint32_t maxHours);

    bool         IsEnabled() const { return enabled_.load(std::memory_order_relaxed); }
    bool         IsActivelyWriting() const { return activelyWriting_.load(std::memory_order_relaxed); }
    uint32_t     GetMaxHours() const { return maxHours_.load(std::memory_order_relaxed); }
    std::wstring GetStorageDir() const;
    std::wstring GetCurrentFilePath() const;

    // Called by the background polling thread; internally throttles to 2s writes + flush
    void RecordSnapshot(const std::vector<PortInfo>& ports, const std::vector<SysCompInfo>& sysComps);

    // Flush and close cleanly on exit
    void Shutdown();

    // List all .ptlog files in the given folder (newest first)
    static std::vector<LogFileEntry> ListLogFiles(const std::wstring& folderPath);

    // Parse a .ptlog file into a LoadedLogSession (safe even if currently being written!)
    static bool LoadLogFile(const std::wstring& filePath, LoadedLogSession& outSession);

private:
    bool EnsureFileOpenLocked(int64_t nowEpochSec);
    void CloseFileLocked();
    void PruneOldLogsLocked(int64_t nowEpochSec);

    mutable std::mutex mutex_;
    std::atomic<bool>     enabled_{false};
    std::atomic<bool>     activelyWriting_{false};
    std::atomic<uint32_t> maxHours_{24};

    std::wstring storageDir_;
    std::wstring currentFilePath_;
    void*        fileHandle_     = nullptr; // HANDLE
    int64_t      sessionStartSec_= 0;
    uint64_t     lastWriteTickMs_= 0;
    uint64_t     lastFlushTickMs_= 0;
};

} // namespace porttool

