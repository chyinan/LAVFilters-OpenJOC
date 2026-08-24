/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "OpenJocShippedLayouts.h"
#include "OpenJocOutput.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main()
{
    std::size_t count = 0;
    const LAVOpenJocOutputPolicy *policies = GetLAVOpenJocShippedOutputPolicies(&count);
    assert(policies != nullptr);
    assert(count == 1);
    assert(policies[0] == LAVOpenJocOutputPolicy::Stereo);
    assert(IsLAVOpenJocOutputPolicyShipped(LAVOpenJocOutputPolicy::Stereo));
    for (std::uint32_t value = 1; value <= 6; ++value)
        assert(!IsLAVOpenJocOutputPolicyShipped(static_cast<LAVOpenJocOutputPolicy>(value)));
    for (std::size_t index = 0; index < count; ++index)
        assert(FindLAVOpenJocOutputContract(policies[index]) != nullptr);

    const std::filesystem::path source_path = std::filesystem::path(__FILE__).parent_path() / "AudioSettingsProp.cpp";
    std::ifstream source_file(source_path, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(source_file)), std::istreambuf_iterator<char>());
    assert(source.find("CB_SETITEMDATA") != std::string::npos);
    assert(source.find("CB_GETITEMDATA") != std::string::npos);
    assert(source.find("selected_index != CB_ERR") != std::string::npos);
    assert(source.find("SetOutputPolicy") != std::string::npos);
    assert(source.find("ParseOpenJoc") == std::string::npos);
    assert(source.find("endpoint") == std::string::npos);
    assert(source.find("product name") == std::string::npos);
    assert(source.find("UpdateOpenJocStatusDisplay();") != std::string::npos);
    assert(source.find("case WM_TIMER:") != std::string::npos);
    return 0;
}
