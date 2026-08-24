/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "stdafx.h"
#include "AudioSettingsProp.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main()
{
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
    return 0;
}
