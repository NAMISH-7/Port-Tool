#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace porttool {

constexpr size_t kWaveformPoints = 512;

struct WaveformSnapshot {
    bool               active = false;
    bool               isOutput = true;
    bool               micIdleNoExternalApp = false; // True when mic is unused by any other app (kept OFF)
    float              peakLinear = 0.0f; // 0.0 .. 1.0
    float              rmsLinear  = 0.0f; // 0.0 .. 1.0
    std::vector<float> samples;           // kWaveformPoints in [-1.0, +1.0]
};

class AudioWaveformTap {
public:
    AudioWaveformTap();
    ~AudioWaveformTap();

    // Start capturing from the given MMDevice endpoint ID (read-only WASAPI shared/loopback).
    // Pass empty endpointId to stop capturing.
    void SetTarget(const std::wstring& endpointId, bool isOutput);
    void Stop();

    WaveformSnapshot GetSnapshot() const;
    bool IsCapturing() const { return active_.load(std::memory_order_relaxed); }

private:
    void CaptureLoop(std::wstring endpointId, bool isOutput);

    mutable std::mutex mutex_;
    std::wstring       currentEndpointId_;
    bool               currentIsOutput_ = true;
    std::vector<float> ring_;
    size_t             writePos_   = 0;
    float              peakLinear_ = 0.0f;
    float              rmsLinear_  = 0.0f;

    std::atomic<bool>  stopFlag_{false};
    std::atomic<bool>  active_{false};
    std::atomic<bool>  micIdleNoExternalApp_{false};
    std::thread        thread_;
};

} // namespace porttool

