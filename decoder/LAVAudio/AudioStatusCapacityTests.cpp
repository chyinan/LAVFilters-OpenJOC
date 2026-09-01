/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#include "stdafx.h"
#include "AudioSettingsProp.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

static void TestStatusLockProbeDoesNotWait()
{
    CCritSec receive_lock;
    std::atomic<bool> holder_ready{false};
    std::atomic<bool> release_holder{false};
    std::thread holder([&]() {
        CAutoLock lock(&receive_lock);
        holder_ready.store(true, std::memory_order_release);
        while (!release_holder.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });

    while (!holder_ready.load(std::memory_order_acquire))
        std::this_thread::yield();

    const auto begin = std::chrono::steady_clock::now();
    CAutoTryLock status_lock(&receive_lock);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    assert(!status_lock.IsLocked());
    assert(elapsed < std::chrono::milliseconds(50));

    release_holder.store(true, std::memory_order_release);
    holder.join();
}

int main()
{
    TestStatusLockProbeDoesNotWait();
    static_assert(LAVAudioStatusMeterCount(0) == 0);
    static_assert(LAVAudioStatusMeterCount(8) == 8);
    static_assert(LAVAudioStatusMeterCount(10) == 8);
    static_assert(LAVAudioStatusMeterCount(12) == 8);

    const std::filesystem::path source_path = std::filesystem::path(__FILE__).parent_path() / "AudioSettingsProp.cpp";
    std::ifstream source_file(source_path, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(source_file)), std::istreambuf_iterator<char>());
    assert(source.find("m_nMeterChannels = LAVAudioStatusMeterCount(nOutputChannels);") != std::string::npos);
    assert(source.find("L\"%d / 0x%x\", nOutputChannels, dwOutputChannelMask") != std::string::npos);
    assert(source.find("for (int i = 0; i < m_nMeterChannels; ++i)") != std::string::npos);
    assert(source.find("case WM_TIMER:") != std::string::npos);
    assert(source.find("UpdateVolumeDisplay();") != std::string::npos);
    assert(source.find("GetOutputPolicy(&policy) == S_OK") != std::string::npos);
    assert(source.find("GetOpenJocPlaybackDiagnostics(") != std::string::npos);

    const std::filesystem::path lav_audio_path = std::filesystem::path(__FILE__).parent_path() / "LAVAudio.cpp";
    std::ifstream lav_audio_file(lav_audio_path, std::ios::binary);
    const std::string lav_audio_source((std::istreambuf_iterator<char>(lav_audio_file)),
                                       std::istreambuf_iterator<char>());
    assert(lav_audio_source.find("CAutoTryLock receive_lock(&m_csReceive);") != std::string::npos);
    assert(lav_audio_source.find("m_openJocAdmissionSnapshot.load") != std::string::npos);
    assert(lav_audio_source.find("CAutoLock volume_lock(&m_csVolumeStats);") != std::string::npos);
    return 0;
}
