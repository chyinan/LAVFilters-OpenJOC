/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocShippedLayouts.h"
#include "OpenJocOutput.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(const int argc, char **argv)
{
    std::size_t count = 0;
    const LAVOpenJocOutputPolicy *policies = GetLAVOpenJocShippedOutputPolicies(&count);
    assert(policies != nullptr);

    if (argc == 2 && std::strcmp(argv[1], "--list-shipped") == 0)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policies[index]);
            assert(contract != nullptr);
            assert(contract->property_page_label != nullptr);
            std::cout << contract->property_page_label << '\n';
        }
        return 0;
    }
    if (argc != 1)
        return 64;

    constexpr LAVOpenJocOutputPolicy expected[] = {
        LAVOpenJocOutputPolicy::Stereo,
        LAVOpenJocOutputPolicy::Binaural,
        LAVOpenJocOutputPolicy::Layout51,
        LAVOpenJocOutputPolicy::Layout71,
        LAVOpenJocOutputPolicy::Layout512,
        LAVOpenJocOutputPolicy::Layout514,
        LAVOpenJocOutputPolicy::Layout712,
        LAVOpenJocOutputPolicy::Layout714,
    };
    assert(count == LAV_OPENJOC_OUTPUT_CONTRACT_COUNT);
    for (std::size_t index = 0; index < count; ++index)
        assert(policies[index] == expected[index]);
    for (std::uint32_t value = 0; value < LAV_OPENJOC_OUTPUT_CONTRACT_COUNT; ++value)
        assert(IsLAVOpenJocOutputPolicyShipped(static_cast<LAVOpenJocOutputPolicy>(value)));
    for (std::size_t index = 0; index < count; ++index)
        assert(FindLAVOpenJocOutputContract(policies[index]) != nullptr);

    const std::filesystem::path source_path = std::filesystem::path(__FILE__).parent_path() / "AudioSettingsProp.cpp";
    std::ifstream source_file(source_path, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(source_file)), std::istreambuf_iterator<char>());
    assert(source.find("CB_SETITEMDATA") != std::string::npos);
    assert(source.find("CB_GETITEMDATA") != std::string::npos);
    assert(source.find("CLAVAudioOpenJocProp::OnActivate") != std::string::npos);
    assert(source.find("selected_output == CB_ERR") != std::string::npos);
    assert(source.find("SetOutputPolicy") != std::string::npos);
    const auto settings_apply = source.find("HRESULT CLAVAudioSettingsProp::OnApplyChanges()");
    const auto openjoc_page = source.find("CLAVAudioOpenJocProp::CLAVAudioOpenJocProp");
    assert(settings_apply != std::string::npos && openjoc_page != std::string::npos && settings_apply < openjoc_page);
    assert(source.substr(settings_apply, openjoc_page - settings_apply).find("SetOutputPolicy") == std::string::npos);
    assert(source.find("ParseOpenJoc") == std::string::npos);
    assert(source.find("endpoint") == std::string::npos);
    assert(source.find("product name") == std::string::npos);
    assert(source.find("UpdateOpenJocStatusDisplay();") != std::string::npos);
    assert(source.find("case WM_TIMER:") != std::string::npos);
    return 0;
}
