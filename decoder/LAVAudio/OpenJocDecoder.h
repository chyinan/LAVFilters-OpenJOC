/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#pragma once

#include "OpenJocAdmission.h"
#include "OpenJocBinauralSettings.h"
#include "OpenJocDiagnostic.h"
#include "OpenJocOutput.h"

#if defined(LAV_ENABLE_OPENJOC)
#include "openjoc.h"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
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
    bool SetBinauralConfiguration(const LAVOpenJocOutputContract *contract,
                                  LAVOpenJocDialnormPolicy dialnorm_policy,
                                  LAVOpenJocHrtfSource hrtf_source,
                                  std::vector<unsigned char> sofa_data,
                                  std::string virtual_layout);
    bool SetDialnormPolicy(LAVOpenJocDialnormPolicy policy);
    const LAVOpenJocOutputContract *OutputContract() const;
    LAVOpenJocDialnormPolicy DialnormPolicy() const;
    LAVOpenJocProcessResult Process(const unsigned char *data, std::size_t data_size, std::int64_t pts_samples,
                                    bool end_of_stream);
    bool ReceiveFrame(LAVOpenJocFrame &frame);
    bool Drain();
    void Reset();
    void ResetForNewStream();

    bool HasError() const;
    const char *LastError() const;
    void ClearTransientError();
    void SetConfigurationError(const char *detail);
    [[nodiscard]] LAVOpenJocDiagnosticSnapshot DiagnosticSnapshot() const;
    void RecordRuntimeDiagnostic(LAVOpenJocFailureReason reason, const char *detail);
    std::size_t ClassifierInputBytes() const;
    std::size_t StreamInputBytes() const;
#if defined(LAV_ENABLE_OPENJOC)
    bool GetLiveInspectionSnapshot(openjoc_live_inspection_snapshot *snapshot) const;
    bool CopyLiveInspectionJson(char *output, std::size_t output_capacity,
                                std::size_t *required_size) const;
#endif

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
    mutable std::recursive_mutex m_mutex;
};
