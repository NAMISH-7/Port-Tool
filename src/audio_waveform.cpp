#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <ksmedia.h>

#include "audio_waveform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace porttool {

namespace {

// Strictly passive check: returns true ONLY if another process (PID != current)
// currently has an active audio session on this endpoint. Never opens or activates the mic!
bool HasExternalActiveAudioSession(IMMDevice* device) {
    if (!device) return false;
    IAudioSessionManager2* mgr = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&mgr))) || !mgr) {
        return false;
    }

    bool foundExternal = false;
    IAudioSessionEnumerator* enumerator = nullptr;
    if (SUCCEEDED(mgr->GetSessionEnumerator(&enumerator)) && enumerator) {
        int count = 0;
        enumerator->GetCount(&count);
        DWORD selfPid = GetCurrentProcessId();

        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* ctrl = nullptr;
            if (FAILED(enumerator->GetSession(i, &ctrl)) || !ctrl) continue;

            AudioSessionState state = AudioSessionStateInactive;
            if (SUCCEEDED(ctrl->GetState(&state)) && state == AudioSessionStateActive) {
                IAudioSessionControl2* ctrl2 = nullptr;
                if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                   reinterpret_cast<void**>(&ctrl2))) && ctrl2) {
                    DWORD pid = 0;
                    if (SUCCEEDED(ctrl2->GetProcessId(&pid)) && pid != 0 && pid != selfPid) {
                        foundExternal = true;
                    }
                    ctrl2->Release();
                }
            }
            ctrl->Release();
            if (foundExternal) break;
        }
        enumerator->Release();
    }

    mgr->Release();
    return foundExternal;
}

} // namespace

AudioWaveformTap::AudioWaveformTap()
    : ring_(kWaveformPoints, 0.0f) {}

AudioWaveformTap::~AudioWaveformTap() {
    Stop();
}

void AudioWaveformTap::SetTarget(const std::wstring& endpointId, bool isOutput) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (endpointId == currentEndpointId_ && isOutput == currentIsOutput_ && active_.load()) {
            return;
        }
    }

    Stop();

    if (endpointId.empty()) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentEndpointId_ = endpointId;
        currentIsOutput_   = isOutput;
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        writePos_   = 0;
        peakLinear_ = 0.0f;
        rmsLinear_  = 0.0f;
    }

    stopFlag_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&AudioWaveformTap::CaptureLoop, this, endpointId, isOutput);
}

void AudioWaveformTap::Stop() {
    stopFlag_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    active_.store(false, std::memory_order_relaxed);
    micIdleNoExternalApp_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    currentEndpointId_.clear();
}

WaveformSnapshot AudioWaveformTap::GetSnapshot() const {
    WaveformSnapshot snap{};
    std::lock_guard<std::mutex> lock(mutex_);
    snap.active               = active_.load(std::memory_order_relaxed);
    snap.isOutput             = currentIsOutput_;
    snap.micIdleNoExternalApp = micIdleNoExternalApp_.load(std::memory_order_relaxed);
    snap.peakLinear           = peakLinear_;
    snap.rmsLinear            = rmsLinear_;
    snap.samples.resize(kWaveformPoints, 0.0f);
    for (size_t i = 0; i < kWaveformPoints; ++i) {
        snap.samples[i] = ring_[(writePos_ + i) % kWaveformPoints];
    }
    return snap;
}

void AudioWaveformTap::CaptureLoop(std::wstring endpointId, bool isOutput) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator) {
        enumerator->GetDevice(endpointId.c_str(), &device);
    }

    while (device && !stopFlag_.load(std::memory_order_relaxed)) {
        // For Audio Input (Microphone): NEVER open the mic unless another app is actively using it!
        if (!isOutput && !HasExternalActiveAudioSession(device)) {
            active_.store(false, std::memory_order_relaxed);
            micIdleNoExternalApp_.store(true, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                std::fill(ring_.begin(), ring_.end(), 0.0f);
                peakLinear_ = 0.0f;
                rmsLinear_  = 0.0f;
            }
            for (int w = 0; w < 10 && !stopFlag_.load(std::memory_order_relaxed); ++w) {
                Sleep(50);
            }
            continue;
        }

        micIdleNoExternalApp_.store(false, std::memory_order_relaxed);

        IAudioClient*        client  = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX*        wfx     = nullptr;

        do {
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(&client))) || !client) {
                break;
            }
            if (FAILED(client->GetMixFormat(&wfx)) || !wfx) {
                break;
            }

            DWORD streamFlags = isOutput ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
            REFERENCE_TIME hnsDuration = 500000; // 50 ms buffer

            if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, hnsDuration, 0, wfx, nullptr))) {
                break;
            }
            if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))) || !capture) {
                break;
            }
            if (FAILED(client->Start())) {
                break;
            }

            bool isFloat32 = false;
            bool isInt16   = false;
            if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32) {
                isFloat32 = true;
            } else if (wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 16) {
                isInt16 = true;
            } else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
                auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
                if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && wfx->wBitsPerSample == 32) {
                    isFloat32 = true;
                } else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && wfx->wBitsPerSample == 16) {
                    isInt16 = true;
                }
            }

            UINT channels = std::max<UINT>(1, wfx->nChannels);
            UINT decimate = std::max<UINT>(1, wfx->nSamplesPerSec / 12000);

            active_.store(true, std::memory_order_relaxed);
            uint64_t lastSessionCheckMs = GetTickCount64();

            while (!stopFlag_.load(std::memory_order_relaxed)) {
                // For microphones, re-verify every 500ms that another app is STILL using the mic.
                // If the other app closed the mic, release immediately so Windows turns off the mic!
                if (!isOutput) {
                    uint64_t now = GetTickCount64();
                    if (now - lastSessionCheckMs >= 500) {
                        lastSessionCheckMs = now;
                        if (!HasExternalActiveAudioSession(device)) {
                            break;
                        }
                    }
                }

                UINT32 packetLen = 0;
                if (FAILED(capture->GetNextPacketSize(&packetLen))) break;

                if (packetLen == 0) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        peakLinear_ *= 0.85f;
                        rmsLinear_  *= 0.85f;
                        for (int k = 0; k < 8; ++k) {
                            ring_[writePos_] = 0.0f;
                            writePos_ = (writePos_ + 1) % kWaveformPoints;
                        }
                    }
                    Sleep(15);
                    continue;
                }

                while (packetLen > 0 && !stopFlag_.load(std::memory_order_relaxed)) {
                    BYTE*  data      = nullptr;
                    UINT32 numFrames = 0;
                    DWORD  flags     = 0;
                    if (FAILED(capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr))) {
                        packetLen = 0;
                        break;
                    }

                    float localPeak = 0.0f;
                    double sumSq = 0.0;
                    size_t count = 0;

                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
                        for (UINT32 f = 0; f < numFrames; f += decimate) {
                            float s = 0.0f;
                            if (!silent) {
                                if (isFloat32) {
                                    const float* fp = reinterpret_cast<const float*>(data) + f * channels;
                                    for (UINT ch = 0; ch < channels; ++ch) s += fp[ch];
                                    s /= static_cast<float>(channels);
                                } else if (isInt16) {
                                    const int16_t* ip = reinterpret_cast<const int16_t*>(data) + f * channels;
                                    for (UINT ch = 0; ch < channels; ++ch) s += static_cast<float>(ip[ch]) / 32768.0f;
                                    s /= static_cast<float>(channels);
                                }
                            }
                            s = std::clamp(s, -1.0f, 1.0f);
                            localPeak = std::max(localPeak, std::fabs(s));
                            sumSq += static_cast<double>(s) * static_cast<double>(s);
                            ++count;

                            ring_[writePos_] = s;
                            writePos_ = (writePos_ + 1) % kWaveformPoints;
                        }

                        if (count > 0) {
                            float rms = static_cast<float>(std::sqrt(sumSq / static_cast<double>(count)));
                            peakLinear_ = std::max(localPeak, peakLinear_ * 0.75f);
                            rmsLinear_  = rms * 0.5f + rmsLinear_ * 0.5f;
                        }
                    }

                    capture->ReleaseBuffer(numFrames);
                    if (FAILED(capture->GetNextPacketSize(&packetLen))) break;
                }
            }

            client->Stop();
        } while (false);

        active_.store(false, std::memory_order_relaxed);
        if (wfx)     CoTaskMemFree(wfx);
        if (capture) capture->Release();
        if (client)  client->Release();

        if (!isOutput) {
            // Loop back to passive wait if mic became idle
            continue;
        }
        break;
    }

    active_.store(false, std::memory_order_relaxed);
    if (device)     device->Release();
    if (enumerator) enumerator->Release();

    CoUninitialize();
}

} // namespace porttool

