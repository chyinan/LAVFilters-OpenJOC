/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#pragma once

#include "OpenJocAdmission.h"
#include "OpenJocOutput.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct LAVOpenJocFrame
{
    std::vector<float> samples;
    std::uint32_t sample_rate = 0;
    std::uint32_t channel_count = 0;
    std::size_t sample_count = 0;
    std::int64_t pts_samples = INT64_MIN;
    const LAVOpenJocOutputContract *output_contract = nullptr;
};

enum class LAVOpenJocProcessResult
{
    Waiting,
    UseStockDecoder,
    OpenJoc,
    Error,
};

class LAVOpenJocDecoder final
{
  public:
    LAVOpenJocDecoder();
    ~LAVOpenJocDecoder();

    LAVOpenJocDecoder(const LAVOpenJocDecoder &) = delete;
    LAVOpenJocDecoder &operator=(const LAVOpenJocDecoder &) = delete;

    bool IsAvailable() const;
    LAVOpenJocState State() const;
    bool SetOutputPolicy(LAVOpenJocOutputPolicy policy);
    bool SetDialnormPolicy(LAVOpenJocDialnormPolicy policy);
    const LAVOpenJocOutputContract *OutputContract() const;
    LAVOpenJocDialnormPolicy DialnormPolicy() const;
    LAVOpenJocProcessResult Process(const unsigned char *data, std::size_t data_size, std::int64_t pts_samples,
                                    bool end_of_stream);
    bool ReceiveFrame(LAVOpenJocFrame &frame);
    bool Drain();
    void Reset();

    bool HasError() const;
    const char *LastError() const;
    std::size_t ClassifierInputBytes() const;
    std::size_t StreamInputBytes() const;

#if defined(LAV_OPENJOC_TESTING)
    void FailNextClassifierCreateForTesting();
    void FailNextDecoderCreateForTesting();
    void FailNextClassifierResetForTesting();
    const char *ConfigDescriptorForTesting() const;
#endif

  private:
    bool SetConfiguration(const LAVOpenJocOutputContract *contract, LAVOpenJocDialnormPolicy dialnorm_policy);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
