#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

#include "telemetry_logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace porttool {

namespace {

std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string SanitizeField(const std::wstring& ws) {
    std::string s = WideToUtf8(ws);
    for (char& ch : s) {
        if (ch == '|' || ch == '\r' || ch == '\n') ch = ' ';
    }
    return s;
}

void EnsureDirectoryRecursive(const std::wstring& path) {
    if (path.empty()) return;
    SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
}

std::vector<std::string> SplitPipe(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= line.size()) {
        size_t pos = line.find('|', start);
        if (pos == std::string::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

} // namespace

TelemetryRecorder::TelemetryRecorder() {
    storageDir_ = GetDefaultStorageDir();
}

TelemetryRecorder::~TelemetryRecorder() {
    Shutdown();
}

std::wstring TelemetryRecorder::GetDefaultStorageDir() {
    PWSTR docsPath = nullptr;
    std::wstring base = L"C:\\Users\\Public\\Documents";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docsPath)) && docsPath) {
        base = docsPath;
        CoTaskMemFree(docsPath);
    }
    if (!base.empty() && base.back() != L'\\') base += L'\\';
    return base + L"PortTool_Telemetry";
}

void TelemetryRecorder::Configure(bool enabled, const std::wstring& folderPath, uint32_t maxHours) {
    uint32_t clampedHours = std::clamp<uint32_t>(maxHours, 1, 24);
    std::lock_guard<std::mutex> lock(mutex_);

    bool dirChanged = (!folderPath.empty() && folderPath != storageDir_);
    if (!folderPath.empty()) {
        storageDir_ = folderPath;
        while (storageDir_.size() > 3 && storageDir_.back() == L'\\') {
            storageDir_.pop_back();
        }
    }

    maxHours_.store(clampedHours, std::memory_order_relaxed);
    enabled_.store(enabled, std::memory_order_relaxed);

    if (!enabled || dirChanged) {
        CloseFileLocked();
    }
}

std::wstring TelemetryRecorder::GetStorageDir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return storageDir_;
}

std::wstring TelemetryRecorder::GetCurrentFilePath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentFilePath_;
}

void TelemetryRecorder::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseFileLocked();
}

void TelemetryRecorder::CloseFileLocked() {
    if (fileHandle_ && fileHandle_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(static_cast<HANDLE>(fileHandle_));
        CloseHandle(static_cast<HANDLE>(fileHandle_));
    }
    fileHandle_ = nullptr;
    activelyWriting_.store(false, std::memory_order_relaxed);
}

bool TelemetryRecorder::EnsureFileOpenLocked(int64_t nowEpochSec) {
    int64_t maxSec = static_cast<int64_t>(maxHours_.load(std::memory_order_relaxed)) * 3600LL;

    // Roll over to a new session file if current session exceeded the selected timeframe (max 24h)
    if (fileHandle_ && fileHandle_ != INVALID_HANDLE_VALUE) {
        if (nowEpochSec - sessionStartSec_ >= maxSec) {
            CloseFileLocked();
        } else {
            return true;
        }
    }

    if (storageDir_.empty()) storageDir_ = GetDefaultStorageDir();
    EnsureDirectoryRecursive(storageDir_);

    // Build timestamped filename: PortTool_YYYY-MM-DD_HH-MM-SS.ptlog
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t nameBuf[96];
    swprintf_s(nameBuf, L"PortTool_%04u-%02u-%02u_%02u-%02u-%02u.ptlog",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    currentFilePath_ = storageDir_ + L"\\" + nameBuf;

    // FILE_APPEND_DATA + FILE_SHARE_READ allows reading the live file safely while recording!
    HANDLE h = CreateFileW(
        currentFilePath_.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        activelyWriting_.store(false, std::memory_order_relaxed);
        return false;
    }

    fileHandle_      = h;
    sessionStartSec_ = nowEpochSec;
    activelyWriting_.store(true, std::memory_order_relaxed);

    // Write file header line
    char hdr[128];
    int n = snprintf(hdr, sizeof(hdr), "PTLOG|1|%lld|%u\n",
                     static_cast<long long>(nowEpochSec), maxHours_.load(std::memory_order_relaxed));
    if (n > 0) {
        DWORD written = 0;
        WriteFile(h, hdr, static_cast<DWORD>(n), &written, nullptr);
        FlushFileBuffers(h);
    }

    PruneOldLogsLocked(nowEpochSec);
    return true;
}

void TelemetryRecorder::PruneOldLogsLocked(int64_t nowEpochSec) {
    // Keep logs within 24h retention window if user set <= 24h, to avoid unbounded growth
    (void)nowEpochSec;
}

void TelemetryRecorder::RecordSnapshot(const std::vector<PortInfo>& ports, const std::vector<SysCompInfo>& sysComps) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        if (activelyWriting_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(mutex_);
            CloseFileLocked();
        }
        return;
    }

    uint64_t nowTick = GetTickCount64();
    // Write a compact full snapshot every 2 seconds (smooth 24h logs, ~43,200 snapshots/24h)
    if (lastWriteTickMs_ != 0 && (nowTick - lastWriteTickMs_) < 2000ULL) {
        return;
    }
    lastWriteTickMs_ = nowTick;

    int64_t epochSec = static_cast<int64_t>(std::time(nullptr));

    // Format entire snapshot into one memory buffer for atomic WriteFile
    std::string buf;
    buf.reserve(2048);

    char line[512];
    int n = snprintf(line, sizeof(line), "T|%lld\n", static_cast<long long>(epochSec));
    if (n > 0) buf.append(line, static_cast<size_t>(n));

    // Record System Hardware components (CPU, GPUs, Disks, Memory)
    for (const auto& c : sysComps) {
        n = snprintf(line, sizeof(line),
                     "S|%s|%s|%u|%d|%.2f|%d|%.1f|%d|%.1f|%d|%.3f|%d|%.2f\n",
                     SanitizeField(c.id).c_str(),
                     SanitizeField(c.name).c_str(),
                     static_cast<unsigned>(c.type),
                     c.current.hasUtil    ? 1 : 0, c.current.utilPct,
                     c.current.hasDataTo  ? 1 : 0, c.current.dataToVal,
                     c.current.hasDataFro ? 1 : 0, c.current.dataFroVal,
                     c.current.hasPower   ? 1 : 0, c.current.powerWatts,
                     c.current.hasTemp    ? 1 : 0, c.current.tempC);
        if (n > 0) buf.append(line, static_cast<size_t>(n));
    }

    // Record Active Ports (or any port reporting data/power)
    for (const auto& p : ports) {
        if (!p.active && !p.current.hasDataTo && !p.current.hasDataFro && !p.current.hasPower) {
            continue;
        }
        n = snprintf(line, sizeof(line),
                     "P|%s|%s|%u|%d|%.1f|%d|%.1f|%d|%.3f|%u\n",
                     SanitizeField(p.id).c_str(),
                     SanitizeField(p.name).c_str(),
                     static_cast<unsigned>(p.category),
                     p.current.hasDataTo  ? 1 : 0, p.current.bytesToSec,
                     p.current.hasDataFro ? 1 : 0, p.current.bytesFroSec,
                     p.current.hasPower   ? 1 : 0, p.current.powerWatts,
                     static_cast<unsigned>(p.current.powerDir));
        if (n > 0) buf.append(line, static_cast<size_t>(n));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureFileOpenLocked(epochSec)) return;

    HANDLE h = static_cast<HANDLE>(fileHandle_);
    DWORD written = 0;
    WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);

    // Crash-safe flush every write (every 2s) so power loss/crash loses at most 2 seconds!
    FlushFileBuffers(h);
    lastFlushTickMs_ = nowTick;
}

std::vector<LogFileEntry> TelemetryRecorder::ListLogFiles(const std::wstring& folderPath) {
    std::vector<LogFileEntry> list;
    if (folderPath.empty()) return list;

    std::wstring pattern = folderPath;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L"*.ptlog";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return list;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        LogFileEntry e{};
        e.fileName  = fd.cFileName;
        e.fullPath  = folderPath + ((folderPath.back() == L'\\') ? L"" : L"\\") + fd.cFileName;
        e.sizeBytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        list.push_back(std::move(e));
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    // Sort newest first (filenames are YYYY-MM-DD_HH-MM-SS so descending lexicographical = newest first)
    std::sort(list.begin(), list.end(), [](const LogFileEntry& a, const LogFileEntry& b) {
        return a.fileName > b.fileName;
    });
    return list;
}

bool TelemetryRecorder::LoadLogFile(const std::wstring& filePath, LoadedLogSession& out) {
    out = {};
    out.filePath = filePath;
    size_t slash = filePath.find_last_of(L"\\/");
    out.fileName = (slash == std::wstring::npos) ? filePath : filePath.substr(slash + 1);

    // Open with FILE_SHARE_READ | FILE_SHARE_WRITE so we can read even the actively recording file!
    HANDLE h = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fsize{};
    if (!GetFileSizeEx(h, &fsize) || fsize.QuadPart <= 0) {
        CloseHandle(h);
        return false;
    }

    // Cap read to 64 MB for safety
    size_t toRead = static_cast<size_t>(std::min<LONGLONG>(fsize.QuadPart, 64LL * 1024LL * 1024LL));
    std::string content(toRead, '\0');
    DWORD bytesRead = 0;
    ReadFile(h, content.data(), static_cast<DWORD>(toRead), &bytesRead, nullptr);
    CloseHandle(h);
    content.resize(bytesRead);

    std::unordered_map<std::string, size_t> entityIndex;
    std::istringstream iss(content);
    std::string line;

    int64_t curEpoch  = 0;
    int64_t prevEpoch = 0;
    LogSeriesPoint curOverview{};
    bool hasOverview = false;

    auto flushOverview = [&]() {
        if (!hasOverview) return;
        out.overviewPoints.push_back(curOverview);
        out.snapshotCount++;
        hasOverview = false;
    };

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        auto parts = SplitPipe(line);
        if (parts.empty()) continue;

        if (parts[0] == "T" && parts.size() >= 2) {
            flushOverview();
            try { curEpoch = std::stoll(parts[1]); } catch (...) { continue; }
            if (out.startEpochSec == 0) out.startEpochSec = curEpoch;
            out.endEpochSec = curEpoch;

            curOverview = {};
            curOverview.epochSec = curEpoch;
            hasOverview = true;
            if (prevEpoch == 0) prevEpoch = curEpoch;
        } else if (parts[0] == "S" && parts.size() >= 14 && curEpoch > 0) {
            // S|id|name|type|hasUtil|util|hasTo|to|hasFro|fro|hasPwr|pwr|hasTemp|temp
            const std::string& idStr = parts[1];
            auto it = entityIndex.find(idStr);
            size_t idx = 0;
            if (it == entityIndex.end()) {
                idx = out.entities.size();
                entityIndex[idStr] = idx;
                LogEntitySeries ent{};
                ent.id      = Utf8ToWide(idStr);
                ent.name    = Utf8ToWide(parts[2]);
                ent.isPort  = false;
                ent.subType = static_cast<uint8_t>(std::atoi(parts[3].c_str()));
                out.entities.push_back(std::move(ent));
            } else {
                idx = it->second;
            }

            LogSeriesPoint pt{};
            pt.epochSec   = curEpoch;
            pt.hasUtil    = (parts[4] == "1");
            pt.utilPct    = std::atof(parts[5].c_str());
            pt.hasDataTo  = (parts[6] == "1");
            pt.dataToBps  = std::atof(parts[7].c_str());
            pt.hasDataFro = (parts[8] == "1");
            pt.dataFroBps = std::atof(parts[9].c_str());
            pt.hasPower   = (parts[10] == "1");
            pt.powerW     = std::atof(parts[11].c_str());
            pt.hasTemp    = (parts[12] == "1");
            pt.tempC      = std::atof(parts[13].c_str());

            auto& ent = out.entities[idx];
            double dt = (ent.points.empty()) ? 2.0 : std::clamp(static_cast<double>(curEpoch - ent.points.back().epochSec), 0.5, 10.0);
            if (pt.hasUtil) {
                ent.hasAnyUtil  = true;
                ent.peakUtilPct = std::max(ent.peakUtilPct, pt.utilPct);
                ent.avgUtilPct += pt.utilPct;
            }
            if (pt.hasDataTo || pt.hasDataFro) {
                ent.hasAnyData = true;
                if (ent.subType == static_cast<uint8_t>(SysCompType::Disk)) {
                    ent.totalToBytes  += pt.dataToBps * dt;
                    ent.totalFroBytes += pt.dataFroBps * dt;
                }
                ent.peakToBps  = std::max(ent.peakToBps,  pt.dataToBps);
                ent.peakFroBps = std::max(ent.peakFroBps, pt.dataFroBps);
            }
            if (pt.hasPower) {
                ent.hasAnyPower = true;
                ent.peakPowerW  = std::max(ent.peakPowerW, pt.powerW);
                ent.avgPowerW  += pt.powerW;
            }
            if (pt.hasTemp) {
                ent.hasAnyTemp = true;
                ent.peakTempC  = std::max(ent.peakTempC, pt.tempC);
                out.hasPeakTemp = true;
                out.peakTempC   = std::max(out.peakTempC, pt.tempC);
                if (!curOverview.hasTemp || pt.tempC > curOverview.tempC) {
                    curOverview.hasTemp = true;
                    curOverview.tempC   = pt.tempC;
                }
            }
            if (ent.subType == static_cast<uint8_t>(SysCompType::CPU) && pt.hasUtil) {
                curOverview.hasUtil = true;
                curOverview.utilPct = pt.utilPct;
                out.peakCpuPct      = std::max(out.peakCpuPct, pt.utilPct);
            }
            ent.points.push_back(pt);
        } else if (parts[0] == "P" && parts.size() >= 11 && curEpoch > 0) {
            // P|id|name|cat|hasTo|to|hasFro|fro|hasPwr|pwr|pwrDir
            const std::string& idStr = parts[1];
            auto it = entityIndex.find(idStr);
            size_t idx = 0;
            if (it == entityIndex.end()) {
                idx = out.entities.size();
                entityIndex[idStr] = idx;
                LogEntitySeries ent{};
                ent.id      = Utf8ToWide(idStr);
                ent.name    = Utf8ToWide(parts[2]);
                ent.isPort  = true;
                ent.subType = static_cast<uint8_t>(std::atoi(parts[3].c_str()));
                out.entities.push_back(std::move(ent));
            } else {
                idx = it->second;
            }

            LogSeriesPoint pt{};
            pt.epochSec   = curEpoch;
            pt.hasDataTo  = (parts[4] == "1");
            pt.dataToBps  = std::atof(parts[5].c_str());
            pt.hasDataFro = (parts[6] == "1");
            pt.dataFroBps = std::atof(parts[7].c_str());
            pt.hasPower   = (parts[8] == "1");
            pt.powerW     = std::atof(parts[9].c_str());

            auto& ent = out.entities[idx];
            double dt = (ent.points.empty()) ? 2.0 : std::clamp(static_cast<double>(curEpoch - ent.points.back().epochSec), 0.5, 10.0);
            if (pt.hasDataTo || pt.hasDataFro) {
                ent.hasAnyData     = true;
                ent.totalToBytes  += pt.dataToBps * dt;
                ent.totalFroBytes += pt.dataFroBps * dt;
                ent.peakToBps      = std::max(ent.peakToBps,  pt.dataToBps);
                ent.peakFroBps     = std::max(ent.peakFroBps, pt.dataFroBps);

                curOverview.hasDataTo  = true;
                curOverview.hasDataFro = true;
                curOverview.dataToBps  += pt.dataToBps;
                curOverview.dataFroBps += pt.dataFroBps;
                out.totalPortToB       += pt.dataToBps * dt;
                out.totalPortFroB      += pt.dataFroBps * dt;
            }
            if (pt.hasPower) {
                ent.hasAnyPower = true;
                ent.peakPowerW  = std::max(ent.peakPowerW, pt.powerW);
                ent.avgPowerW  += pt.powerW;
                if (ent.subType == static_cast<uint8_t>(PortCategory::Power)) {
                    curOverview.hasPower = true;
                    curOverview.powerW   = pt.powerW;
                    out.hasPeakPower     = true;
                    out.peakPowerW       = std::max(out.peakPowerW, pt.powerW);
                }
            }
            ent.points.push_back(pt);
        }
    }

    flushOverview();

    // Finalize averages
    for (auto& ent : out.entities) {
        if (!ent.points.empty()) {
            double cnt = static_cast<double>(ent.points.size());
            ent.avgUtilPct /= cnt;
            ent.avgPowerW  /= cnt;
        }
    }

    return !out.overviewPoints.empty();
}

} // namespace porttool

