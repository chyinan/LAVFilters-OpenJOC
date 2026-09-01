/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Controlled-sink evidence is deliberately distinct from renderer support.
// CONTROLLED_SINK_COMPLETE proves this exact private graph and capture sink;
// only a later named-renderer run may classify renderer support.

// pattern: Mixed (needs refactoring)

#include <windows.h>

#include "streams.h"

#include <bcrypt.h>
#include <audioclient.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <psapi.h>
#include <propsys.h>

#include "OpenJocDecoder.h"
#include "OpenJocStrictNegotiation.h"
#include "OpenJocStrictOutput.h"
#include "LAVAudioSettings.h"
#include "LAVOpenJocDiagnostics.h"
#include "LAVSplitterSettings.h"
#include "ISpecifyPropertyPages2.h"
#include "resource.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr GUID kTargetLavAudio = {
    0x27247580, 0xc701, 0x40cd, {0x88, 0x6d, 0xe6, 0x18, 0xfc, 0x8c, 0x9f, 0xff}};
constexpr GUID kPristineLavAudio = {
    0xe8e73b6b, 0x4cb3, 0x44a4, {0xbe, 0x99, 0x4f, 0x7b, 0xcb, 0x96, 0xe4, 0x91}};
constexpr GUID kLavSplitterSource = {
    0xb98d13e7, 0x55db, 0x4385, {0xa3, 0x3d, 0x09, 0xfd, 0x1b, 0xa2, 0x63, 0x38}};
// MEDIASUBTYPE_DOLBY_DDPLUS from the Windows SDK. Keep a local value so the
// standalone harness does not depend on a registered GUID-definition object.
constexpr GUID kDolbyDdPlus = {
    0xa7fb87af, 0x2d02, 0x42fb, {0xa4, 0xd4, 0x05, 0xcd, 0x93, 0x84, 0x3b, 0xdd}};
constexpr GUID kOpenJocDiagnosticsIidOracle = {
    0x16c95ff3, 0x9d9e, 0x4282, {0xaf, 0x61, 0xe6, 0xc7, 0xaf, 0x32, 0x44, 0x6b}};
constexpr GUID kOpenJocDiagnostics2IidOracle = {
    0xa9c07b6a, 0x4c8f, 0x4b6a, {0x9d, 0x1f, 0x6c, 0x9d, 0x3e, 0x5b, 0x7a, 0x20}};
constexpr GUID kLavAudioStatusPage = {
    0x20ed4a03, 0x6afd, 0x4fd9, {0x98, 0x0b, 0x2f, 0x61, 0x43, 0xaa, 0x08, 0x92}};
constexpr wchar_t kRuntimeManifestName[] = L"OpenJocRuntimeIdentity.tsv";
constexpr wchar_t kDependencyManifestName[] = L"LAVFilters.Dependencies.manifest";
} // namespace

// pattern: Functional Core
namespace openjoc_harness_core
{
bool ExactMediaTypeEqual(const AM_MEDIA_TYPE &left, const AM_MEDIA_TYPE &right)
{
    if (left.majortype != right.majortype || left.subtype != right.subtype ||
        left.bFixedSizeSamples != right.bFixedSizeSamples ||
        left.bTemporalCompression != right.bTemporalCompression ||
        left.lSampleSize != right.lSampleSize || left.formattype != right.formattype ||
        left.cbFormat != right.cbFormat || left.pUnk != right.pUnk)
        return false;
    if (left.cbFormat == 0)
        return left.pbFormat == nullptr && right.pbFormat == nullptr;
    return left.pbFormat && right.pbFormat &&
           std::equal(left.pbFormat, left.pbFormat + left.cbFormat, right.pbFormat);
}

bool FingerprintsArePairwiseDistinct(const std::vector<std::vector<float>> &fingerprints)
{
    if (fingerprints.size() < 2 || fingerprints.front().empty())
        return false;
    const std::size_t samples = fingerprints.front().size();
    for (const auto &fingerprint : fingerprints)
    {
        if (fingerprint.size() != samples)
            return false;
    }
    for (std::size_t left = 0; left < fingerprints.size(); ++left)
    {
        for (std::size_t right = left + 1; right < fingerprints.size(); ++right)
        {
            if (fingerprints[left] == fingerprints[right])
                return false;
        }
    }
    return true;
}

std::vector<std::vector<float>> InterleavedFingerprints(const std::vector<BYTE> &bytes,
                                                        const std::uint32_t channels)
{
    if (channels < 2 || bytes.empty() || bytes.size() % sizeof(float) != 0)
        return {};
    const std::size_t elements = bytes.size() / sizeof(float);
    if (elements % channels != 0)
        return {};
    std::vector<std::vector<float>> fingerprints(channels);
    for (auto &fingerprint : fingerprints)
        fingerprint.reserve(elements / channels);
    for (std::size_t element = 0; element < elements; ++element)
    {
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + element * sizeof(float), sizeof(value));
        fingerprints[element % channels].push_back(value);
    }
    return fingerprints;
}

std::vector<std::vector<BYTE>> InterleavedChannelBytes(const std::vector<BYTE> &bytes,
                                                       const std::uint32_t channels)
{
    if (channels < 2 || bytes.empty() || bytes.size() % sizeof(float) != 0)
        return {};
    const std::size_t frames = bytes.size() / sizeof(float) / channels;
    if (frames == 0 || frames * channels * sizeof(float) != bytes.size())
        return {};
    std::vector<std::vector<BYTE>> result(channels);
    for (auto &channel : result)
        channel.reserve(frames * sizeof(float));
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        for (std::uint32_t channel = 0; channel < channels; ++channel)
        {
            const std::size_t offset = (frame * channels + channel) * sizeof(float);
            result[channel].insert(result[channel].end(), bytes.begin() + offset,
                                   bytes.begin() + offset + sizeof(float));
        }
    }
    return result;
}

struct EvidenceInputs
{
    bool requested_type_exact = false;
    bool receive_type_exact = false;
    bool output_type_exact = false;
    bool input_type_exact = false;
    bool post_stream_type_exact = false;
    bool sample_types_exact = false;
    bool exact_connection = false;
    bool paused = false;
    bool running = false;
    bool running_sample = false;
    bool allocator_valid = false;
    bool timestamps_complete = false;
    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
    std::uint64_t end_of_stream_count = 0;
    bool end_of_stream_running = false;
    bool graph_error = false;
};

enum class ControlledEvidenceState
{
    Incomplete,
    ControlledSinkComplete,
};

ControlledEvidenceState ClassifyControlledEvidence(const EvidenceInputs &inputs)
{
    if (!inputs.requested_type_exact || !inputs.receive_type_exact || !inputs.output_type_exact ||
        !inputs.input_type_exact || !inputs.post_stream_type_exact || !inputs.sample_types_exact ||
        !inputs.exact_connection || !inputs.paused || !inputs.running || !inputs.running_sample ||
        !inputs.allocator_valid || !inputs.timestamps_complete || inputs.samples == 0 ||
        inputs.bytes == 0 || inputs.end_of_stream_count != 1 || !inputs.end_of_stream_running ||
        inputs.graph_error)
        return ControlledEvidenceState::Incomplete;
    return ControlledEvidenceState::ControlledSinkComplete;
}

enum class NativeProbeState
{
    Unverified,
    ExactRejection,
    TypeMutation,
    InitialStreamObserved,
    StreamObserved,
};

struct NativeTypeAggregateEvidence
{
    std::uint32_t observation_count = 0;
    std::uint32_t successful_observation_count = 0;
    bool query_failure = false;
    bool mismatch_observed = false;
};

NativeTypeAggregateEvidence AccumulateNativeTypeObservation(
    NativeTypeAggregateEvidence value, const bool executed,
    const HRESULT output_status, const HRESULT renderer_input_status,
    const bool output_exact, const bool renderer_input_exact, const bool peer_equal)
{
    if (!executed)
        return value;
    ++value.observation_count;
    if (output_status != S_OK || renderer_input_status != S_OK)
    {
        value.query_failure = true;
        return value;
    }
    ++value.successful_observation_count;
    if (!output_exact || !renderer_input_exact || !peer_equal)
        value.mismatch_observed = true;
    return value;
}

struct NativeSeekEpochWitness
{
    HRESULT pre_drain_status = E_UNEXPECTED;
    bool prior_graph_error = false;
    HRESULT diagnostics_before_status = E_UNEXPECTED;
    ULONGLONG classifier_bytes_before = 0;
    ULONGLONG stream_bytes_before = 0;
    HRESULT diagnostics_after_status = E_UNEXPECTED;
    ULONGLONG classifier_bytes_after = 0;
    ULONGLONG stream_bytes_after = 0;
    HRESULT position_before_run_status = E_UNEXPECTED;
    LONGLONG position_before_run = 0;
    HRESULT position_after_completion_status = E_UNEXPECTED;
    LONGLONG position_after_completion = 0;
    bool fresh_completion = false;
    HRESULT renderer_stats_status = E_UNEXPECTED;
    DWORD renderer_last_buffer_duration = 0;
    HRESULT renderer_discontinuities_before_status = E_UNEXPECTED;
    DWORD renderer_discontinuities_before = 0;
    HRESULT renderer_discontinuities_after_status = E_UNEXPECTED;
    DWORD renderer_discontinuities_after = 0;
};

bool NativeSeekEpochWitnessIsComplete(const NativeSeekEpochWitness &value)
{
    return value.pre_drain_status == S_OK && !value.prior_graph_error &&
           value.diagnostics_before_status == S_OK &&
           value.diagnostics_after_status == S_OK &&
           value.classifier_bytes_after > value.classifier_bytes_before &&
           value.stream_bytes_after > value.stream_bytes_before &&
           value.position_before_run_status == S_OK &&
           value.position_after_completion_status == S_OK &&
           value.position_after_completion > value.position_before_run &&
           value.fresh_completion && value.renderer_stats_status == S_OK &&
           value.renderer_last_buffer_duration > 0 &&
           value.renderer_discontinuities_before_status == S_OK &&
           value.renderer_discontinuities_after_status == S_OK &&
           value.renderer_discontinuities_after >
               value.renderer_discontinuities_before;
}

struct NativeProbeEvidence
{
    bool runtime_identity = false;
    bool fixture_identity = false;
    bool graph_setup_complete = false;
    bool connect_attempted = false;
    HRESULT connect_direct_status = E_UNEXPECTED;
    NativeTypeAggregateEvidence type_observations;
    HRESULT pause_call_status = E_UNEXPECTED;
    HRESULT pause_state_status = E_UNEXPECTED;
    OAFilterState pause_state = State_Stopped;
    HRESULT run_call_status = E_UNEXPECTED;
    HRESULT run_state_status = E_UNEXPECTED;
    OAFilterState run_state = State_Stopped;
    HRESULT wait_completion_status = E_UNEXPECTED;
    long completion_code = 0;
    HRESULT renderer_stats_status = E_UNEXPECTED;
    DWORD renderer_last_buffer_duration = 0;
    HRESULT midstream_renderer_stats_status = E_UNEXPECTED;
    DWORD midstream_last_buffer_duration = 0;
    HRESULT diagnostics_status = E_UNEXPECTED;
    ULONGLONG classifier_bytes = 0;
    ULONGLONG stream_bytes = 0;
    bool initial_eos_complete = false;
    bool seek_25_complete = false;
    bool forward_seek_complete = false;
    bool backward_seek_complete = false;
    HRESULT stop_status = E_UNEXPECTED;
    bool reopen_complete = false;
};

NativeProbeState ClassifyNativeProbe(const NativeProbeEvidence &value)
{
    if (!value.runtime_identity || !value.fixture_identity)
        return NativeProbeState::Unverified;
    if (value.connect_attempted &&
        (value.connect_direct_status == VFW_E_TYPE_NOT_ACCEPTED ||
         value.connect_direct_status == VFW_E_UNSUPPORTED_AUDIO))
        return NativeProbeState::ExactRejection;
    if (!value.connect_attempted || value.connect_direct_status != S_OK)
        return NativeProbeState::Unverified;

    if (value.type_observations.mismatch_observed)
        return NativeProbeState::TypeMutation;
    if (value.type_observations.query_failure ||
        value.type_observations.observation_count < 2 ||
        value.type_observations.successful_observation_count !=
            value.type_observations.observation_count)
        return NativeProbeState::Unverified;

    const bool renderer_delivery_observed =
        (value.midstream_renderer_stats_status == S_OK &&
         value.midstream_last_buffer_duration > 0) ||
        (value.renderer_stats_status == S_OK && value.renderer_last_buffer_duration > 0);
    if (!value.graph_setup_complete || !SUCCEEDED(value.pause_call_status) ||
        value.pause_state_status != S_OK ||
        value.pause_state != State_Paused || !SUCCEEDED(value.run_call_status) ||
        value.run_state_status != S_OK || value.run_state != State_Running ||
        value.wait_completion_status != S_OK || value.completion_code != EC_COMPLETE ||
        !renderer_delivery_observed ||
        value.diagnostics_status != S_OK || value.classifier_bytes == 0 ||
        value.stream_bytes == 0 || !value.initial_eos_complete)
        return NativeProbeState::Unverified;
    return value.seek_25_complete && value.forward_seek_complete &&
                   value.backward_seek_complete && value.stop_status == S_OK &&
                   value.reopen_complete
               ? NativeProbeState::StreamObserved
               : NativeProbeState::InitialStreamObserved;
}

struct Task4TrendEvidence
{
    double full_slope = 0.0;
    double full_r_squared = 0.0;
    double tail_slope = 0.0;
    double tail_r_squared = 0.0;
    std::array<std::size_t, 4> quartile_medians{};
    std::array<std::size_t, 4> quartile_lower_quartiles{};
    std::array<std::size_t, 2> tail_half_medians{};
    bool gate_a = false;
    bool gate_b = false;
    bool tail_gate = false;
};

struct Task4CycleEvidence
{
    bool source_type_exact = false;
    bool output_type_exact = false;
    bool graph_exactly_three_filters = false;
    bool paused = false;
    bool running = false;
    bool no_graph_error = false;
    bool timestamps_valid = false;
    bool sample_contracts_valid = false;
    bool allocator_valid = false;
    bool runtime_identity_valid = false;
    bool fixture_identity_valid = false;
    bool policy_exact = false;
    bool end_of_stream_running = false;
    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
    std::uint64_t timestamp_count = 0;
    std::array<std::uint8_t, 32> payload_digest{};
    REFERENCE_TIME first_timestamp = 0;
    REFERENCE_TIME last_timestamp = 0;
    std::uint64_t end_of_stream_count = 0;
    long allocator_requested_capacity = 0;
    long allocator_actual_capacity = 0;
    long sample_capacity_high_water = 0;
    long sample_actual_length_high_water = 0;
    std::size_t checked_total_bytes = 0;
    std::uint64_t actual_length_total = 0;
};

bool Task4CycleEvidenceIsValid(const Task4CycleEvidence &value)
{
    return value.source_type_exact && value.output_type_exact &&
           value.graph_exactly_three_filters && value.paused && value.running &&
           value.no_graph_error && value.timestamps_valid && value.sample_contracts_valid &&
           value.allocator_valid && value.runtime_identity_valid &&
           value.fixture_identity_valid && value.policy_exact && value.end_of_stream_running &&
           value.samples > 0 && value.bytes > 0 && value.timestamp_count == value.samples &&
           value.first_timestamp < value.last_timestamp && value.end_of_stream_count == 1 &&
           value.allocator_requested_capacity > 0 &&
           value.allocator_actual_capacity >= value.allocator_requested_capacity &&
           value.sample_capacity_high_water > 0 && value.sample_actual_length_high_water > 0 &&
           value.sample_actual_length_high_water <= value.sample_capacity_high_water &&
           value.checked_total_bytes == value.bytes && value.actual_length_total == value.bytes;
}

bool Task4CycleMatchesBaseline(const Task4CycleEvidence &baseline,
                               const Task4CycleEvidence &candidate)
{
    return Task4CycleEvidenceIsValid(baseline) && Task4CycleEvidenceIsValid(candidate) &&
           baseline.samples == candidate.samples && baseline.bytes == candidate.bytes &&
           baseline.timestamp_count == candidate.timestamp_count &&
           baseline.payload_digest == candidate.payload_digest &&
           baseline.first_timestamp == candidate.first_timestamp &&
           baseline.last_timestamp == candidate.last_timestamp &&
           baseline.end_of_stream_count == candidate.end_of_stream_count &&
           baseline.allocator_requested_capacity == candidate.allocator_requested_capacity &&
           baseline.allocator_actual_capacity == candidate.allocator_actual_capacity &&
           baseline.sample_capacity_high_water == candidate.sample_capacity_high_water &&
           baseline.sample_actual_length_high_water == candidate.sample_actual_length_high_water &&
           baseline.checked_total_bytes == candidate.checked_total_bytes &&
           baseline.actual_length_total == candidate.actual_length_total;
}

std::size_t MedianRange(const std::vector<std::size_t> &values, const std::size_t begin,
                        const std::size_t count)
{
    if (count == 0 || begin > values.size() || count > values.size() - begin)
        return 0;
    std::vector<std::size_t> ordered(values.begin() + begin, values.begin() + begin + count);
    std::sort(ordered.begin(), ordered.end());
    const std::size_t middle = ordered.size() / 2;
    if (ordered.size() % 2 != 0)
        return ordered[middle];
    return ordered[middle - 1] + (ordered[middle] - ordered[middle - 1]) / 2;
}

std::size_t LowerQuartileRange(const std::vector<std::size_t> &values,
                               const std::size_t begin, const std::size_t count)
{
    if (count < 4 || begin > values.size() || count > values.size() - begin)
        return 0;
    std::vector<std::size_t> ordered(values.begin() + begin, values.begin() + begin + count);
    std::sort(ordered.begin(), ordered.end());
    const std::size_t lower_count = ordered.size() / 2;
    const std::size_t middle = lower_count / 2;
    return lower_count % 2 != 0
               ? ordered[middle]
               : ordered[middle - 1] + (ordered[middle] - ordered[middle - 1]) / 2;
}

bool LinearFit(const std::vector<std::size_t> &values, const std::size_t begin,
               const std::size_t count, double *slope, double *r_squared)
{
    if (!slope || !r_squared || count < 2 || begin > values.size() ||
        count > values.size() - begin)
        return false;
    const double mean_x = static_cast<double>(count - 1) / 2.0;
    double mean_y = 0.0;
    for (std::size_t index = 0; index < count; ++index)
        mean_y += static_cast<double>(values[begin + index]);
    mean_y /= static_cast<double>(count);
    double covariance = 0.0;
    double variance_x = 0.0;
    double variance_y = 0.0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const double centered_x = static_cast<double>(index) - mean_x;
        const double centered_y = static_cast<double>(values[begin + index]) - mean_y;
        covariance += centered_x * centered_y;
        variance_x += centered_x * centered_x;
        variance_y += centered_y * centered_y;
    }
    if (variance_x <= 0.0)
        return false;
    *slope = covariance / variance_x;
    *r_squared = variance_y <= 0.0 ? 0.0 : (covariance * covariance) / (variance_x * variance_y);
    return std::isfinite(*slope) && std::isfinite(*r_squared);
}

bool IncreasedByPages(const std::size_t high, const std::size_t low,
                      const std::size_t page_size, const std::size_t pages)
{
    return page_size > 0 && pages <= (std::numeric_limits<std::size_t>::max)() / page_size &&
           high >= low && high - low >= page_size * pages;
}

bool WorkingSetTrendIsBounded(const std::vector<std::size_t> &values,
                              const std::size_t page_size, Task4TrendEvidence *evidence)
{
    if (!evidence)
        return false;
    *evidence = {};
    if (values.size() != 128 || page_size == 0 ||
        !LinearFit(values, 0, values.size(), &evidence->full_slope,
                   &evidence->full_r_squared) ||
        !LinearFit(values, 64, 64, &evidence->tail_slope, &evidence->tail_r_squared))
        return false;
    for (std::size_t quartile = 0; quartile < 4; ++quartile)
    {
        evidence->quartile_medians[quartile] = MedianRange(values, quartile * 32, 32);
        evidence->quartile_lower_quartiles[quartile] =
            LowerQuartileRange(values, quartile * 32, 32);
    }
    evidence->tail_half_medians[0] = MedianRange(values, 64, 32);
    evidence->tail_half_medians[1] = MedianRange(values, 96, 32);
    evidence->gate_a = evidence->full_slope >= static_cast<double>(page_size) / 8.0 &&
                       evidence->full_r_squared >= 0.90 &&
                       IncreasedByPages(evidence->quartile_medians[3],
                                        evidence->quartile_medians[0], page_size, 16);
    evidence->gate_b = IncreasedByPages(evidence->quartile_lower_quartiles[1],
                                        evidence->quartile_lower_quartiles[0], page_size, 43) &&
                       IncreasedByPages(evidence->quartile_lower_quartiles[2],
                                        evidence->quartile_lower_quartiles[1], page_size, 43) &&
                       IncreasedByPages(evidence->quartile_lower_quartiles[3],
                                        evidence->quartile_lower_quartiles[2], page_size, 43) &&
                       IncreasedByPages(evidence->quartile_lower_quartiles[3],
                                        evidence->quartile_lower_quartiles[0], page_size, 128);
    evidence->tail_gate = evidence->tail_slope >= static_cast<double>(page_size) &&
                          evidence->tail_r_squared >= 0.85 &&
                          IncreasedByPages(evidence->tail_half_medians[1],
                                           evidence->tail_half_medians[0], page_size, 32);
    return !evidence->gate_a && !evidence->gate_b && !evidence->tail_gate;
}
} // namespace openjoc_harness_core

// pattern: Imperative Shell
namespace openjoc_harness_shell
{
using Digest = std::array<std::uint8_t, 32>;
using DllGetClassObjectProc = HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *);

struct FixtureIdentity
{
    std::filesystem::path final_path;
    Digest sha256{};
};

constexpr wchar_t kOpenJocPolicyRegistryKey[] = L"Software\\LAV\\Audio\\OpenJOC";
constexpr wchar_t kOpenJocPolicyVersionValue[] = L"OpenJocOutputPolicyVersion";
constexpr wchar_t kOpenJocPolicyValue[] = L"OpenJocOutputPolicy";

struct RegistrySnapshotEntry
{
    std::wstring path;
    std::wstring name;
    DWORD type = 0;
    std::vector<BYTE> data;

    bool operator==(const RegistrySnapshotEntry &right) const
    {
        return path == right.path && name == right.name && type == right.type &&
               data == right.data;
    }
    bool operator<(const RegistrySnapshotEntry &right) const
    {
        if (path != right.path)
            return path < right.path;
        return name < right.name;
    }
};

bool SnapshotRegistryKeyRecursive(HKEY key, const std::wstring &path,
                                  std::vector<RegistrySnapshotEntry> *entries)
{
    if (!key || !entries)
        return false;
    entries->push_back({path, L"", REG_NONE, {}});
    for (DWORD index = 0;; ++index)
    {
        wchar_t name[16384] = {};
        DWORD name_length = static_cast<DWORD>(std::size(name));
        DWORD type = 0;
        DWORD data_size = 0;
        LONG status = RegEnumValueW(key, index, name, &name_length, nullptr, &type, nullptr,
                                    &data_size);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS)
            return false;
        std::vector<BYTE> data(data_size);
        name_length = static_cast<DWORD>(std::size(name));
        status = RegEnumValueW(key, index, name, &name_length, nullptr, &type,
                               data.empty() ? nullptr : data.data(), &data_size);
        if (status != ERROR_SUCCESS)
            return false;
        data.resize(data_size);
        entries->push_back({path, std::wstring(name, name + name_length), type, std::move(data)});
    }
    for (DWORD index = 0;; ++index)
    {
        wchar_t name[256] = {};
        DWORD name_length = static_cast<DWORD>(std::size(name));
        LONG status = RegEnumKeyExW(key, index, name, &name_length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS)
            return false;
        HKEY child = nullptr;
        status = RegOpenKeyExW(key, name, 0, KEY_READ, &child);
        if (status != ERROR_SUCCESS)
            return false;
        const std::wstring child_path = path + L"\\" + std::wstring(name, name + name_length);
        const bool ok = SnapshotRegistryKeyRecursive(child, child_path, entries);
        RegCloseKey(child);
        if (!ok)
            return false;
    }
    return true;
}

bool SnapshotOpenJocRegistry(std::vector<RegistrySnapshotEntry> *entries)
{
    if (!entries)
        return false;
    entries->clear();
    HKEY key = nullptr;
    const LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kOpenJocPolicyRegistryKey, 0,
                                      KEY_READ, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return true;
    if (status != ERROR_SUCCESS)
        return false;
    const bool ok = SnapshotRegistryKeyRecursive(key, kOpenJocPolicyRegistryKey, entries);
    RegCloseKey(key);
    std::sort(entries->begin(), entries->end());
    return ok;
}

struct RegistryRestoreObservation
{
    bool attempted = false;
    bool succeeded = false;
};

class VolatileCurrentUserOverride final
{
  public:
    explicit VolatileCurrentUserOverride(RegistryRestoreObservation *observation = nullptr)
        : observation_(observation)
    {
        if (!SnapshotOpenJocRegistry(&before_))
            return;
        wchar_t path[160] = {};
        _snwprintf_s(path, _TRUNCATE, L"Software\\OpenJOC-Task3-%lu-%llu",
                     static_cast<unsigned long>(GetCurrentProcessId()),
                     static_cast<unsigned long long>(GetTickCount64()));
        path_ = path;
        DWORD disposition = 0;
        status_ = RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr,
                                  REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &root_,
                                  &disposition);
        if (status_ == ERROR_SUCCESS)
            status_ = RegOverridePredefKey(HKEY_CURRENT_USER, root_);
        overridden_ = status_ == ERROR_SUCCESS;
        HKEY policy = nullptr;
        if (overridden_)
            status_ = RegCreateKeyExW(HKEY_CURRENT_USER, kOpenJocPolicyRegistryKey, 0, nullptr,
                                      REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &policy,
                                      &disposition);
        if (policy)
            RegCloseKey(policy);
    }

    ~VolatileCurrentUserOverride()
    {
        const bool restored = Restore();
        if (observation_)
        {
            observation_->attempted = true;
            observation_->succeeded = restored;
        }
    }

    bool ready() const { return overridden_ && status_ == ERROR_SUCCESS; }
    const std::wstring &temporary_path() const { return path_; }

    bool WritePolicy(const LAVOpenJocOutputPolicy policy)
    {
        if (!ready() || !FindLAVOpenJocOutputContract(policy))
            return false;
        HKEY key = nullptr;
        LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kOpenJocPolicyRegistryKey, 0,
                                    KEY_SET_VALUE, &key);
        const DWORD version = LAV_OPENJOC_OUTPUT_POLICY_SCHEMA_VERSION;
        const DWORD value = static_cast<DWORD>(policy);
        if (status == ERROR_SUCCESS)
            status = RegSetValueExW(key, kOpenJocPolicyVersionValue, 0, REG_DWORD,
                                    reinterpret_cast<const BYTE *>(&version), sizeof(version));
        if (status == ERROR_SUCCESS)
            status = RegSetValueExW(key, kOpenJocPolicyValue, 0, REG_DWORD,
                                    reinterpret_cast<const BYTE *>(&value), sizeof(value));
        if (key)
            RegCloseKey(key);
        return status == ERROR_SUCCESS;
    }

    bool Restore()
    {
        if (restored_)
            return restore_ok_;
        if (overridden_)
        {
            status_ = RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            overridden_ = false;
        }
        if (root_)
        {
            RegCloseKey(root_);
            root_ = nullptr;
        }
        const LONG delete_status = path_.empty()
                                       ? ERROR_INVALID_PARAMETER
                                       : RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
        std::vector<RegistrySnapshotEntry> after;
        const bool snapshot_ok = SnapshotOpenJocRegistry(&after) && after == before_;
        HKEY deleted = nullptr;
        const LONG absent_status = path_.empty()
                                       ? ERROR_INVALID_PARAMETER
                                       : RegOpenKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0,
                                                       KEY_READ, &deleted);
        if (deleted)
            RegCloseKey(deleted);
        restore_ok_ = status_ == ERROR_SUCCESS &&
                      (delete_status == ERROR_SUCCESS || delete_status == ERROR_FILE_NOT_FOUND ||
                       delete_status == ERROR_PATH_NOT_FOUND) &&
                      (absent_status == ERROR_FILE_NOT_FOUND || absent_status == ERROR_PATH_NOT_FOUND) &&
                      snapshot_ok;
        std::wprintf(L"TASK3_REGISTRY_RESTORE override_status=%ld delete_status=%ld "
                     L"absent_status=%ld snapshot_exact=%d result=%d path=%ls\n",
                     status_, delete_status, absent_status, snapshot_ok ? 1 : 0,
                     restore_ok_ ? 1 : 0, path_.c_str());
        restored_ = true;
        return restore_ok_;
    }

  private:
    HKEY root_ = nullptr;
    std::wstring path_;
    std::vector<RegistrySnapshotEntry> before_;
    LONG status_ = ERROR_INVALID_FUNCTION;
    bool overridden_ = false;
    bool restored_ = false;
    bool restore_ok_ = false;
    RegistryRestoreObservation *observation_ = nullptr;
};

enum class StagedKind
{
    Module,
    File,
};

struct StagedRecord
{
    StagedKind kind = StagedKind::File;
    std::wstring basename;
    std::wstring final_path;
    Digest sha256{};
};

template <typename T> void Release(T *&value)
{
    if (value)
    {
        value->Release();
        value = nullptr;
    }
}

bool SameText(const std::wstring &left, const std::wstring &right)
{
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool SamePath(const std::wstring &left, const std::wstring &right)
{
    return !left.empty() && !right.empty() && SameText(left, right);
}

std::wstring Lowercase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t value) { return std::towlower(value); });
    return value;
}

bool LooksLikeLavPrivateDll(const std::wstring &basename)
{
    const std::wstring lower = Lowercase(basename);
    return lower.size() > 9 && lower.find(L"-lav-") != std::wstring::npos &&
           lower.compare(lower.size() - 4, 4, L".dll") == 0;
}

std::wstring NormalizeFinalPath(std::wstring path)
{
    if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        return L"\\\\" + path.substr(8);
    if (path.compare(0, 4, L"\\\\?\\") == 0)
        return path.substr(4);
    return path;
}

std::wstring FinalPathForFile(const std::filesystem::path &input)
{
    if (!input.is_absolute())
        return {};
    HANDLE file = CreateFileW(input.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file, path.data(),
                                                   static_cast<DWORD>(path.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return NormalizeFinalPath(std::move(path));
}

std::wstring ModulePath(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return FinalPathForFile(path);
}

bool Sha256File(const std::wstring &path, Digest *digest)
{
    if (!digest)
        return false;
    digest->fill(0);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0;
    if (ok)
        ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    std::array<std::uint8_t, 65536> buffer{};
    while (ok)
    {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            ok = false;
            break;
        }
        if (read == 0)
            break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) == 0;
    }
    if (ok)
        ok = BCryptFinishHash(hash, digest->data(), static_cast<ULONG>(digest->size()), 0) == 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok)
        digest->fill(0);
    return ok;
}

bool Sha256Bytes(const std::vector<BYTE> &bytes, Digest *digest)
{
    if (!digest || bytes.empty() || bytes.size() > (std::numeric_limits<ULONG>::max)())
        return false;
    digest->fill(0);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0;
    if (ok)
        ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok)
        ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                            static_cast<ULONG>(bytes.size()), 0) == 0;
    if (ok)
        ok = BCryptFinishHash(hash, digest->data(), static_cast<ULONG>(digest->size()), 0) == 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok)
        digest->fill(0);
    return ok;
}

bool ChannelDigestsEqualAndDistinct(const std::vector<BYTE> &captured,
                                    const std::vector<BYTE> &oracle,
                                    const std::uint32_t channels)
{
    const auto captured_channels =
        openjoc_harness_core::InterleavedChannelBytes(captured, channels);
    const auto oracle_channels = openjoc_harness_core::InterleavedChannelBytes(oracle, channels);
    if (captured_channels.size() != channels || oracle_channels.size() != channels)
        return false;
    std::vector<Digest> captured_digests(channels);
    std::vector<Digest> oracle_digests(channels);
    for (std::uint32_t channel = 0; channel < channels; ++channel)
    {
        if (captured_channels[channel] != oracle_channels[channel] ||
            !Sha256Bytes(captured_channels[channel], &captured_digests[channel]) ||
            !Sha256Bytes(oracle_channels[channel], &oracle_digests[channel]) ||
            captured_digests[channel] != oracle_digests[channel])
            return false;
    }
    for (std::uint32_t left = 0; left < channels; ++left)
    {
        for (std::uint32_t right = left + 1; right < channels; ++right)
        {
            if (captured_digests[left] == captured_digests[right])
                return false;
        }
    }
    return true;
}

std::string DigestHex(const Digest &digest)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(64);
    for (const std::uint8_t byte : digest)
    {
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 15]);
    }
    return result;
}

std::string GuidText(const GUID &guid)
{
    wchar_t wide[40] = {};
    const int length = StringFromGUID2(guid, wide, static_cast<int>(std::size(wide)));
    if (length <= 1)
        return {};
    std::string result;
    result.reserve(static_cast<std::size_t>(length - 1));
    for (int index = 0; index < length - 1; ++index)
    {
        if (wide[index] > 0x7f)
            return {};
        result.push_back(static_cast<char>(wide[index]));
    }
    return result;
}

std::string BytesHex(const BYTE *bytes, const std::size_t count)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    if (count != 0 && !bytes)
        return {};
    std::string result;
    result.reserve(count * 2);
    for (std::size_t index = 0; index < count; ++index)
    {
        result.push_back(kHex[bytes[index] >> 4]);
        result.push_back(kHex[bytes[index] & 15]);
    }
    return result;
}

std::string SerializeMediaType(const AM_MEDIA_TYPE &media_type)
{
    if (media_type.pUnk != nullptr || (media_type.cbFormat != 0 && !media_type.pbFormat))
        return {};
    std::ostringstream stream;
    stream << GuidText(media_type.majortype) << '|' << GuidText(media_type.subtype) << '|'
           << static_cast<unsigned long>(media_type.bFixedSizeSamples) << '|'
           << static_cast<unsigned long>(media_type.bTemporalCompression) << '|'
           << static_cast<unsigned long>(media_type.lSampleSize) << '|'
           << GuidText(media_type.formattype) << '|' << media_type.cbFormat << '|'
           << BytesHex(media_type.pbFormat, media_type.cbFormat);
    return stream.str();
}

bool BuildFixtureIdentity(const std::filesystem::path &path, FixtureIdentity *identity)
{
    if (!identity || !path.is_absolute() || !std::filesystem::is_regular_file(path))
        return false;
    const std::wstring final_path = FinalPathForFile(path);
    if (final_path.empty() || !Sha256File(final_path, &identity->sha256))
        return false;
    identity->final_path = final_path;
    return true;
}

bool FixtureIdentityMatches(const FixtureIdentity &identity)
{
    Digest current{};
    return !identity.final_path.empty() &&
           SamePath(FinalPathForFile(identity.final_path), identity.final_path.native()) &&
           Sha256File(identity.final_path.native(), &current) && current == identity.sha256;
}

bool ParseDigest(const std::string &text, Digest *digest)
{
    if (!digest || text.size() != 64)
        return false;
    auto nibble = [](char value) {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < digest->size(); ++index)
    {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        (*digest)[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::string WideToUtf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (needed <= 0)
        return {};
    std::string result(static_cast<std::size_t>(needed), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), result.data(), needed, nullptr,
                               nullptr) == needed
               ? result
               : std::string{};
}

std::wstring Utf8ToWide(const std::string &text)
{
    if (text.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), result.data(), needed) == needed
               ? result
               : std::wstring{};
}

bool WriteAll(HANDLE file, const std::string &payload)
{
    std::size_t offset = 0;
    while (offset < payload.size())
    {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            payload.size() - offset, (std::numeric_limits<DWORD>::max)()));
        DWORD written = 0;
        if (!WriteFile(file, payload.data() + offset, requested, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

bool ReadAll(const std::wstring &path, std::string *payload)
{
    if (!payload)
        return false;
    payload->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 1024 * 1024;
    if (ok)
    {
        payload->resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, payload->data(), static_cast<DWORD>(payload->size()), &read, nullptr) &&
             read == payload->size();
    }
    CloseHandle(file);
    if (!ok)
        payload->clear();
    return ok;
}

bool AddRecord(const std::wstring &runtime_final, const std::filesystem::path &path,
               StagedKind kind, std::vector<StagedRecord> *records)
{
    if (!records)
        return false;
    StagedRecord record;
    record.kind = kind;
    record.final_path = FinalPathForFile(path);
    if (record.final_path.empty() ||
        !SamePath(std::filesystem::path(record.final_path).parent_path().native(), runtime_final))
        return false;
    record.basename = std::filesystem::path(record.final_path).filename().native();
    if (!Sha256File(record.final_path, &record.sha256))
        return false;
    for (const auto &existing : *records)
    {
        if (SameText(existing.basename, record.basename))
            return false;
    }
    records->push_back(std::move(record));
    return true;
}

bool WriteStagedManifest(const std::filesystem::path &runtime_dir,
                         const std::filesystem::path &manifest_path)
{
    const std::wstring runtime_final = FinalPathForFile(runtime_dir);
    if (runtime_final.empty() || !manifest_path.is_absolute() ||
        !SameText(manifest_path.filename().native(), kRuntimeManifestName) ||
        !SamePath(FinalPathForFile(manifest_path.parent_path()), runtime_final))
        return false;
    std::vector<StagedRecord> records;
    for (const wchar_t *name : {L"OpenJocDirectShowNegotiationSmoke.exe", L"LAVAudio.ax",
                                L"LAVSplitter.ax", L"openjoc_capi.dll", L"libbluray.dll"})
    {
        if (!AddRecord(runtime_final, runtime_dir / name, StagedKind::Module, &records))
            return false;
    }
    if (!AddRecord(runtime_final, runtime_dir / kDependencyManifestName, StagedKind::File, &records))
        return false;

    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((runtime_dir / L"*-lav-*.dll").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return false;
    bool found_private_dll = false;
    do
    {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            found_private_dll = true;
            if (!AddRecord(runtime_final, runtime_dir / found.cFileName, StagedKind::Module,
                           &records))
            {
                FindClose(search);
                return false;
            }
        }
    } while (FindNextFileW(search, &found));
    const DWORD find_error = GetLastError();
    FindClose(search);
    if (!found_private_dll || find_error != ERROR_NO_MORE_FILES)
        return false;

    std::sort(records.begin(), records.end(), [](const StagedRecord &left,
                                                  const StagedRecord &right) {
        return Lowercase(left.basename) < Lowercase(right.basename);
    });
    std::string payload = "OPENJOC_RUNTIME_IDENTITY_V1\n";
    for (const auto &record : records)
    {
        const std::string basename = WideToUtf8(record.basename);
        const std::string path = WideToUtf8(record.final_path);
        if (basename.empty() || path.empty() ||
            basename.find_first_of("\t\r\n") != std::string::npos ||
            path.find_first_of("\t\r\n") != std::string::npos)
            return false;
        payload += record.kind == StagedKind::Module ? "module\t" : "file\t";
        payload += basename + "\t" + DigestHex(record.sha256) + "\t" + path + "\n";
    }
    HANDLE output = CreateFileW(manifest_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = WriteAll(output, payload) && FlushFileBuffers(output);
    CloseHandle(output);
    if (!ok)
        DeleteFileW(manifest_path.c_str());
    return ok;
}

std::vector<std::string> SplitTabs(const std::string &line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
        if (tab == std::string::npos)
            return fields;
        start = tab + 1;
    }
}

const StagedRecord *FindRecord(const std::vector<StagedRecord> &records, StagedKind kind,
                               const std::wstring &basename)
{
    const auto found = std::find_if(records.begin(), records.end(), [&](const StagedRecord &record) {
        return record.kind == kind && SameText(record.basename, basename);
    });
    return found == records.end() ? nullptr : &*found;
}

bool ReadStagedManifest(const std::filesystem::path &runtime_dir,
                        const std::filesystem::path &manifest_path,
                        std::vector<StagedRecord> *records)
{
    if (!records)
        return false;
    records->clear();
    const std::wstring runtime_final = FinalPathForFile(runtime_dir);
    const std::wstring manifest_final = FinalPathForFile(manifest_path);
    if (runtime_final.empty() || manifest_final.empty() ||
        !SameText(std::filesystem::path(manifest_final).filename().native(), kRuntimeManifestName) ||
        !SamePath(std::filesystem::path(manifest_final).parent_path().native(), runtime_final))
        return false;
    const DWORD attributes = GetFileAttributesW(manifest_final.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_READONLY) == 0)
        return false;

    std::string payload;
    constexpr char kHeader[] = "OPENJOC_RUNTIME_IDENTITY_V1\n";
    constexpr std::size_t kHeaderLength = sizeof(kHeader) - 1;
    if (!ReadAll(manifest_final, &payload) || payload.compare(0, kHeaderLength, kHeader) != 0 ||
        payload.back() != '\n')
        return false;
    std::size_t position = kHeaderLength;
    while (position < payload.size())
    {
        const std::size_t end = payload.find('\n', position);
        if (end == std::string::npos || end == position || records->size() >= 128)
            return false;
        const std::vector<std::string> fields = SplitTabs(payload.substr(position, end - position));
        if (fields.size() != 4)
            return false;
        StagedRecord record;
        if (fields[0] == "module")
            record.kind = StagedKind::Module;
        else if (fields[0] == "file")
            record.kind = StagedKind::File;
        else
            return false;
        record.basename = Utf8ToWide(fields[1]);
        record.final_path = Utf8ToWide(fields[3]);
        if (record.basename.empty() || record.final_path.empty() ||
            !ParseDigest(fields[2], &record.sha256) ||
            !SameText(std::filesystem::path(record.final_path).filename().native(),
                      record.basename) ||
            !SamePath(std::filesystem::path(record.final_path).parent_path().native(),
                      runtime_final) ||
            !SamePath(FinalPathForFile(record.final_path), record.final_path))
            return false;
        for (const auto &existing : *records)
        {
            if (SameText(existing.basename, record.basename))
                return false;
        }
        records->push_back(std::move(record));
        position = end + 1;
    }

    for (const auto &required :
         std::array<std::pair<StagedKind, std::wstring>, 6>{{
             {StagedKind::Module, L"OpenJocDirectShowNegotiationSmoke.exe"},
             {StagedKind::Module, L"LAVAudio.ax"},
             {StagedKind::Module, L"LAVSplitter.ax"},
             {StagedKind::Module, L"openjoc_capi.dll"},
             {StagedKind::Module, L"libbluray.dll"},
             {StagedKind::File, kDependencyManifestName},
         }})
    {
        if (!FindRecord(*records, required.first, required.second))
            return false;
    }
    std::size_t private_dlls = 0;
    for (const auto &record : *records)
    {
        const bool fixed_module = record.kind == StagedKind::Module &&
                                  (SameText(record.basename,
                                            L"OpenJocDirectShowNegotiationSmoke.exe") ||
                                   SameText(record.basename, L"LAVAudio.ax") ||
                                   SameText(record.basename, L"LAVSplitter.ax") ||
                                   SameText(record.basename, L"openjoc_capi.dll") ||
                                   SameText(record.basename, L"libbluray.dll"));
        const bool private_module = record.kind == StagedKind::Module &&
                                    LooksLikeLavPrivateDll(record.basename);
        const bool dependency_file = record.kind == StagedKind::File &&
                                     SameText(record.basename, kDependencyManifestName);
        if (!fixed_module && !private_module && !dependency_file)
            return false;
        if (private_module)
            ++private_dlls;
    }
    return private_dlls > 0;
}

struct LoadedModule
{
    std::wstring basename;
    std::wstring final_path;
    Digest sha256{};
};

bool EnumerateLoadedModules(std::vector<LoadedModule> *loaded)
{
    if (!loaded)
        return false;
    loaded->clear();
    HMODULE modules[2048] = {};
    DWORD required = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &required) ||
        required > sizeof(modules) || required % sizeof(HMODULE) != 0)
        return false;
    for (std::size_t index = 0; index < required / sizeof(HMODULE); ++index)
    {
        LoadedModule module;
        module.final_path = ModulePath(modules[index]);
        if (module.final_path.empty())
            return false;
        module.basename = std::filesystem::path(module.final_path).filename().native();
        if (!Sha256File(module.final_path, &module.sha256))
            return false;
        loaded->push_back(std::move(module));
    }
    return true;
}

bool RuntimeIdentityMatches(const std::vector<StagedRecord> &records)
{
    std::vector<LoadedModule> loaded;
    if (!EnumerateLoadedModules(&loaded))
        return false;
    for (const auto &record : records)
    {
        if (record.kind == StagedKind::File)
        {
            Digest current{};
            if (!Sha256File(record.final_path, &current) || current != record.sha256)
                return false;
            continue;
        }
        std::size_t matches = 0;
        for (const auto &module : loaded)
        {
            if (SameText(module.basename, record.basename))
            {
                ++matches;
                if (!SamePath(module.final_path, record.final_path) || module.sha256 != record.sha256)
                    return false;
            }
        }
        if ((!LooksLikeLavPrivateDll(record.basename) && matches != 1) ||
            (LooksLikeLavPrivateDll(record.basename) && matches > 1))
            return false;
    }
    for (const auto &module : loaded)
    {
        if (LooksLikeLavPrivateDll(module.basename) &&
            !FindRecord(records, StagedKind::Module, module.basename))
            return false;
    }
    return true;
}

class PrivateComModule final
{
  public:
    PrivateComModule(const std::filesystem::path &absolute_module_path, REFCLSID class_id)
        : path_(FinalPathForFile(absolute_module_path)), class_id_(class_id)
    {
        if (path_.empty())
        {
            status_ = E_INVALIDARG;
            return;
        }
        module_ = LoadLibraryExW(path_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module_)
        {
            status_ = HRESULT_FROM_WIN32(GetLastError());
            return;
        }
        if (!SamePath(path_, ModulePath(module_)) || !Sha256File(path_, &sha256_))
        {
            status_ = E_UNEXPECTED;
            return;
        }
        auto get_class_object = reinterpret_cast<DllGetClassObjectProc>(
            GetProcAddress(module_, "DllGetClassObject"));
        if (!get_class_object)
        {
            status_ = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            return;
        }
        status_ = get_class_object(class_id_, IID_IClassFactory,
                                   reinterpret_cast<void **>(&factory_));
    }

    ~PrivateComModule()
    {
        Release(factory_);
        if (module_)
            FreeLibrary(module_);
    }

    PrivateComModule(const PrivateComModule &) = delete;
    PrivateComModule &operator=(const PrivateComModule &) = delete;

    HRESULT status() const { return status_; }
    const std::wstring &path() const { return path_; }
    const Digest &sha256() const { return sha256_; }

    HRESULT CreateInstance(REFIID interface_id, void **instance) const
    {
        if (!instance)
            return E_POINTER;
        *instance = nullptr;
        if (FAILED(status_) || !factory_)
            return FAILED(status_) ? status_ : E_UNEXPECTED;
        return factory_->CreateInstance(nullptr, interface_id, instance);
    }

  private:
    std::wstring path_;
    CLSID class_id_{};
    HMODULE module_ = nullptr;
    IClassFactory *factory_ = nullptr;
    HRESULT status_ = E_FAIL;
    Digest sha256_{};
};

class ScopedActivationContext final
{
  public:
    ScopedActivationContext(const std::wstring &module_path,
                            const std::wstring &assembly_directory)
    {
        ACTCTXW configuration{};
        configuration.cbSize = sizeof(configuration);
        configuration.dwFlags = ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID |
                                ACTCTX_FLAG_RESOURCE_NAME_VALID;
        configuration.lpSource = module_path.c_str();
        configuration.lpAssemblyDirectory = assembly_directory.c_str();
        configuration.lpResourceName = MAKEINTRESOURCEW(2);
        context_ = CreateActCtxW(&configuration);
        if (context_ != INVALID_HANDLE_VALUE && !ActivateActCtx(context_, &cookie_))
        {
            ReleaseActCtx(context_);
            context_ = INVALID_HANDLE_VALUE;
            cookie_ = 0;
        }
    }

    ~ScopedActivationContext()
    {
        if (cookie_)
            DeactivateActCtx(0, cookie_);
        if (context_ != INVALID_HANDLE_VALUE)
            ReleaseActCtx(context_);
    }

    ScopedActivationContext(const ScopedActivationContext &) = delete;
    ScopedActivationContext &operator=(const ScopedActivationContext &) = delete;
    bool active() const { return context_ != INVALID_HANDLE_VALUE && cookie_ != 0; }

  private:
    HANDLE context_ = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie_ = 0;
};

void FreeModules(std::vector<HMODULE> *modules)
{
    if (!modules)
        return;
    for (auto module = modules->rbegin(); module != modules->rend(); ++module)
        FreeLibrary(*module);
    modules->clear();
}

class LoadedDependenciesOwner final
{
  public:
    ~LoadedDependenciesOwner() { FreeModules(&modules_); }
    std::vector<HMODULE> *put() { return &modules_; }

  private:
    std::vector<HMODULE> modules_;
};

bool LoadStagedDependencies(const std::vector<StagedRecord> &records,
                            std::vector<HMODULE> *modules)
{
    if (!modules)
        return false;
    modules->clear();
    for (const auto &record : records)
    {
        if (record.kind != StagedKind::Module ||
            (!SameText(record.basename, L"openjoc_capi.dll") &&
             !SameText(record.basename, L"libbluray.dll")))
            continue;
        HMODULE module = LoadLibraryExW(record.final_path.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module || !SamePath(ModulePath(module), record.final_path))
        {
            std::fwprintf(stderr, L"staged dependency load failed: %ls (win32=%lu)\n",
                          record.basename.c_str(), static_cast<unsigned long>(GetLastError()));
            if (module)
                FreeLibrary(module);
            FreeModules(modules);
            return false;
        }
        modules->push_back(module);
    }
    return true;
}

template <typename T> class ComOwner final
{
  public:
    ComOwner() = default;
    ~ComOwner() { Release(value_); }
    ComOwner(const ComOwner &) = delete;
    ComOwner &operator=(const ComOwner &) = delete;
    T *get() const { return value_; }
    T **put()
    {
        Release(value_);
        return &value_;
    }
    T *operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void attach(T *value)
    {
        Release(value_);
        value_ = value;
    }
    T *detach()
    {
        T *value = value_;
        value_ = nullptr;
        return value;
    }

  private:
    T *value_ = nullptr;
};

CMediaType BuildStrictTarget(const LAVOpenJocOutputContract &contract)
{
    CMediaType media_type;
    if (FindLAVOpenJocOutputContract(contract.policy) != &contract ||
        contract.channel_count == 0 ||
        contract.channel_count > (std::numeric_limits<WORD>::max)() ||
        contract.windows_channel_mask == 0 ||
        contract.ffmpeg_channel_mask != contract.windows_channel_mask ||
        __popcnt(contract.windows_channel_mask) != contract.channel_count)
        return media_type;

    const std::uint64_t block_align =
        static_cast<std::uint64_t>(contract.channel_count) * sizeof(float);
    const std::uint64_t average_bytes = block_align * 48000u;
    if (block_align > (std::numeric_limits<WORD>::max)() ||
        average_bytes > (std::numeric_limits<DWORD>::max)())
        return media_type;

    WAVEFORMATEXTENSIBLE wave{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = static_cast<WORD>(contract.channel_count);
    wave.Format.nSamplesPerSec = 48000;
    wave.Format.nAvgBytesPerSec = static_cast<DWORD>(average_bytes);
    wave.Format.nBlockAlign = static_cast<WORD>(block_align);
    wave.Format.wBitsPerSample = 32;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.Samples.wValidBitsPerSample = 32;
    wave.dwChannelMask = contract.windows_channel_mask;
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    media_type.SetType(&MEDIATYPE_Audio);
    media_type.SetSubtype(&MEDIASUBTYPE_IEEE_FLOAT);
    media_type.SetSampleSize(wave.Format.nBlockAlign);
    media_type.SetTemporalCompression(FALSE);
    media_type.SetFormatType(&FORMAT_WaveFormatEx);
    if (!media_type.SetFormat(reinterpret_cast<BYTE *>(&wave), sizeof(wave)))
        media_type.InitMediaType();
    return media_type;
}

CMediaType BuildPcmType(const std::uint32_t channels, const std::uint32_t mask,
                        const bool floating_point)
{
    CMediaType media_type;
    if (channels == 0 || channels > (std::numeric_limits<WORD>::max)())
        return media_type;
    WAVEFORMATEXTENSIBLE wave{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = static_cast<WORD>(channels);
    wave.Format.nSamplesPerSec = 48000;
    wave.Format.wBitsPerSample = floating_point ? 32 : 16;
    const std::uint32_t block_align = channels * (wave.Format.wBitsPerSample / 8);
    if (block_align > (std::numeric_limits<WORD>::max)())
        return media_type;
    wave.Format.nBlockAlign = static_cast<WORD>(block_align);
    wave.Format.nAvgBytesPerSec = block_align * wave.Format.nSamplesPerSec;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.Samples.wValidBitsPerSample = wave.Format.wBitsPerSample;
    wave.dwChannelMask = mask;
    wave.SubFormat = floating_point ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    const GUID subtype = floating_point ? MEDIASUBTYPE_IEEE_FLOAT : MEDIASUBTYPE_PCM;
    media_type.SetType(&MEDIATYPE_Audio);
    media_type.SetSubtype(&subtype);
    media_type.SetSampleSize(wave.Format.nBlockAlign);
    media_type.SetTemporalCompression(FALSE);
    media_type.SetFormatType(&FORMAT_WaveFormatEx);
    if (!media_type.SetFormat(reinterpret_cast<BYTE *>(&wave), sizeof(wave)))
        media_type.InitMediaType();
    return media_type;
}

bool IsPcmType(const AM_MEDIA_TYPE &media_type)
{
    if (media_type.majortype != MEDIATYPE_Audio || media_type.formattype != FORMAT_WaveFormatEx ||
        !media_type.pbFormat || media_type.cbFormat < sizeof(WAVEFORMATEX) ||
        (media_type.subtype != MEDIASUBTYPE_IEEE_FLOAT && media_type.subtype != MEDIASUBTYPE_PCM))
        return false;
    const auto *wave = reinterpret_cast<const WAVEFORMATEX *>(media_type.pbFormat);
    return wave->nChannels > 0 && wave->nSamplesPerSec == 48000 && wave->nBlockAlign > 0;
}

class StrictCaptureSink;

struct CapturedSampleEvidence
{
    REFERENCE_TIME start = 0;
    REFERENCE_TIME stop = 0;
    long length = 0;
    long capacity = 0;
    bool discontinuity = false;
    bool sync_point = false;
    bool preroll = false;
    Digest sha256{};
    std::vector<BYTE> bytes;
    bool has_attached_type = false;
    CMediaType attached_type;
};

enum class StreamEventKind
{
    BeginFlush,
    EndFlush,
    NewSegment,
    EndOfStream,
};

struct StreamEventEvidence
{
    StreamEventKind kind = StreamEventKind::BeginFlush;
    REFERENCE_TIME start = 0;
    REFERENCE_TIME stop = 0;
    double rate = 0.0;
};

class StrictCaptureInputPin final : public CBaseInputPin
{
  public:
    StrictCaptureInputPin(StrictCaptureSink *owner, CCritSec *lock, HRESULT *status);
    HRESULT CheckMediaType(const CMediaType *media_type) override;
    STDMETHODIMP ReceiveConnection(IPin *connector, const AM_MEDIA_TYPE *media_type) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *media_type) override;
    HRESULT SetMediaType(const CMediaType *media_type) override;
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES *properties) override;
    STDMETHODIMP NotifyAllocator(IMemAllocator *allocator, BOOL read_only) override;
    STDMETHODIMP Receive(IMediaSample *sample) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override;

  private:
    StrictCaptureSink *owner_ = nullptr;
};

class StrictCaptureSink final : public CBaseFilter
{
  public:
    StrictCaptureSink(const CMediaType &expected, const bool rejection_trap,
                       std::vector<CMediaType> accepted_fallbacks, HRESULT *status,
                       const bool accept_first_audio_type = false,
                       const bool allow_type_transitions = false)
        : CBaseFilter(L"OpenJOC Strict Capture Sink", nullptr, &filter_lock_, CLSID_NULL, status),
          expected_(expected), rejection_trap_(rejection_trap),
          accepted_fallbacks_(std::move(accepted_fallbacks)), input_(this, &filter_lock_, status),
          accept_first_audio_type_(accept_first_audio_type),
          allow_type_transitions_(allow_type_transitions)
    {
        end_of_stream_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        rejection_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        running_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        activity_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if ((!end_of_stream_event_ || !rejection_event_ || !running_event_ || !activity_event_) &&
            status)
            *status = HRESULT_FROM_WIN32(GetLastError());
    }

    ~StrictCaptureSink() override
    {
        if (end_of_stream_event_)
            CloseHandle(end_of_stream_event_);
        if (rejection_event_)
            CloseHandle(rejection_event_);
        if (running_event_)
            CloseHandle(running_event_);
        if (activity_event_)
            CloseHandle(activity_event_);
    }

    int GetPinCount() override { return 1; }
    CBasePin *GetPin(const int index) override { return index == 0 ? &input_ : nullptr; }
    StrictCaptureInputPin *input() { return &input_; }
    STDMETHODIMP Pause() override
    {
        ResetEvent(running_event_);
        return CBaseFilter::Pause();
    }
    STDMETHODIMP Run(REFERENCE_TIME start) override
    {
        const HRESULT status = CBaseFilter::Run(start);
        if (SUCCEEDED(status))
            SetEvent(running_event_);
        return status;
    }
    STDMETHODIMP Stop() override
    {
        SetEvent(running_event_);
        return CBaseFilter::Stop();
    }
    bool WaitUntilRunning() const
    {
        return running_event_ && WaitForSingleObject(running_event_, 30000) == WAIT_OBJECT_0;
    }
    void UnblockRunningWait() { SetEvent(running_event_); }

    HRESULT Check(const AM_MEDIA_TYPE &media_type) const
    {
        if (accept_first_audio_type_ && !captured_first_audio_type_)
            return media_type.majortype == MEDIATYPE_Audio && media_type.pUnk == nullptr
                       ? S_OK
                       : VFW_E_TYPE_NOT_ACCEPTED;
        const bool exact = openjoc_harness_core::ExactMediaTypeEqual(expected_, media_type);
        if (rejection_trap_)
        {
            if (exact)
                return VFW_E_TYPE_NOT_ACCEPTED;
            for (const auto &fallback : accepted_fallbacks_)
            {
                if (openjoc_harness_core::ExactMediaTypeEqual(fallback, media_type))
                    return S_OK;
            }
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        if (exact)
            return S_OK;
        for (const auto &fallback : accepted_fallbacks_)
        {
            if (openjoc_harness_core::ExactMediaTypeEqual(fallback, media_type))
                return S_OK;
        }
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    HRESULT RecordQuery(const AM_MEDIA_TYPE &media_type)
    {
        if (accept_first_audio_type_ && !captured_first_audio_type_)
        {
            CAutoLock lock(&filter_lock_);
            query_accepts_.emplace_back(media_type);
            RecordMutationLocked();
            return media_type.majortype == MEDIATYPE_Audio && media_type.pUnk == nullptr
                       ? S_OK
                       : S_FALSE;
        }
        const bool exact = openjoc_harness_core::ExactMediaTypeEqual(expected_, media_type);
        if (rejection_trap_ && exact && !WaitUntilRunning())
            return S_FALSE;
        CAutoLock lock(&filter_lock_);
        query_accepts_.emplace_back(media_type);
        RecordMutationLocked();
        const HRESULT checked = Check(media_type);
        if (rejection_trap_ && exact)
        {
            rejected_stage_ = L"QueryAccept";
            rejected_raw_result_ = S_FALSE;
            rejected_normalized_result_ = NormalizeLAVOpenJocQueryAcceptResult(S_FALSE);
            SetEvent(rejection_event_);
        }
        return checked == S_OK ? S_OK : S_FALSE;
    }

    void RecordConnection(const AM_MEDIA_TYPE &media_type)
    {
        CAutoLock lock(&filter_lock_);
        if (accept_first_audio_type_ && !captured_first_audio_type_)
        {
            expected_.Set(media_type);
            captured_first_audio_type_ = true;
        }
        else if (allow_type_transitions_ && Check(media_type) == S_OK)
            expected_.Set(media_type);
        receive_connections_.emplace_back(media_type);
        RecordMutationLocked();
    }
    void RecordSetMediaType(const AM_MEDIA_TYPE &media_type)
    {
        CAutoLock lock(&filter_lock_);
        if (allow_type_transitions_ && Check(media_type) == S_OK)
            expected_.Set(media_type);
        set_media_types_.emplace_back(media_type);
        RecordMutationLocked();
    }

    void RecordAllocatorRequirements(const ALLOCATOR_PROPERTIES &properties)
    {
        CAutoLock lock(&filter_lock_);
        allocator_requested_properties_ = properties;
        allocator_requirements_recorded_ = true;
    }

    void RecordAllocator(IMemAllocator *allocator, const BOOL read_only)
    {
        CAutoLock lock(&filter_lock_);
        allocator_read_only_ = read_only;
        allocator_notified_ = allocator != nullptr;
        if (allocator)
            allocator->GetProperties(&allocator_properties_);
    }

    HRESULT RecordSample(IMediaSample *sample)
    {
        BYTE *data = nullptr;
        const long length = sample ? sample->GetActualDataLength() : -1;
        const long capacity = sample ? sample->GetSize() : -1;
        AM_MEDIA_TYPE *attached = nullptr;
        const HRESULT attached_status = sample ? sample->GetMediaType(&attached) : E_POINTER;
        const HRESULT discontinuity_status = sample ? sample->IsDiscontinuity() : E_POINTER;
        const HRESULT sync_status = sample ? sample->IsSyncPoint() : E_POINTER;
        const HRESULT preroll_status = sample ? sample->IsPreroll() : E_POINTER;
        REFERENCE_TIME start = 0;
        REFERENCE_TIME stop = 0;
        const bool has_time = sample && sample->GetTime(&start, &stop) == S_OK;
        CapturedSampleEvidence sample_evidence;
        sample_evidence.start = start;
        sample_evidence.stop = stop;
        sample_evidence.length = length;
        sample_evidence.capacity = capacity;
        sample_evidence.discontinuity = discontinuity_status == S_OK;
        sample_evidence.sync_point = sync_status == S_OK;
        sample_evidence.preroll = preroll_status == S_OK;
        const auto *wave = expected_.formattype == FORMAT_WaveFormatEx && expected_.pbFormat &&
                                   expected_.cbFormat >= sizeof(WAVEFORMATEX)
                               ? reinterpret_cast<const WAVEFORMATEX *>(expected_.pbFormat)
                               : nullptr;
        const bool flags_valid =
            (discontinuity_status == S_OK || discontinuity_status == S_FALSE) &&
            (sync_status == S_OK || sync_status == S_FALSE) &&
            (preroll_status == S_OK || preroll_status == S_FALSE) &&
            ((attached_status == S_OK && attached) || (attached_status == S_FALSE && !attached));
        const bool buffer_valid = sample && flags_valid && wave && wave->nBlockAlign > 0 && capacity > 0 &&
                                  length > 0 && length <= capacity &&
                                  length % wave->nBlockAlign == 0 &&
                                  SUCCEEDED(sample->GetPointer(&data)) && data;
        {
            CAutoLock lock(&filter_lock_);
            ++sample_observation_count_;
            RecordMutationLocked();
            if (attached_status == S_OK && attached)
            {
                sample_attached_types_.emplace_back(*attached);
                sample_evidence.has_attached_type = true;
                sample_evidence.attached_type.Set(*attached);
            }
            sample_capacities_.push_back(capacity);
            sample_lengths_.push_back(length);
            const REFERENCE_TIME previous_stop =
                timestamps_.empty() ? start : timestamps_.back().second;
            const REFERENCE_TIME timestamp_gap =
                previous_stop >= start ? previous_stop - start : start - previous_stop;
            const bool timestamp_valid = has_time && start < stop &&
                                         (timestamps_.empty() || timestamp_gap <= 1);
            sample_buffers_valid_ = sample_buffers_valid_ && buffer_valid;
            timestamps_valid_ = timestamps_valid_ && timestamp_valid;
            if (!buffer_valid || !timestamp_valid ||
                bytes_.size() > (std::numeric_limits<std::size_t>::max)() -
                                    static_cast<std::size_t>(length))
            {
                std::wprintf(L"TASK3_SAMPLE_REJECT buffer_valid=%d timestamp_valid=%d "
                             L"length=%ld capacity=%ld block_align=%u start=%lld stop=%lld "
                             L"previous_stop=%lld gap=%lld attached_hr=0x%08lx\n",
                             buffer_valid, timestamp_valid, length, capacity,
                             wave ? static_cast<unsigned int>(wave->nBlockAlign) : 0,
                             static_cast<long long>(start), static_cast<long long>(stop),
                             static_cast<long long>(previous_stop),
                             static_cast<long long>(timestamp_gap),
                             static_cast<unsigned long>(attached_status));
                if (attached)
                    DeleteMediaType(attached);
                return !buffer_valid || !timestamp_valid
                           ? E_INVALIDARG
                           : HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            }
            bytes_.insert(bytes_.end(), data, data + length);
            std::vector<BYTE> sample_bytes(data, data + length);
            if (!Sha256Bytes(sample_bytes, &sample_evidence.sha256))
            {
                if (attached)
                    DeleteMediaType(attached);
                return E_FAIL;
            }
            sample_evidence.bytes = std::move(sample_bytes);
            samples_.push_back(sample_evidence);
            ++sample_count_;
            if (m_State == State_Running)
                ++running_sample_count_;
            if (has_time)
            {
                timestamps_.emplace_back(start, stop);
                ++timestamp_observation_count_;
            }
        }
        if (attached)
            DeleteMediaType(attached);
        return S_OK;
    }

    void RecordEndOfStream()
    {
        CAutoLock lock(&filter_lock_);
        end_of_stream_ = true;
        end_of_stream_running_ = m_State == State_Running;
        ++end_of_stream_count_;
        stream_events_.push_back({StreamEventKind::EndOfStream, 0, 0, 0.0});
        SetEvent(end_of_stream_event_);
    }
    void RecordBeginFlush()
    {
        CAutoLock lock(&filter_lock_);
        ++begin_flush_count_;
        timestamps_.clear();
        stream_events_.push_back({StreamEventKind::BeginFlush, 0, 0, 0.0});
    }
    void RecordEndFlush()
    {
        CAutoLock lock(&filter_lock_);
        ++end_flush_count_;
        stream_events_.push_back({StreamEventKind::EndFlush, 0, 0, 0.0});
    }
    void RecordNewSegment(const REFERENCE_TIME start, const REFERENCE_TIME stop,
                          const double rate)
    {
        CAutoLock lock(&filter_lock_);
        ++new_segment_count_;
        timestamps_.clear();
        stream_events_.push_back({StreamEventKind::NewSegment, start, stop, rate});
    }

    void ResetQueries()
    {
        CAutoLock lock(&filter_lock_);
        query_accepts_.clear();
        rejected_stage_.clear();
        rejected_raw_result_ = S_OK;
        rejected_normalized_result_ = S_OK;
        ResetEvent(rejection_event_);
    }

    std::uint64_t PrepareQuiescence()
    {
        CAutoLock lock(&filter_lock_);
        ResetEvent(activity_event_);
        return mutation_serial_;
    }

    HANDLE end_of_stream_event() const { return end_of_stream_event_; }
    HANDLE rejection_event() const { return rejection_event_; }
    HANDLE activity_event() const { return activity_event_; }
    std::vector<CMediaType> query_accepts() const
    {
        CAutoLock lock(&filter_lock_);
        return query_accepts_;
    }
    std::vector<CMediaType> receive_connections() const
    {
        CAutoLock lock(&filter_lock_);
        return receive_connections_;
    }
    std::vector<CMediaType> sample_attached_types() const
    {
        CAutoLock lock(&filter_lock_);
        return sample_attached_types_;
    }
    std::vector<CMediaType> set_media_types() const
    {
        CAutoLock lock(&filter_lock_);
        return set_media_types_;
    }
    std::vector<BYTE> bytes() const
    {
        CAutoLock lock(&filter_lock_);
        return bytes_;
    }
    std::vector<CapturedSampleEvidence> samples() const
    {
        CAutoLock lock(&filter_lock_);
        return samples_;
    }
    std::vector<StreamEventEvidence> stream_events() const
    {
        CAutoLock lock(&filter_lock_);
        return stream_events_;
    }
    CMediaType expected_type() const
    {
        CAutoLock lock(&filter_lock_);
        return expected_;
    }
    std::uint64_t sample_count() const
    {
        CAutoLock lock(&filter_lock_);
        return sample_count_;
    }
    std::uint64_t sample_observation_count() const
    {
        CAutoLock lock(&filter_lock_);
        return sample_observation_count_;
    }
    std::uint64_t mutation_serial() const
    {
        CAutoLock lock(&filter_lock_);
        return mutation_serial_;
    }
    std::uint64_t running_sample_count() const
    {
        CAutoLock lock(&filter_lock_);
        return running_sample_count_;
    }
    std::size_t timestamp_count() const
    {
        CAutoLock lock(&filter_lock_);
        return timestamps_.size();
    }
    std::uint64_t timestamp_observation_count() const
    {
        CAutoLock lock(&filter_lock_);
        return timestamp_observation_count_;
    }
    bool end_of_stream() const
    {
        CAutoLock lock(&filter_lock_);
        return end_of_stream_;
    }
    bool end_of_stream_running() const
    {
        CAutoLock lock(&filter_lock_);
        return end_of_stream_running_;
    }
    std::uint64_t end_of_stream_count() const
    {
        CAutoLock lock(&filter_lock_);
        return end_of_stream_count_;
    }
    std::uint64_t begin_flush_count() const
    {
        CAutoLock lock(&filter_lock_);
        return begin_flush_count_;
    }
    std::uint64_t end_flush_count() const
    {
        CAutoLock lock(&filter_lock_);
        return end_flush_count_;
    }
    std::uint64_t new_segment_count() const
    {
        CAutoLock lock(&filter_lock_);
        return new_segment_count_;
    }
    void ResetCompletionForNextSegment()
    {
        CAutoLock lock(&filter_lock_);
        end_of_stream_ = false;
        ResetEvent(end_of_stream_event_);
        timestamps_.clear();
    }
    bool allocator_contract_valid() const
    {
        CAutoLock lock(&filter_lock_);
        return allocator_requirements_recorded_ && allocator_notified_ &&
               allocator_requested_properties_.cBuffers > 0 &&
               allocator_requested_properties_.cbBuffer > 0 &&
               allocator_requested_properties_.cbAlign > 0 &&
               allocator_requested_properties_.cbPrefix >= 0 &&
               allocator_properties_.cBuffers >= allocator_requested_properties_.cBuffers &&
               allocator_properties_.cbBuffer >= allocator_requested_properties_.cbBuffer &&
               allocator_properties_.cbAlign >= allocator_requested_properties_.cbAlign &&
               allocator_properties_.cbPrefix >= allocator_requested_properties_.cbPrefix;
    }
    ALLOCATOR_PROPERTIES allocator_requested_properties() const
    {
        CAutoLock lock(&filter_lock_);
        return allocator_requested_properties_;
    }
    ALLOCATOR_PROPERTIES allocator_actual_properties() const
    {
        CAutoLock lock(&filter_lock_);
        return allocator_properties_;
    }
    bool sample_contracts_valid() const
    {
        CAutoLock lock(&filter_lock_);
        return sample_buffers_valid_ && timestamps_valid_ &&
                sample_capacities_.size() == sample_count_ &&
                sample_lengths_.size() == sample_count_ &&
                timestamp_observation_count_ == sample_count_;
    }
    std::wstring rejected_stage() const
    {
        CAutoLock lock(&filter_lock_);
        return rejected_stage_;
    }
    HRESULT rejected_raw_result() const
    {
        CAutoLock lock(&filter_lock_);
        return rejected_raw_result_;
    }
    HRESULT rejected_normalized_result() const
    {
        CAutoLock lock(&filter_lock_);
        return rejected_normalized_result_;
    }

  private:
    void RecordMutationLocked()
    {
        ++mutation_serial_;
        SetEvent(activity_event_);
    }

    mutable CCritSec filter_lock_;
    CMediaType expected_;
    bool rejection_trap_ = false;
    std::vector<CMediaType> accepted_fallbacks_;
    StrictCaptureInputPin input_;
    HANDLE end_of_stream_event_ = nullptr;
    HANDLE rejection_event_ = nullptr;
    HANDLE running_event_ = nullptr;
    HANDLE activity_event_ = nullptr;
    std::vector<CMediaType> receive_connections_;
    std::vector<CMediaType> query_accepts_;
    std::vector<CMediaType> sample_attached_types_;
    std::vector<CMediaType> set_media_types_;
    std::vector<BYTE> bytes_;
    std::vector<std::pair<REFERENCE_TIME, REFERENCE_TIME>> timestamps_;
    std::vector<long> sample_capacities_;
    std::vector<long> sample_lengths_;
    std::vector<CapturedSampleEvidence> samples_;
    std::vector<StreamEventEvidence> stream_events_;
    ALLOCATOR_PROPERTIES allocator_properties_{};
    ALLOCATOR_PROPERTIES allocator_requested_properties_{};
    BOOL allocator_read_only_ = FALSE;
    bool allocator_notified_ = false;
    bool allocator_requirements_recorded_ = false;
    bool sample_buffers_valid_ = true;
    bool timestamps_valid_ = true;
    std::uint64_t sample_count_ = 0;
    std::uint64_t sample_observation_count_ = 0;
    std::uint64_t running_sample_count_ = 0;
    std::uint64_t timestamp_observation_count_ = 0;
    std::uint64_t mutation_serial_ = 0;
    bool end_of_stream_ = false;
    bool end_of_stream_running_ = false;
    std::uint64_t end_of_stream_count_ = 0;
    std::uint64_t begin_flush_count_ = 0;
    std::uint64_t end_flush_count_ = 0;
    std::uint64_t new_segment_count_ = 0;
    std::wstring rejected_stage_;
    HRESULT rejected_raw_result_ = S_OK;
    HRESULT rejected_normalized_result_ = S_OK;
    bool accept_first_audio_type_ = false;
    bool captured_first_audio_type_ = false;
    bool allow_type_transitions_ = false;
};

StrictCaptureInputPin::StrictCaptureInputPin(StrictCaptureSink *owner, CCritSec *lock,
                                             HRESULT *status)
    : CBaseInputPin(L"OpenJOC Strict Capture Input", owner, lock, status, L"In"), owner_(owner)
{
}

HRESULT StrictCaptureInputPin::CheckMediaType(const CMediaType *media_type)
{
    return owner_ && media_type ? owner_->Check(*media_type) : E_POINTER;
}

STDMETHODIMP StrictCaptureInputPin::ReceiveConnection(IPin *connector,
                                                       const AM_MEDIA_TYPE *media_type)
{
    if (!owner_ || !media_type)
        return E_POINTER;
    owner_->RecordConnection(*media_type);
    return CBaseInputPin::ReceiveConnection(connector, media_type);
}

STDMETHODIMP StrictCaptureInputPin::QueryAccept(const AM_MEDIA_TYPE *media_type)
{
    return owner_ && media_type ? owner_->RecordQuery(*media_type) : E_POINTER;
}

HRESULT StrictCaptureInputPin::SetMediaType(const CMediaType *media_type)
{
    if (!owner_ || !media_type)
        return E_POINTER;
    owner_->RecordSetMediaType(*media_type);
    return CBaseInputPin::SetMediaType(media_type);
}

STDMETHODIMP StrictCaptureInputPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES *properties)
{
    if (!properties)
        return E_POINTER;
    *properties = {};
    properties->cBuffers = 4;
    properties->cbBuffer = 1024 * 1024;
    properties->cbAlign = 1;
    if (owner_)
        owner_->RecordAllocatorRequirements(*properties);
    return S_OK;
}

STDMETHODIMP StrictCaptureInputPin::NotifyAllocator(IMemAllocator *allocator, BOOL read_only)
{
    if (owner_)
        owner_->RecordAllocator(allocator, read_only);
    return CBaseInputPin::NotifyAllocator(allocator, read_only);
}

STDMETHODIMP StrictCaptureInputPin::Receive(IMediaSample *sample)
{
    if (!owner_ || !owner_->WaitUntilRunning())
        return VFW_E_TIMEOUT;
    const HRESULT base_status = CBaseInputPin::Receive(sample);
    return SUCCEEDED(base_status) && owner_ ? owner_->RecordSample(sample) : base_status;
}

STDMETHODIMP StrictCaptureInputPin::EndOfStream()
{
    if (!owner_ || !owner_->WaitUntilRunning())
        return VFW_E_TIMEOUT;
    const HRESULT status = CBaseInputPin::EndOfStream();
    if (SUCCEEDED(status) && owner_)
        owner_->RecordEndOfStream();
    return status;
}

STDMETHODIMP StrictCaptureInputPin::BeginFlush()
{
    if (owner_)
    {
        owner_->UnblockRunningWait();
        owner_->RecordBeginFlush();
    }
    return CBaseInputPin::BeginFlush();
}

STDMETHODIMP StrictCaptureInputPin::EndFlush()
{
    const HRESULT status = CBaseInputPin::EndFlush();
    if (SUCCEEDED(status) && owner_)
        owner_->RecordEndFlush();
    return status;
}

STDMETHODIMP StrictCaptureInputPin::NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop,
                                                double rate)
{
    if (owner_)
        owner_->RecordNewSegment(start, stop, rate);
    return CBaseInputPin::NewSegment(start, stop, rate);
}

HRESULT FindSingleOwnedPin(IBaseFilter *filter, const PIN_DIRECTION direction, IPin **result)
{
    if (!filter || !result)
        return E_POINTER;
    *result = nullptr;
    ComOwner<IEnumPins> pins;
    HRESULT status = filter->EnumPins(pins.put());
    if (FAILED(status))
        return status;
    std::size_t matches = 0;
    for (;;)
    {
        IPin *pin = nullptr;
        ULONG fetched = 0;
        if (pins->Next(1, &pin, &fetched) != S_OK)
            break;
        PIN_DIRECTION actual = PINDIR_INPUT;
        PIN_INFO info{};
        const bool owned = SUCCEEDED(pin->QueryDirection(&actual)) && actual == direction &&
                           SUCCEEDED(pin->QueryPinInfo(&info)) && info.pFilter == filter;
        if (info.pFilter)
            info.pFilter->Release();
        if (owned)
        {
            ++matches;
            if (matches == 1)
            {
                *result = pin;
                continue;
            }
        }
        pin->Release();
    }
    if (matches != 1)
    {
        Release(*result);
        return VFW_E_NOT_FOUND;
    }
    return S_OK;
}

bool ReadFixtureBytes(const std::filesystem::path &path, std::vector<unsigned char> *bytes)
{
    if (!bytes)
        return false;
    std::string payload;
    if (!ReadAll(path.native(), &payload))
        return false;
    bytes->assign(payload.begin(), payload.end());
    return !bytes->empty();
}

bool BuildOracleBytes(const std::filesystem::path &raw_fixture,
                      const LAVOpenJocOutputPolicy policy, std::vector<BYTE> *bytes)
{
    if (!bytes)
        return false;
    bytes->clear();
    std::vector<unsigned char> input;
    if (!ReadFixtureBytes(raw_fixture, &input))
        return false;
    LAVOpenJocDecoder decoder;
    if (!decoder.IsAvailable() || !decoder.SetOutputPolicy(policy) ||
        decoder.Process(input.data(), input.size(), (std::numeric_limits<std::int64_t>::min)(), true) !=
            LAVOpenJocProcessResult::OpenJoc)
        return false;
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    LAVOpenJocFrame frame;
    while (decoder.ReceiveFrame(frame))
    {
        if (frame.output_contract != contract || frame.sample_rate != 48000 ||
            frame.channel_count != contract->channel_count || frame.sample_count == 0 ||
            frame.samples.size() != frame.sample_count * frame.channel_count ||
            frame.samples.size() > ((std::numeric_limits<std::size_t>::max)() - bytes->size()) /
                                       sizeof(float))
            return false;
        const BYTE *begin = reinterpret_cast<const BYTE *>(frame.samples.data());
        bytes->insert(bytes->end(), begin, begin + frame.samples.size() * sizeof(float));
    }
    return !decoder.HasError() && decoder.StreamInputBytes() == input.size() && !bytes->empty() &&
           openjoc_harness_core::FingerprintsArePairwiseDistinct(
               openjoc_harness_core::InterleavedFingerprints(*bytes, contract->channel_count));
}

bool BuildLegacyCoreOracleBytes(const std::filesystem::path &raw_fixture,
                                const LAVOpenJocOutputPolicy policy, std::vector<BYTE> *bytes)
{
    if (!bytes)
        return false;
    bytes->clear();
    std::vector<unsigned char> input;
    if (!ReadFixtureBytes(raw_fixture, &input))
        return false;
    LAVOpenJocDecoder decoder;
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    if (!contract || !decoder.IsAvailable() || !decoder.SetOutputPolicy(policy))
        return false;
    const LAVOpenJocProcessResult process = decoder.Process(
        input.data(), input.size(), (std::numeric_limits<std::int64_t>::min)(), true);
    if (process != LAVOpenJocProcessResult::OpenJoc)
        return false;
    LAVOpenJocFrame frame;
    while (decoder.ReceiveFrame(frame))
    {
        if (frame.output_contract != contract || frame.sample_rate != 48000 ||
            frame.channel_count != contract->channel_count || frame.sample_count == 0 ||
            frame.samples.size() != frame.sample_count * frame.channel_count ||
            frame.samples.size() > ((std::numeric_limits<std::size_t>::max)() - bytes->size()) /
                                       sizeof(float))
            return false;
        const BYTE *begin = reinterpret_cast<const BYTE *>(frame.samples.data());
        bytes->insert(bytes->end(), begin, begin + frame.samples.size() * sizeof(float));
    }
    return !decoder.HasError() && decoder.StreamInputBytes() == input.size() && !bytes->empty();
}

HRESULT BindRendererMoniker(const wchar_t *display_name, IBaseFilter **renderer)
{
    if (!display_name || !*display_name || !renderer)
        return E_INVALIDARG;
    *renderer = nullptr;
    ComOwner<IBindCtx> bind_context;
    HRESULT status = CreateBindCtx(0, bind_context.put());
    if (FAILED(status))
        return status;
    ULONG eaten = 0;
    ComOwner<IMoniker> moniker;
    status = MkParseDisplayName(bind_context.get(), display_name, &eaten, moniker.put());
    if (FAILED(status) || eaten != std::wcslen(display_name))
        return FAILED(status) ? status : MK_E_SYNTAX;
    return moniker->BindToObject(bind_context.get(), nullptr, IID_IBaseFilter,
                                 reinterpret_cast<void **>(renderer));
}

bool DrainGraphErrors(IMediaEvent *events, HRESULT *first_error)
{
    if (first_error)
        *first_error = S_OK;
    if (!events)
        return false;
    bool graph_error = false;
    for (;;)
    {
        long event_code = 0;
        LONG_PTR parameter1 = 0;
        LONG_PTR parameter2 = 0;
        const HRESULT status = events->GetEvent(&event_code, &parameter1, &parameter2, 0);
        if (status != S_OK)
            break;
        if (event_code == EC_USERABORT || event_code == EC_ERRORABORT ||
            event_code == EC_STREAM_ERROR_STOPPED ||
            event_code == EC_STREAM_ERROR_STILLPLAYING ||
            event_code == EC_ERROR_STILLPLAYING || event_code == EC_ERRORABORTEX)
        {
            graph_error = true;
            if (first_error && SUCCEEDED(*first_error))
                *first_error = static_cast<HRESULT>(parameter1);
        }
        events->FreeEventParams(event_code, parameter1, parameter2);
    }
    return graph_error;
}

struct NativeEventDrainEvidence
{
    HRESULT status = E_UNEXPECTED;
    HRESULT terminal_status = E_UNEXPECTED;
    HRESULT free_status = S_OK;
    std::uint64_t event_count = 0;
    std::uint64_t completion_count = 0;
    bool graph_error = false;
    HRESULT first_error = S_OK;
};

NativeEventDrainEvidence DrainNativeGraphEvents(IMediaEvent *events)
{
    NativeEventDrainEvidence result;
    if (!events)
    {
        result.status = E_POINTER;
        return result;
    }
    for (;;)
    {
        long event_code = 0;
        LONG_PTR parameter1 = 0;
        LONG_PTR parameter2 = 0;
        result.terminal_status =
            events->GetEvent(&event_code, &parameter1, &parameter2, 0);
        if (result.terminal_status != S_OK)
            break;
        ++result.event_count;
        if (event_code == EC_COMPLETE)
            ++result.completion_count;
        if (event_code == EC_USERABORT || event_code == EC_ERRORABORT ||
            event_code == EC_STREAM_ERROR_STOPPED ||
            event_code == EC_STREAM_ERROR_STILLPLAYING ||
            event_code == EC_ERROR_STILLPLAYING || event_code == EC_ERRORABORTEX)
        {
            result.graph_error = true;
            if (SUCCEEDED(result.first_error))
            {
                result.first_error = static_cast<HRESULT>(parameter1);
                if (SUCCEEDED(result.first_error))
                    result.first_error = E_FAIL;
            }
        }
        const HRESULT free_status =
            events->FreeEventParams(event_code, parameter1, parameter2);
        if (FAILED(free_status) && SUCCEEDED(result.free_status))
            result.free_status = free_status;
    }
    if (result.terminal_status == E_ABORT && SUCCEEDED(result.free_status) &&
        !result.graph_error)
        result.status = S_OK;
    else if (result.graph_error)
        result.status = result.first_error;
    else if (FAILED(result.free_status))
        result.status = result.free_status;
    else
        result.status = result.terminal_status;
    return result;
}

bool WaitForSinkQuiescence(StrictCaptureSink *sink, std::uint64_t *stable_serial)
{
    if (!sink || !stable_serial || !sink->activity_event())
        return false;
    constexpr DWORD kQuietWindowMs = 3000;
    const std::uint64_t serial = sink->PrepareQuiescence();
    const DWORD wait = WaitForSingleObject(sink->activity_event(), kQuietWindowMs);
    if (wait != WAIT_TIMEOUT || sink->mutation_serial() != serial)
        return false;
    *stable_serial = serial;
    return true;
}

bool IsExactDolbyCompressedMediaType(const AM_MEDIA_TYPE &media_type)
{
    return media_type.majortype == MEDIATYPE_Audio &&
           (media_type.subtype == kDolbyDdPlus ||
            media_type.subtype == MEDIASUBTYPE_DOLBY_AC3) &&
           media_type.formattype != GUID_NULL && media_type.cbFormat >= sizeof(WAVEFORMATEX) &&
           media_type.pbFormat != nullptr;
}

HRESULT FindSingleDolbyCompressedSourcePin(IBaseFilter *source, IPin **result,
                                           CMediaType *enumerated_type)
{
    if (!source || !result || !enumerated_type)
        return E_POINTER;
    *result = nullptr;
    enumerated_type->InitMediaType();
    ComOwner<IEnumPins> pins;
    HRESULT status = source->EnumPins(pins.put());
    if (FAILED(status))
        return status;
    std::size_t matching_pins = 0;
    for (;;)
    {
        IPin *pin = nullptr;
        ULONG fetched = 0;
        if (pins->Next(1, &pin, &fetched) != S_OK)
            break;
        PIN_DIRECTION direction = PINDIR_INPUT;
        PIN_INFO info{};
        bool matching = SUCCEEDED(pin->QueryDirection(&direction)) && direction == PINDIR_OUTPUT &&
                        SUCCEEDED(pin->QueryPinInfo(&info)) && info.pFilter == source;
        if (info.pFilter)
            info.pFilter->Release();
        CMediaType selected;
        if (matching)
        {
            ComOwner<IEnumMediaTypes> types;
            matching = SUCCEEDED(pin->EnumMediaTypes(types.put()));
            while (matching)
            {
                AM_MEDIA_TYPE *candidate = nullptr;
                ULONG type_fetched = 0;
                if (types->Next(1, &candidate, &type_fetched) != S_OK)
                    break;
                if (IsExactDolbyCompressedMediaType(*candidate))
                {
                    selected = *candidate;
                    DeleteMediaType(candidate);
                    break;
                }
                DeleteMediaType(candidate);
            }
            matching = selected.majortype != GUID_NULL;
        }
        if (matching)
        {
            ++matching_pins;
            if (matching_pins == 1)
            {
                *result = pin;
                *enumerated_type = selected;
                continue;
            }
        }
        pin->Release();
    }
    if (matching_pins != 1)
    {
        Release(*result);
        enumerated_type->InitMediaType();
        return VFW_E_NOT_FOUND;
    }
    return S_OK;
}

bool GraphContainsExactly(IGraphBuilder *graph, const std::size_t expected)
{
    if (!graph)
        return false;
    ComOwner<IEnumFilters> filters;
    if (FAILED(graph->EnumFilters(filters.put())))
        return false;
    std::size_t count = 0;
    for (;;)
    {
        IBaseFilter *filter = nullptr;
        ULONG fetched = 0;
        if (filters->Next(1, &filter, &fetched) != S_OK)
            break;
        ++count;
        filter->Release();
    }
    return count == expected;
}

bool SameControllingUnknown(IUnknown *left, IUnknown *right)
{
    if (!left || !right)
        return false;
    ComOwner<IUnknown> left_identity;
    ComOwner<IUnknown> right_identity;
    return SUCCEEDED(left->QueryInterface(IID_IUnknown,
                                          reinterpret_cast<void **>(left_identity.put()))) &&
           SUCCEEDED(right->QueryInterface(IID_IUnknown,
                                           reinterpret_cast<void **>(right_identity.put()))) &&
           left_identity.get() == right_identity.get();
}

bool TestDiagnosticsAbi(IBaseFilter *audio_filter, const bool target_lane)
{
    if (!audio_filter || !IsEqualGUID(__uuidof(ILAVOpenJocDiagnostics),
                                      kOpenJocDiagnosticsIidOracle) ||
        !IsEqualGUID(__uuidof(ILAVOpenJocDiagnostics2),
                     kOpenJocDiagnostics2IidOracle))
        return false;
    ComOwner<ILAVOpenJocDiagnostics> diagnostics;
    const HRESULT query = audio_filter->QueryInterface(
        __uuidof(ILAVOpenJocDiagnostics), reinterpret_cast<void **>(diagnostics.put()));
    ComOwner<ILAVOpenJocDiagnostics2> diagnostics2;
    const HRESULT query2 = audio_filter->QueryInterface(
        __uuidof(ILAVOpenJocDiagnostics2), reinterpret_cast<void **>(diagnostics2.put()));
    if (!target_lane)
        return query == E_NOINTERFACE && !diagnostics && query2 == E_NOINTERFACE && !diagnostics2;
    if (query != S_OK || !diagnostics ||
        !SameControllingUnknown(audio_filter, diagnostics.get()))
        return false;
    ULONGLONG classifier = 1;
    ULONGLONG stream = 1;
    if (diagnostics->GetOpenJocInputByteCounts(nullptr, &stream) != E_POINTER || stream != 1 ||
        diagnostics->GetOpenJocInputByteCounts(&classifier, nullptr) != E_POINTER || classifier != 1)
        return false;
    if (diagnostics->GetOpenJocInputByteCounts(&classifier, &stream) != S_OK ||
        classifier != 0 || stream != 0 || query2 != S_OK || !diagnostics2 ||
        !SameControllingUnknown(audio_filter, diagnostics2.get()))
        return false;

    LAVOpenJocDiagnosticReason reason = LAVOpenJocDiagnosticMalformedJocMetadata;
    BOOL warning = TRUE;
    BOOL failure_au_known = TRUE;
    ULONGLONG failure_au = 99;
    WCHAR detail[32] = L"stale";
    return diagnostics2->GetOpenJocPlaybackDiagnostics(
               &reason, &warning, &failure_au_known, &failure_au, detail, ARRAYSIZE(detail)) == S_OK &&
           reason == LAVOpenJocDiagnosticNone && warning == FALSE &&
           failure_au_known == FALSE && failure_au == 0 && detail[0] == L'\0';
}

std::wstring WindowText(HWND window)
{
    if (!window)
        return {};
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length + 1), L'\0');
    const int copied = GetWindowTextW(window, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return text;
}

class LiveStatusPageBinding final
{
  public:
    explicit LiveStatusPageBinding(IBaseFilter *audio_filter)
    {
        if (!audio_filter || FAILED(audio_filter->QueryInterface(
                                 IID_IUnknown, reinterpret_cast<void **>(identity_.put()))) ||
            FAILED(audio_filter->QueryInterface(__uuidof(ILAVOpenJocSettings),
                                                reinterpret_cast<void **>(settings_.put()))) ||
            FAILED(audio_filter->QueryInterface(__uuidof(ILAVAudioStatus),
                                                reinterpret_cast<void **>(audio_status_.put()))) ||
            FAILED(audio_filter->QueryInterface(__uuidof(ILAVOpenJocStatus),
                                                reinterpret_cast<void **>(openjoc_status_.put()))) ||
            FAILED(audio_filter->QueryInterface(__uuidof(ILAVOpenJocDiagnostics),
                                                reinterpret_cast<void **>(diagnostics_.put()))) ||
            FAILED(audio_filter->QueryInterface(__uuidof(ISpecifyPropertyPages2),
                                                reinterpret_cast<void **>(pages_.put()))))
        {
            status_ = E_NOINTERFACE;
            return;
        }
        if (!SameControllingUnknown(identity_.get(), settings_.get()) ||
            !SameControllingUnknown(identity_.get(), audio_status_.get()) ||
            !SameControllingUnknown(identity_.get(), openjoc_status_.get()) ||
            !SameControllingUnknown(identity_.get(), diagnostics_.get()) ||
            !SameControllingUnknown(identity_.get(), pages_.get()))
        {
            status_ = E_UNEXPECTED;
            return;
        }
        CAUUID page_ids{};
        status_ = pages_->GetPages(&page_ids);
        bool found_status = false;
        if (SUCCEEDED(status_))
        {
            for (ULONG index = 0; index < page_ids.cElems; ++index)
                found_status = found_status || page_ids.pElems[index] == kLavAudioStatusPage;
        }
        CoTaskMemFree(page_ids.pElems);
        if (!found_status)
        {
            status_ = E_UNEXPECTED;
            return;
        }
        status_ = pages_->CreatePage(kLavAudioStatusPage, page_.put());
        IUnknown *object = identity_.get();
        if (SUCCEEDED(status_))
            status_ = page_->SetObjects(1, &object);
        if (FAILED(status_))
            return;
        parent_ = CreateWindowExW(0, L"STATIC", L"OpenJOC hidden status host",
                                  WS_OVERLAPPED, 0, 0, 640, 480, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        if (!parent_)
        {
            status_ = HRESULT_FROM_WIN32(GetLastError());
            return;
        }
        RECT bounds{0, 0, 620, 440};
        status_ = page_->Activate(parent_, &bounds, FALSE);
        if (SUCCEEDED(status_))
        {
            active_ = true;
            page_window_ = FindWindowExW(parent_, nullptr, nullptr, nullptr);
            if (!page_window_)
                status_ = E_UNEXPECTED;
        }
    }

    ~LiveStatusPageBinding()
    {
        if (active_)
            page_->Deactivate();
        if (page_)
            page_->SetObjects(0, nullptr);
        if (parent_)
            DestroyWindow(parent_);
    }

    HRESULT status() const { return status_; }
    IUnknown *identity() const { return identity_.get(); }

    bool Verify(const LAVOpenJocOutputPolicy policy,
                const LAVOpenJocAdmissionState admission,
                const WORD channels, const DWORD sample_rate, const DWORD mask)
    {
        if (FAILED(status_) || !page_window_)
            return false;
        LAVOpenJocOutputPolicy actual_policy = LAVOpenJocOutputPolicy::Stereo;
        const char *format = nullptr;
        int actual_channels = 0;
        int actual_rate = 0;
        DWORD actual_mask = 0;
        if (settings_->GetOutputPolicy(&actual_policy) != S_OK || actual_policy != policy ||
            openjoc_status_->GetOpenJocAdmissionState() != admission ||
            audio_status_->GetOutputDetails(&format, &actual_channels, &actual_rate,
                                            &actual_mask) != S_OK ||
            !format || std::strcmp(format, "32bit Float") != 0 ||
            actual_channels != channels || actual_rate != static_cast<int>(sample_rate) ||
            actual_mask != mask)
            return false;
        SendMessageW(page_window_, WM_TIMER, 0, 0);
        const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
        wchar_t channel_text[64] = {};
        wchar_t rate_text[32] = {};
        _snwprintf_s(channel_text, _TRUNCATE, L"%u / 0x%x", channels, mask);
        _snwprintf_s(rate_text, _TRUNCATE, L"%lu", static_cast<unsigned long>(sample_rate));
        const std::wstring expected_policy =
            contract ? Utf8ToWide(contract->property_page_label) : std::wstring{};
        const std::wstring expected_admission =
        admission == LAVOpenJocAdmissionOpenJoc ? L"OpenJOC" : L"Stock decoder";
        return WindowText(GetDlgItem(page_window_, IDC_OPENJOC_STATUS_POLICY)) == expected_policy &&
               WindowText(GetDlgItem(page_window_, IDC_OPENJOC_STATUS_ADMISSION)) == expected_admission &&
               WindowText(GetDlgItem(page_window_, IDC_OUTPUT_CHANNEL)) == channel_text &&
               WindowText(GetDlgItem(page_window_, IDC_OUTPUT_SAMPLERATE)) == rate_text &&
               WindowText(GetDlgItem(page_window_, IDC_OUTPUT_FORMAT)) == L"32bit Float" &&
               WindowText(GetDlgItem(page_window_, IDC_OUTPUT_CODEC)) == L"PCM";
    }

    bool ReadCounters(ULONGLONG *classifier, ULONGLONG *stream) const
    {
        return diagnostics_ &&
               diagnostics_->GetOpenJocInputByteCounts(classifier, stream) == S_OK;
    }

  private:
    HRESULT status_ = E_FAIL;
    bool active_ = false;
    HWND parent_ = nullptr;
    HWND page_window_ = nullptr;
    ComOwner<IUnknown> identity_;
    ComOwner<ILAVOpenJocSettings> settings_;
    ComOwner<ILAVAudioStatus> audio_status_;
    ComOwner<ILAVOpenJocStatus> openjoc_status_;
    ComOwner<ILAVOpenJocDiagnostics> diagnostics_;
    ComOwner<ISpecifyPropertyPages2> pages_;
    ComOwner<IPropertyPage> page_;
};

HRESULT ConfigureTargetAudio(IBaseFilter *audio_filter, const bool eac3_passthrough = false,
                            const bool ac3_passthrough = false)
{
    ComOwner<ILAVAudioSettings> settings;
    HRESULT status = audio_filter
                         ? audio_filter->QueryInterface(
                               __uuidof(ILAVAudioSettings),
                               reinterpret_cast<void **>(settings.put()))
                         : E_POINTER;
    if (FAILED(status) || FAILED(status = settings->SetRuntimeConfig(TRUE)) ||
        FAILED(status = settings->SetFormatConfiguration(Codec_AC3, TRUE)) ||
        FAILED(status = settings->SetFormatConfiguration(Codec_EAC3, TRUE)) ||
        FAILED(status = settings->SetBitstreamConfig(Bitstream_AC3,
                                                     ac3_passthrough ? TRUE : FALSE)) ||
        FAILED(status = settings->SetBitstreamConfig(Bitstream_EAC3,
                                                     eac3_passthrough ? TRUE : FALSE)) ||
        FAILED(status = settings->SetSampleFormat(SampleFormat_FP32, TRUE)) ||
        FAILED(status = settings->SetMixingEnabled(FALSE)) ||
        FAILED(status = settings->SetOutputStandardLayout(FALSE)) ||
        FAILED(status = settings->SetOutput51LegacyLayout(FALSE)) ||
        FAILED(status = settings->SetSuppressFormatChanges(FALSE)) ||
        FAILED(status = settings->SetBitstreamingFallback(FALSE)))
        return status;
    return settings->GetFormatConfiguration(Codec_AC3) && settings->GetFormatConfiguration(Codec_EAC3) &&
                   settings->GetBitstreamConfig(Bitstream_AC3) ==
                       (ac3_passthrough ? TRUE : FALSE) &&
                    settings->GetBitstreamConfig(Bitstream_EAC3) ==
                        (eac3_passthrough ? TRUE : FALSE) &&
                   settings->GetSampleFormat(SampleFormat_FP32) &&
                   !settings->GetMixingEnabled()
               ? S_OK
               : E_UNEXPECTED;
}

HRESULT SetOpenJocPolicy(IBaseFilter *audio_filter, const LAVOpenJocOutputPolicy policy)
{
    ComOwner<ILAVOpenJocSettings> settings;
    HRESULT status = audio_filter
                         ? audio_filter->QueryInterface(
                               __uuidof(ILAVOpenJocSettings),
                               reinterpret_cast<void **>(settings.put()))
                         : E_POINTER;
    if (FAILED(status) || FAILED(status = settings->SetOutputPolicy(policy)))
        return status;
    LAVOpenJocOutputPolicy actual = LAVOpenJocOutputPolicy::Stereo;
    return SUCCEEDED(status = settings->GetOutputPolicy(&actual)) && actual == policy
               ? S_OK
               : (FAILED(status) ? status : E_UNEXPECTED);
}

bool CurrentFileMatches(IFileSourceFilter *source, const std::filesystem::path &fixture)
{
    if (!source || !fixture.is_absolute())
        return false;
    LPOLESTR current = nullptr;
    AM_MEDIA_TYPE media_type{};
    const HRESULT status = source->GetCurFile(&current, &media_type);
    const std::wstring expected = FinalPathForFile(fixture);
    const std::wstring actual = current ? FinalPathForFile(current) : std::wstring{};
    if (current)
        CoTaskMemFree(current);
    FreeMediaType(media_type);
    return status == S_OK && !expected.empty() && SamePath(expected, actual);
}

bool ExactConnectionTypes(IPin *output, IPin *input, const AM_MEDIA_TYPE &expected);

HRESULT CreateGraphForFixture(const PrivateComModule &audio_module,
                              const PrivateComModule &splitter_module,
                              const std::filesystem::path &fixture,
                              const LAVOpenJocOutputPolicy policy,
                               const bool configure_policy, IGraphBuilder **graph,
                              IBaseFilter **source_filter, IBaseFilter **audio_filter,
                              IPin **source_output_pin, IPin **audio_input_pin,
                              IPin **audio_output, CMediaType *exact_eac3_type,
                              const bool eac3_passthrough = false,
                               const bool configure_runtime = true,
                               const bool ac3_passthrough = false)
{
    if (!graph || !source_filter || !audio_filter || !source_output_pin || !audio_input_pin ||
        !audio_output || !exact_eac3_type || !fixture.is_absolute())
        return E_INVALIDARG;
    *graph = nullptr;
    *source_filter = nullptr;
    *audio_filter = nullptr;
    *source_output_pin = nullptr;
    *audio_input_pin = nullptr;
    *audio_output = nullptr;
    exact_eac3_type->InitMediaType();
    HRESULT status = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IGraphBuilder, reinterpret_cast<void **>(graph));
    if (FAILED(status))
        return status;
    if (FAILED(status = splitter_module.CreateInstance(
                   IID_IBaseFilter, reinterpret_cast<void **>(source_filter))) ||
        FAILED(status = audio_module.CreateInstance(IID_IBaseFilter,
                                                     reinterpret_cast<void **>(audio_filter))))
        return status;
    if (FAILED(status = (*graph)->AddFilter(*source_filter, L"Private LAV Splitter Source")) ||
        FAILED(status = (*graph)->AddFilter(*audio_filter, L"Private LAV Audio")))
        return status;

    ComOwner<ILAVFSettings> splitter_settings;
    status = (*source_filter)->QueryInterface(__uuidof(ILAVFSettings),
                                               reinterpret_cast<void **>(splitter_settings.put()));
    if (FAILED(status) || FAILED(status = splitter_settings->SetRuntimeConfig(TRUE)) ||
         (configure_runtime &&
          FAILED(status = ConfigureTargetAudio(*audio_filter, eac3_passthrough,
                                               ac3_passthrough))) ||
        (configure_policy && FAILED(status = SetOpenJocPolicy(*audio_filter, policy))))
        return status;

    ComOwner<IFileSourceFilter> file_source;
    status = (*source_filter)->QueryInterface(IID_IFileSourceFilter,
                                               reinterpret_cast<void **>(file_source.put()));
    if (FAILED(status) || FAILED(status = file_source->Load(fixture.c_str(), nullptr)))
        return status;
    if (!CurrentFileMatches(file_source.get(), fixture))
        return E_UNEXPECTED;

    if (FAILED(status = FindSingleDolbyCompressedSourcePin(*source_filter, source_output_pin,
                                                           exact_eac3_type)) ||
        FAILED(status = FindSingleOwnedPin(*audio_filter, PINDIR_INPUT, audio_input_pin)) ||
        FAILED(status = (*graph)->ConnectDirect(*source_output_pin, *audio_input_pin,
                                                exact_eac3_type)) ||
        !ExactConnectionTypes(*source_output_pin, *audio_input_pin, *exact_eac3_type) ||
        FAILED(status = FindSingleOwnedPin(*audio_filter, PINDIR_OUTPUT, audio_output)))
        return FAILED(status) ? status : E_UNEXPECTED;
    if (!GraphContainsExactly(*graph, 2))
        return E_UNEXPECTED;
    return S_OK;
}

HRESULT AttachCaptureSink(IGraphBuilder *graph, IPin *audio_output, StrictCaptureSink *sink,
                          const AM_MEDIA_TYPE &initial_type)
{
    if (!graph || !audio_output || !sink)
        return E_POINTER;
    HRESULT status = graph->AddFilter(static_cast<IBaseFilter *>(sink), L"Strict Capture Sink");
    if (FAILED(status))
        return status;
    return graph->ConnectDirect(audio_output, static_cast<IPin *>(sink->input()), &initial_type);
}

bool ExactConnectionTypes(IPin *output, IPin *input, const AM_MEDIA_TYPE &expected)
{
    if (!output || !input)
        return false;
    CMediaType output_type;
    CMediaType input_type;
    return output->ConnectionMediaType(&output_type) == S_OK &&
           input->ConnectionMediaType(&input_type) == S_OK &&
           openjoc_harness_core::ExactMediaTypeEqual(expected, output_type) &&
           openjoc_harness_core::ExactMediaTypeEqual(expected, input_type);
}

bool IsIec61937Eac3Type(const AM_MEDIA_TYPE &media_type)
{
    if (media_type.majortype != MEDIATYPE_Audio ||
        media_type.subtype != MEDIASUBTYPE_PCM || media_type.bFixedSizeSamples != TRUE ||
        media_type.bTemporalCompression != FALSE || media_type.lSampleSize != 1 ||
        media_type.pUnk != nullptr ||
        media_type.formattype != FORMAT_WaveFormatEx || !media_type.pbFormat ||
        media_type.cbFormat != sizeof(WAVEFORMATEXTENSIBLE))
        return false;
    const auto *wave = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(media_type.pbFormat);
    return wave->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           wave->Format.cbSize == sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
           wave->Format.nChannels == 2 && wave->Format.nSamplesPerSec == 192000 &&
           wave->Format.wBitsPerSample == 16 && wave->Format.nBlockAlign == 4 &&
           wave->Format.nAvgBytesPerSec == 768000 &&
           wave->Samples.wValidBitsPerSample == 16 && wave->dwChannelMask == 3 &&
           wave->SubFormat == KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS;
}

bool IsIec61937Ac3Type(const AM_MEDIA_TYPE &media_type)
{
    if (media_type.majortype != MEDIATYPE_Audio || media_type.subtype != MEDIASUBTYPE_PCM ||
        media_type.formattype != FORMAT_WaveFormatEx || !media_type.pbFormat ||
        media_type.cbFormat < sizeof(WAVEFORMATEX))
        return false;
    const auto *wave = reinterpret_cast<const WAVEFORMATEX *>(media_type.pbFormat);
    return wave->wFormatTag == WAVE_FORMAT_DOLBY_AC3_SPDIF && wave->nChannels == 2 &&
           wave->nSamplesPerSec == 48000 && wave->wBitsPerSample == 16 &&
           wave->nBlockAlign == 4 && wave->nAvgBytesPerSec == 192000 &&
           media_type.lSampleSize == 1;
}

bool PairwiseDistinctPcmChannelDigests(const AM_MEDIA_TYPE &media_type,
                                       const std::vector<BYTE> &bytes,
                                       std::vector<Digest> *digests)
{
    if (!digests || media_type.majortype != MEDIATYPE_Audio ||
        media_type.subtype != MEDIASUBTYPE_IEEE_FLOAT ||
        media_type.bFixedSizeSamples != TRUE || media_type.bTemporalCompression != FALSE ||
        media_type.lSampleSize != 24 || media_type.pUnk != nullptr ||
        media_type.formattype != FORMAT_WaveFormatEx || !media_type.pbFormat ||
        media_type.cbFormat != sizeof(WAVEFORMATEXTENSIBLE))
        return false;
    const auto *wave_ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(media_type.pbFormat);
    const auto *wave = &wave_ext->Format;
    if (wave->wFormatTag != WAVE_FORMAT_EXTENSIBLE || wave->cbSize != 22 ||
        wave->nChannels != 6 || wave->nSamplesPerSec != 48000 ||
        wave->wBitsPerSample != 32 || wave->nBlockAlign != 24 ||
        wave->nAvgBytesPerSec != 1152000 || wave_ext->Samples.wValidBitsPerSample != 32 ||
        wave_ext->dwChannelMask != 0x0000060fu ||
        (wave_ext->dwChannelMask & SPEAKER_LOW_FREQUENCY) == 0 ||
        wave_ext->SubFormat != MEDIASUBTYPE_IEEE_FLOAT)
        return false;
    bool nonzero_pcm = false;
    if (bytes.size() % sizeof(float) != 0)
        return false;
    for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(float))
    {
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        nonzero_pcm = nonzero_pcm || value != 0.0f;
    }
    if (!nonzero_pcm)
        return false;
    const auto channels = openjoc_harness_core::InterleavedChannelBytes(bytes, wave->nChannels);
    if (wave->nChannels < 2 || channels.size() != wave->nChannels)
        return false;
    digests->assign(channels.size(), Digest{});
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        if (!Sha256Bytes(channels[channel], &(*digests)[channel]))
            return false;
    }
    for (std::size_t left = 0; left < digests->size(); ++left)
    {
        for (std::size_t right = left + 1; right < digests->size(); ++right)
        {
            if ((*digests)[left] == (*digests)[right])
                return false;
        }
    }
    return true;
}

bool WriteTask3CaptureEvidence(const std::filesystem::path &path,
                               const FixtureIdentity &fixture,
                               const StagedRecord &audio_module,
                               const StagedRecord &splitter_module,
                               const AM_MEDIA_TYPE &source_type,
                               const StrictCaptureSink &sink,
                               const bool target_lane,
                               const LAVOpenJocOutputPolicy policy,
                               const bool passthrough)
{
    if (!path.is_absolute() || std::filesystem::exists(path) ||
        !FixtureIdentityMatches(fixture) ||
        static_cast<std::uint32_t>(policy) >= LAV_OPENJOC_OUTPUT_CONTRACT_COUNT)
        return false;
    const CMediaType output_type = sink.expected_type();
    const std::string source = SerializeMediaType(source_type);
    const std::string output = SerializeMediaType(output_type);
    const auto samples = sink.samples();
    const auto bytes = sink.bytes();
    Digest aggregate{};
    if (source.empty() || output.empty() || samples.empty() ||
        !Sha256Bytes(bytes, &aggregate))
        return false;
    std::vector<Digest> channel_digests;
    if (passthrough ? !IsIec61937Eac3Type(output_type)
                    : !PairwiseDistinctPcmChannelDigests(output_type, bytes, &channel_digests))
        return false;

    std::ostringstream payload;
    const std::string audio_path = WideToUtf8(audio_module.final_path);
    const std::string splitter_path = WideToUtf8(splitter_module.final_path);
    const std::string fixture_path = WideToUtf8(fixture.final_path.native());
    if (audio_path.empty() || splitter_path.empty() || fixture_path.empty())
        return false;
    payload << "TASK3_EVIDENCE_V2\nLANE\t" << (target_lane ? "target" : "pristine")
            << "\nPOLICY\t" << static_cast<unsigned int>(policy)
            << "\nAUDIO_MODULE\t" << audio_path << '\t' << DigestHex(audio_module.sha256)
            << "\nSPLITTER_MODULE\t" << splitter_path << '\t'
            << DigestHex(splitter_module.sha256) << "\nFIXTURE\t" << fixture_path << '\t'
            << DigestHex(fixture.sha256)
            << "\nMODE\t" << (passthrough ? "EAC3_PASSTHROUGH" : "STOCK_EAC3_PCM")
            << "\nSOURCE_MEDIA_TYPE\t" << source << "\nOUTPUT_MEDIA_TYPE\t" << output
            << "\nSAMPLE_COUNT\t" << samples.size() << "\n";
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const auto &sample = samples[index];
        const std::string attached = sample.has_attached_type
                                         ? SerializeMediaType(sample.attached_type)
                                         : std::string("NONE");
        if (attached.empty())
            return false;
        payload << "SAMPLE\t" << index << '\t' << sample.length << '\t' << sample.capacity << '\t'
                << sample.start << '\t' << sample.stop << '\t' << sample.discontinuity << '\t' << sample.sync_point << '\t'
                << sample.preroll << '\t' << DigestHex(sample.sha256) << '\t'
                << BytesHex(sample.bytes.data(), sample.bytes.size()) << '\t' << attached << "\n";
    }
    payload << "AGGREGATE\t" << bytes.size() << '\t' << DigestHex(aggregate) << "\n";
    for (std::size_t channel = 0; channel < channel_digests.size(); ++channel)
        payload << "CHANNEL\t" << channel << '\t' << DigestHex(channel_digests[channel]) << "\n";
    payload << "EOS\t" << sink.end_of_stream_count() << '\t' << sink.end_of_stream_running()
            << "\nEVENTS\t" << sink.begin_flush_count() << '\t' << sink.end_flush_count() << '\t'
            << sink.new_segment_count() << "\n";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const std::string text = payload.str();
    const bool ok = WriteAll(file, text) && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

bool ParseUnsignedDecimal(const std::string &text, std::uint64_t *value)
{
    if (!value || text.empty())
        return false;
    std::uint64_t parsed = 0;
    for (const char character : text)
    {
        if (character < '0' || character > '9')
            return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

bool ParseSignedDecimal(const std::string &text, std::int64_t *value)
{
    if (!value || text.empty())
        return false;
    const bool negative = text.front() == '-';
    const std::string magnitude_text = negative ? text.substr(1) : text;
    std::uint64_t magnitude = 0;
    if (!ParseUnsignedDecimal(magnitude_text, &magnitude))
        return false;
    const std::uint64_t negative_limit =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1;
    if ((!negative && magnitude > static_cast<std::uint64_t>(
                                      (std::numeric_limits<std::int64_t>::max)())) ||
        (negative && magnitude > negative_limit))
        return false;
    *value = negative
                 ? (magnitude == negative_limit ? (std::numeric_limits<std::int64_t>::min)()
                                                : -static_cast<std::int64_t>(magnitude))
                 : static_cast<std::int64_t>(magnitude);
    return true;
}

bool ParseBooleanField(const std::string &text, bool *value)
{
    if (!value || (text != "0" && text != "1"))
        return false;
    *value = text == "1";
    return true;
}

std::vector<std::string> SplitDelimited(const std::string &text, const char delimiter)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t next = text.find(delimiter, start);
        fields.push_back(text.substr(start, next == std::string::npos ? next : next - start));
        if (next == std::string::npos)
            return fields;
        start = next + 1;
    }
}

bool ParseHexBytes(const std::string &text, std::vector<BYTE> *bytes)
{
    if (!bytes || text.size() % 2 != 0)
        return false;
    auto nibble = [](const char value) {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    };
    bytes->assign(text.size() / 2, 0);
    for (std::size_t index = 0; index < bytes->size(); ++index)
    {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        (*bytes)[index] = static_cast<BYTE>((high << 4) | low);
    }
    return true;
}

bool ParseSerializedMediaType(const std::string &text, CMediaType *media_type)
{
    if (!media_type)
        return false;
    const auto fields = SplitDelimited(text, '|');
    if (fields.size() != 8)
        return false;
    GUID major{};
    GUID subtype{};
    GUID format_type{};
    const std::wstring major_text = Utf8ToWide(fields[0]);
    const std::wstring subtype_text = Utf8ToWide(fields[1]);
    const std::wstring format_text = Utf8ToWide(fields[5]);
    std::uint64_t fixed = 0;
    std::uint64_t temporal = 0;
    std::uint64_t sample_size = 0;
    std::uint64_t format_size = 0;
    std::vector<BYTE> format;
    if (major_text.empty() || subtype_text.empty() || format_text.empty() ||
        CLSIDFromString(major_text.c_str(), &major) != S_OK ||
        CLSIDFromString(subtype_text.c_str(), &subtype) != S_OK ||
        CLSIDFromString(format_text.c_str(), &format_type) != S_OK ||
        !ParseUnsignedDecimal(fields[2], &fixed) || fixed > 1 ||
        !ParseUnsignedDecimal(fields[3], &temporal) || temporal > 1 ||
        !ParseUnsignedDecimal(fields[4], &sample_size) ||
        sample_size > (std::numeric_limits<ULONG>::max)() ||
        !ParseUnsignedDecimal(fields[6], &format_size) ||
        format_size > (std::numeric_limits<ULONG>::max)() ||
        !ParseHexBytes(fields[7], &format) || format.size() != format_size)
        return false;
    CMediaType parsed;
    parsed.majortype = major;
    parsed.subtype = subtype;
    parsed.bFixedSizeSamples = static_cast<BOOL>(fixed);
    parsed.bTemporalCompression = static_cast<BOOL>(temporal);
    parsed.lSampleSize = static_cast<ULONG>(sample_size);
    parsed.formattype = format_type;
    parsed.pUnk = nullptr;
    if (!format.empty())
    {
        BYTE *destination = parsed.AllocFormatBuffer(static_cast<ULONG>(format.size()));
        if (!destination)
            return false;
        std::copy(format.begin(), format.end(), destination);
    }
    if (SerializeMediaType(parsed) != text)
        return false;
    media_type->Set(parsed);
    return true;
}

bool ValidateRecordedFileIdentity(const std::string &path_text, const std::string &digest_text,
                                  const wchar_t *expected_basename)
{
    Digest expected{};
    const std::wstring path = Utf8ToWide(path_text);
    const std::wstring final_path = FinalPathForFile(path);
    Digest actual{};
    return !path.empty() && !final_path.empty() && SamePath(path, final_path) &&
           (!expected_basename || SameText(std::filesystem::path(final_path).filename().native(),
                                           expected_basename)) &&
           ParseDigest(digest_text, &expected) && Sha256File(final_path, &actual) &&
           actual == expected;
}

struct ParsedTask3Evidence
{
    std::string behavioral_payload;
};

bool ReadTask3Evidence(const std::filesystem::path &path, std::string *payload)
{
    constexpr LONGLONG kMaximumEvidenceBytes = 64ll * 1024ll * 1024ll;
    if (!payload)
        return false;
    payload->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
              size.QuadPart <= kMaximumEvidenceBytes &&
              size.QuadPart <= (std::numeric_limits<DWORD>::max)();
    if (ok)
    {
        payload->resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, payload->data(), static_cast<DWORD>(payload->size()), &read, nullptr) &&
             read == payload->size();
    }
    CloseHandle(file);
    if (!ok)
        payload->clear();
    return ok;
}

bool ParseTask3Evidence(const std::filesystem::path &path, const bool expect_target,
                        const LAVOpenJocOutputPolicy expected_policy,
                        const bool expected_passthrough, ParsedTask3Evidence *parsed)
{
    if (!parsed || !path.is_absolute() ||
        static_cast<std::uint32_t>(expected_policy) >= LAV_OPENJOC_OUTPUT_CONTRACT_COUNT)
        return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    std::string payload;
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_READONLY) == 0 ||
        !ReadTask3Evidence(path, &payload) || payload.empty() || payload.back() != '\n' ||
        payload.find('\r') != std::string::npos)
        return false;
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < payload.size())
    {
        const std::size_t end = payload.find('\n', start);
        if (end == std::string::npos || end == start)
            return false;
        lines.push_back(payload.substr(start, end - start));
        start = end + 1;
    }
    if (lines.size() < 13 || lines[0] != "TASK3_EVIDENCE_V2")
        return false;
    std::size_t cursor = 1;
    const auto require_fields = [&](const char *name, const std::size_t count,
                                    std::vector<std::string> *fields) {
        if (!fields || cursor >= lines.size())
            return false;
        *fields = SplitTabs(lines[cursor++]);
        return fields->size() == count && (*fields)[0] == name;
    };
    std::vector<std::string> fields;
    if (!require_fields("LANE", 2, &fields) ||
        fields[1] != (expect_target ? "target" : "pristine"))
        return false;
    if (!require_fields("POLICY", 2, &fields))
        return false;
    std::uint64_t policy = 0;
    if (!ParseUnsignedDecimal(fields[1], &policy) ||
        policy != static_cast<unsigned int>(expected_policy))
        return false;
    if (!require_fields("AUDIO_MODULE", 3, &fields) ||
        !ValidateRecordedFileIdentity(fields[1], fields[2], L"LAVAudio.ax"))
        return false;
    if (!require_fields("SPLITTER_MODULE", 3, &fields) ||
        !ValidateRecordedFileIdentity(fields[1], fields[2], L"LAVSplitter.ax"))
        return false;
    const std::size_t behavioral_begin = cursor;
    if (!require_fields("FIXTURE", 3, &fields) ||
        !ValidateRecordedFileIdentity(fields[1], fields[2], nullptr))
        return false;
    const std::wstring fixture_basename =
        std::filesystem::path(Utf8ToWide(fields[1])).filename().native();
    const bool fixture_name_valid = expected_passthrough
                                        ? (SameText(fixture_basename, L"joc.multi.ec3") ||
                                           SameText(fixture_basename, L"joc.multi.mp4"))
                                        : (SameText(fixture_basename,
                                                    L"ordinary.fingerprint.eac3") ||
                                           SameText(fixture_basename,
                                                    L"ordinary.fingerprint.mp4") ||
                                           SameText(fixture_basename,
                                                    L"ordinary.fingerprint.ac3"));
    if (!fixture_name_valid || !require_fields("MODE", 2, &fields) ||
        fields[1] != (expected_passthrough ? "EAC3_PASSTHROUGH" : "STOCK_EAC3_PCM"))
        return false;
    CMediaType source_type;
    if (!require_fields("SOURCE_MEDIA_TYPE", 2, &fields) ||
        !ParseSerializedMediaType(fields[1], &source_type) ||
        !IsExactDolbyCompressedMediaType(source_type))
        return false;
    CMediaType output_type;
    if (!require_fields("OUTPUT_MEDIA_TYPE", 2, &fields) ||
        !ParseSerializedMediaType(fields[1], &output_type))
        return false;
    if (!require_fields("SAMPLE_COUNT", 2, &fields))
        return false;
    std::uint64_t sample_count = 0;
    if (!ParseUnsignedDecimal(fields[1], &sample_count) || sample_count == 0 ||
        sample_count > 1000000)
        return false;
    const auto *output_wave = output_type.formattype == FORMAT_WaveFormatEx &&
                                      output_type.pbFormat &&
                                      output_type.cbFormat >= sizeof(WAVEFORMATEX)
                                  ? reinterpret_cast<const WAVEFORMATEX *>(output_type.pbFormat)
                                  : nullptr;
    if (!output_wave || output_wave->nBlockAlign == 0)
        return false;
    std::vector<BYTE> aggregate_bytes;
    REFERENCE_TIME previous_stop = 0;
    for (std::uint64_t index = 0; index < sample_count; ++index)
    {
        if (!require_fields("SAMPLE", 12, &fields))
            return false;
        std::uint64_t recorded_index = 0;
        std::uint64_t length = 0;
        std::uint64_t capacity = 0;
        std::int64_t sample_start = 0;
        std::int64_t sample_stop = 0;
        bool discontinuity = false;
        bool sync = false;
        bool preroll = false;
        Digest recorded_digest{};
        Digest actual_digest{};
        std::vector<BYTE> sample_bytes;
        CMediaType attached;
        const bool has_attached = fields[11] != "NONE";
        if (!ParseUnsignedDecimal(fields[1], &recorded_index) || recorded_index != index ||
            !ParseUnsignedDecimal(fields[2], &length) || length == 0 ||
            length > static_cast<std::uint64_t>((std::numeric_limits<long>::max)()) ||
            length % output_wave->nBlockAlign != 0 ||
            !ParseUnsignedDecimal(fields[3], &capacity) || capacity < length ||
            capacity > static_cast<std::uint64_t>((std::numeric_limits<long>::max)()) ||
            !ParseSignedDecimal(fields[4], &sample_start) ||
            !ParseSignedDecimal(fields[5], &sample_stop) || sample_start >= sample_stop ||
            (index != 0 &&
             (sample_start > previous_stop ? sample_start - previous_stop
                                           : previous_stop - sample_start) > 1) ||
            !ParseBooleanField(fields[6], &discontinuity) ||
            !ParseBooleanField(fields[7], &sync) ||
            !ParseBooleanField(fields[8], &preroll) ||
            !ParseDigest(fields[9], &recorded_digest) ||
            !ParseHexBytes(fields[10], &sample_bytes) || sample_bytes.size() != length ||
            !Sha256Bytes(sample_bytes, &actual_digest) || actual_digest != recorded_digest ||
            (has_attached &&
             (!ParseSerializedMediaType(fields[11], &attached) ||
              !openjoc_harness_core::ExactMediaTypeEqual(output_type, attached))) ||
            aggregate_bytes.size() > (std::numeric_limits<std::size_t>::max)() -
                                         sample_bytes.size())
            return false;
        previous_stop = sample_stop;
        aggregate_bytes.insert(aggregate_bytes.end(), sample_bytes.begin(), sample_bytes.end());
    }
    if (!require_fields("AGGREGATE", 3, &fields))
        return false;
    std::uint64_t aggregate_size = 0;
    Digest aggregate_digest{};
    Digest actual_aggregate_digest{};
    if (!ParseUnsignedDecimal(fields[1], &aggregate_size) ||
        aggregate_size != aggregate_bytes.size() || !ParseDigest(fields[2], &aggregate_digest) ||
        !Sha256Bytes(aggregate_bytes, &actual_aggregate_digest) ||
        actual_aggregate_digest != aggregate_digest)
        return false;
    std::vector<Digest> expected_channels;
    if (expected_passthrough)
    {
        if (!IsIec61937Eac3Type(output_type))
            return false;
    }
    else if (!PairwiseDistinctPcmChannelDigests(output_type, aggregate_bytes,
                                                &expected_channels) ||
             expected_channels.size() != 6)
        return false;
    for (std::size_t channel = 0; channel < expected_channels.size(); ++channel)
    {
        if (!require_fields("CHANNEL", 3, &fields))
            return false;
        std::uint64_t recorded_channel = 0;
        Digest recorded_digest{};
        if (!ParseUnsignedDecimal(fields[1], &recorded_channel) || recorded_channel != channel ||
            !ParseDigest(fields[2], &recorded_digest) ||
            recorded_digest != expected_channels[channel])
            return false;
    }
    if (!require_fields("EOS", 3, &fields))
        return false;
    std::uint64_t eos_count = 0;
    bool eos_running = false;
    if (!ParseUnsignedDecimal(fields[1], &eos_count) || eos_count != 1 ||
        !ParseBooleanField(fields[2], &eos_running) || !eos_running)
        return false;
    if (!require_fields("EVENTS", 4, &fields))
        return false;
    std::uint64_t begin_flush = 0;
    std::uint64_t end_flush = 0;
    std::uint64_t new_segment = 0;
    if (!ParseUnsignedDecimal(fields[1], &begin_flush) ||
        !ParseUnsignedDecimal(fields[2], &end_flush) || begin_flush != end_flush ||
        !ParseUnsignedDecimal(fields[3], &new_segment) || new_segment == 0 ||
        cursor != lines.size())
        return false;
    std::ostringstream behavioral;
    for (std::size_t index = behavioral_begin; index < lines.size(); ++index)
        behavioral << lines[index] << '\n';
    parsed->behavioral_payload = behavioral.str();
    return !parsed->behavioral_payload.empty();
}

bool CompareTask3Evidence(const std::filesystem::path &target_path,
                          const std::filesystem::path &pristine_path,
                          const LAVOpenJocOutputPolicy expected_policy,
                          const bool expected_passthrough)
{
    if (!target_path.is_absolute() || !pristine_path.is_absolute() ||
        SamePath(FinalPathForFile(target_path), FinalPathForFile(pristine_path)))
        return false;
    ParsedTask3Evidence target;
    ParsedTask3Evidence pristine;
    const bool target_valid =
        ParseTask3Evidence(target_path, true, expected_policy, expected_passthrough, &target);
    const bool pristine_valid =
        ParseTask3Evidence(pristine_path, false, LAVOpenJocOutputPolicy::Stereo,
                           expected_passthrough, &pristine);
    if (!target_valid || !pristine_valid)
        std::fwprintf(stderr, L"TASK3_EVIDENCE_PARSE target=%d pristine=%d\n",
                      target_valid ? 1 : 0, pristine_valid ? 1 : 0);
    else if (target.behavioral_payload != pristine.behavioral_payload)
        std::fwprintf(stderr, L"TASK3_EVIDENCE_BEHAVIOR_MISMATCH target_bytes=%zu pristine_bytes=%zu\n",
                      target.behavioral_payload.size(), pristine.behavioral_payload.size());
    return target_valid && pristine_valid && target.behavioral_payload == pristine.behavioral_payload;
}

HRESULT RunTask3StockOrPassthroughWorker(const std::filesystem::path &runtime_dir,
                                         const std::filesystem::path &manifest_path,
                                         const std::filesystem::path &fixture_path,
                                         const std::filesystem::path &evidence_path,
                                         const bool target_lane,
                                         const LAVOpenJocOutputPolicy policy,
                                         const bool passthrough)
{
    std::vector<StagedRecord> records;
    if (!ReadStagedManifest(runtime_dir, manifest_path, &records))
        return E_INVALIDARG;
    const StagedRecord *audio_record = FindRecord(records, StagedKind::Module, L"LAVAudio.ax");
    const StagedRecord *splitter_record = FindRecord(records, StagedKind::Module, L"LAVSplitter.ax");
    FixtureIdentity fixture;
    if (!audio_record || !splitter_record || !BuildFixtureIdentity(fixture_path, &fixture))
        return E_INVALIDARG;
    LoadedDependenciesOwner dependencies;
    const std::wstring runtime_final = FinalPathForFile(runtime_dir);
    ScopedActivationContext activation(audio_record->final_path, runtime_final);
    if (!activation.active())
        return HRESULT_FROM_WIN32(GetLastError());
    PrivateComModule audio(audio_record->final_path,
                           target_lane ? kTargetLavAudio : kPristineLavAudio);
    PrivateComModule splitter(splitter_record->final_path, kLavSplitterSource);
    if (FAILED(audio.status()) || FAILED(splitter.status()))
        return E_UNEXPECTED;
    if (!LoadStagedDependencies(records, dependencies.put()))
        return E_UNEXPECTED;

    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source_filter;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType exact_eac3;
    HRESULT status = CreateGraphForFixture(
        audio, splitter, fixture.final_path, policy, target_lane, graph.put(),
        source_filter.put(), audio_filter.put(), source_output.put(), audio_input.put(),
        audio_output.put(), &exact_eac3, passthrough);
    ComOwner<ILAVOpenJocDiagnostics> diagnostics;
    ULONGLONG initial_classifier = 0;
    ULONGLONG initial_stream = 0;
    if (SUCCEEDED(status) && target_lane)
    {
        status = audio_filter->QueryInterface(__uuidof(ILAVOpenJocDiagnostics),
                                              reinterpret_cast<void **>(diagnostics.put()));
        if (SUCCEEDED(status) &&
            (!SameControllingUnknown(audio_filter.get(), diagnostics.get()) ||
             FAILED(status = diagnostics->GetOpenJocInputByteCounts(&initial_classifier,
                                                                     &initial_stream)) ||
             initial_classifier != 0 || initial_stream != 0))
            status = E_UNEXPECTED;
    }
    if (SUCCEEDED(status) && !target_lane)
    {
        void *unexpected = nullptr;
        if (audio_filter->QueryInterface(__uuidof(ILAVOpenJocDiagnostics), &unexpected) !=
                E_NOINTERFACE ||
            unexpected)
        {
            if (unexpected)
                static_cast<IUnknown *>(unexpected)->Release();
            status = E_UNEXPECTED;
        }
    }

    HRESULT sink_status = S_OK;
    auto *sink =
        new (std::nothrow) StrictCaptureSink(CMediaType{}, false, {}, &sink_status, true);
    if (SUCCEEDED(status) && (!sink || FAILED(sink_status)))
        status = FAILED(sink_status) ? sink_status : E_OUTOFMEMORY;
    ComOwner<IBaseFilter> sink_owner;
    if (SUCCEEDED(status))
    {
        sink->AddRef();
        sink_owner.attach(static_cast<IBaseFilter *>(sink));
    }
    else if (sink)
    {
        delete sink;
        sink = nullptr;
    }
    if (SUCCEEDED(status))
    {
        status = graph->AddFilter(static_cast<IBaseFilter *>(sink), L"Task3 Capture Sink");
        if (SUCCEEDED(status))
            status = graph->ConnectDirect(audio_output.get(), sink->input(), nullptr);
    }
    const CMediaType negotiated = sink ? sink->expected_type() : CMediaType{};
    if (SUCCEEDED(status) &&
        (!ExactConnectionTypes(audio_output.get(), sink->input(), negotiated) ||
         !ExactConnectionTypes(source_output.get(), audio_input.get(), exact_eac3) ||
         !GraphContainsExactly(graph.get(), 3)))
        status = E_UNEXPECTED;

    ComOwner<IMediaControl> control;
    ComOwner<IMediaEvent> events;
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaControl,
                                       reinterpret_cast<void **>(control.put()));
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaEvent,
                                       reinterpret_cast<void **>(events.put()));
    OAFilterState state = State_Stopped;
    if (SUCCEEDED(status) &&
        (control->Run() != S_OK || control->GetState(10000, &state) != S_OK ||
         state != State_Running || WaitForSingleObject(sink->end_of_stream_event(), 30000) !=
                                       WAIT_OBJECT_0))
        status = E_FAIL;

    ULONGLONG classifier = 0;
    ULONGLONG stream = 0;
    if (SUCCEEDED(status) && target_lane &&
        (FAILED(diagnostics->GetOpenJocInputByteCounts(&classifier, &stream)) ||
         (passthrough ? (classifier != 0 || stream != 0)
                      : (classifier == 0 || stream != 0))))
        status = E_UNEXPECTED;
    ComOwner<ILAVOpenJocStatus> admission;
    if (SUCCEEDED(status) && target_lane && !passthrough)
    {
        status = audio_filter->QueryInterface(__uuidof(ILAVOpenJocStatus),
                                              reinterpret_cast<void **>(admission.put()));
        if (SUCCEEDED(status) &&
            admission->GetOpenJocAdmissionState() != LAVOpenJocAdmissionStockEac3)
            status = E_UNEXPECTED;
    }
    HRESULT graph_error = S_OK;
    if (SUCCEEDED(status) &&
        (DrainGraphErrors(events.get(), &graph_error) || !sink->end_of_stream() ||
         sink->end_of_stream_count() != 1 || !sink->end_of_stream_running() ||
         sink->sample_count() == 0 || sink->bytes().empty() ||
         !sink->sample_contracts_valid() || !sink->allocator_contract_valid() ||
         !WriteTask3CaptureEvidence(evidence_path, fixture, *audio_record, *splitter_record,
                                    exact_eac3, *sink, target_lane, policy, passthrough)))
        status = FAILED(graph_error) ? graph_error : E_UNEXPECTED;
    if (target_lane)
    {
        const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
        std::wprintf(L"TASK3_LIVE_COUNTERS phase=post_eos_pre_stop mode=%ls fixture=%ls "
                     L"policy=%hs classifier_input_bytes=%llu stream_input_bytes=%llu\n",
                     passthrough ? L"passthrough" : L"stock", fixture.final_path.c_str(),
                     contract ? contract->property_page_label : "invalid",
                     static_cast<unsigned long long>(classifier),
                     static_cast<unsigned long long>(stream));
    }
    if (control)
    {
        const HRESULT stop_status = control->Stop();
        if (SUCCEEDED(status) && FAILED(stop_status))
            status = stop_status;
    }
    if (SUCCEEDED(status) &&
        (!RuntimeIdentityMatches(records) || !FixtureIdentityMatches(fixture)))
        status = E_UNEXPECTED;
    const std::string audio_sha = DigestHex(audio_record->sha256);
    const std::string splitter_sha = DigestHex(splitter_record->sha256);
    const std::string fixture_sha = DigestHex(fixture.sha256);
    std::wprintf(L"TASK3_MODULE_IDENTITY lane=%ls audio_path=%ls audio_sha256=%hs "
                 L"splitter_path=%ls splitter_sha256=%hs fixture_path=%ls fixture_sha256=%hs\n",
                 target_lane ? L"target" : L"pristine", audio_record->final_path.c_str(),
                 audio_sha.c_str(), splitter_record->final_path.c_str(), splitter_sha.c_str(),
                 fixture.final_path.c_str(), fixture_sha.c_str());
    return status;
}

enum class LifecycleEpochOutcome
{
    Failed,
    Supported,
    Unsupported
};

std::string SerializeLifecycleEventTrace(const std::vector<StreamEventEvidence> &events,
                                         const std::size_t begin)
{
    std::ostringstream trace;
    for (std::size_t index = begin; index < events.size(); ++index)
    {
        if (index != begin)
            trace << ',';
        const auto &event = events[index];
        if (event.kind == StreamEventKind::BeginFlush)
            trace << "BeginFlush";
        else if (event.kind == StreamEventKind::EndFlush)
            trace << "EndFlush";
        else if (event.kind == StreamEventKind::NewSegment)
            trace << "NewSegment(" << event.start << ',' << event.stop << ','
                  << std::setprecision(17) << event.rate << ')';
        else
            trace << "EndOfStream";
    }
    return trace.str();
}

bool ValidateLifecycleEpoch(const std::vector<StreamEventEvidence> &events,
                            const std::size_t begin, const bool require_flush,
                            const REFERENCE_TIME expected_start)
{
    if (begin >= events.size())
        return false;
    std::size_t cursor = begin;
    if (require_flush)
    {
        if (cursor >= events.size() || events[cursor++].kind != StreamEventKind::BeginFlush ||
            cursor >= events.size() || events[cursor++].kind != StreamEventKind::EndFlush)
            return false;
    }
    if (cursor >= events.size() || events[cursor].kind != StreamEventKind::NewSegment ||
        events[cursor].start != expected_start || events[cursor].stop <= events[cursor].start ||
        events[cursor].rate != 1.0)
        return false;
    ++cursor;
    return cursor + 1 == events.size() &&
           events[cursor].kind == StreamEventKind::EndOfStream;
}

HRESULT WaitForLifecycleEpoch(IMediaControl *control, IMediaEvent *events, IMediaSeeking *seeking,
                               StrictCaptureSink *sink, IPin *source_output, IPin *audio_input,
                               IPin *audio_output, const AM_MEDIA_TYPE &source_type,
                               const AM_MEDIA_TYPE &output_type, const bool seek,
                               const REFERENCE_TIME position, const bool require_flush,
                               const bool issue_run, const wchar_t *epoch_label,
                               ILAVOpenJocDiagnostics *diagnostics,
                               ILAVOpenJocStatus *admission,
                               const bool permit_raw_seek_unsupported,
                               LifecycleEpochOutcome *outcome)
{
    if (!control || !events || !seeking || !sink || !source_output || !audio_input ||
        !audio_output || !epoch_label || !*epoch_label || !diagnostics || !admission || !outcome)
        return E_POINTER;
    *outcome = LifecycleEpochOutcome::Failed;
    sink->ResetCompletionForNextSegment();
    const std::size_t event_begin = sink->stream_events().size();
    const std::size_t sample_begin = sink->samples().size();
    const std::size_t byte_begin = sink->bytes().size();
    const std::uint64_t eos_begin = sink->end_of_stream_count();
    ULONGLONG classifier_begin = 0;
    ULONGLONG stream_begin = 0;
    if (diagnostics->GetOpenJocInputByteCounts(&classifier_begin, &stream_begin) != S_OK)
        return E_UNEXPECTED;
    if (seek)
    {
        DWORD capabilities = 0;
        const HRESULT capabilities_status = seeking->GetCapabilities(&capabilities);
        DWORD checked_capabilities =
            AM_SEEKING_CanSeekAbsolute | AM_SEEKING_CanGetDuration;
        const HRESULT check_status = seeking->CheckCapabilities(&checked_capabilities);
        LONGLONG current = position;
        const HRESULT seek_status = seeking->SetPositions(
            &current, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
        std::wprintf(L"TASK3_LIFECYCLE_SEEK label=%ls get_capabilities_hr=0x%08lx "
                     L"capabilities=0x%08lx check_capabilities_hr=0x%08lx checked=0x%08lx "
                     L"set_positions_hr=0x%08lx requested=%lld actual=%lld\n",
                     epoch_label, static_cast<unsigned long>(capabilities_status),
                     static_cast<unsigned long>(capabilities),
                     static_cast<unsigned long>(check_status),
                     static_cast<unsigned long>(checked_capabilities),
                     static_cast<unsigned long>(seek_status), static_cast<long long>(position),
                     static_cast<long long>(current));
        const DWORD required_capabilities =
            AM_SEEKING_CanSeekAbsolute | AM_SEEKING_CanGetDuration;
        if (capabilities_status != S_OK ||
            (capabilities & required_capabilities) != required_capabilities ||
            check_status != S_OK || checked_capabilities != required_capabilities)
        {
            std::wprintf(L"TASK3_LIFECYCLE_FAILURE label=%ls stage=seeking-capabilities\n",
                         epoch_label);
            return E_UNEXPECTED;
        }
        if (FAILED(seek_status) || current != position)
        {
            std::wprintf(L"TASK3_LIFECYCLE_FAILURE label=%ls stage=SetPositions "
                         L"hr=0x%08lx requested=%lld actual=%lld\n",
                         epoch_label, static_cast<unsigned long>(seek_status),
                         static_cast<long long>(position), static_cast<long long>(current));
            return FAILED(seek_status) ? seek_status : E_UNEXPECTED;
        }
    }
    if (issue_run)
    {
        const HRESULT run_status = control->Run();
        if (run_status != S_OK)
            return FAILED(run_status) ? run_status : E_UNEXPECTED;
    }
    OAFilterState state = State_Stopped;
    if (control->GetState(10000, &state) != S_OK || state != State_Running ||
        WaitForSingleObject(sink->end_of_stream_event(), 30000) != WAIT_OBJECT_0)
        return E_FAIL;
    HRESULT graph_error = S_OK;
    if (DrainGraphErrors(events, &graph_error))
        return FAILED(graph_error) ? graph_error : E_FAIL;
    const auto epoch_events = sink->stream_events();
    const auto all_samples = sink->samples();
    const auto all_bytes = sink->bytes();
    ULONGLONG classifier_end = 0;
    ULONGLONG stream_end = 0;
    const HRESULT diagnostics_status =
        diagnostics->GetOpenJocInputByteCounts(&classifier_end, &stream_end);
    const LAVOpenJocAdmissionState admission_state = admission->GetOpenJocAdmissionState();
    const std::string event_trace = SerializeLifecycleEventTrace(epoch_events, event_begin);
    const bool no_sample_delta =
        all_samples.size() == sample_begin && all_bytes.size() == byte_begin;
    if (no_sample_delta && permit_raw_seek_unsupported)
    {
        const bool source_exact = ExactConnectionTypes(source_output, audio_input, source_type);
        const bool output_exact = ExactConnectionTypes(audio_output, sink->input(), output_type);
        const bool events_valid =
            ValidateLifecycleEpoch(epoch_events, event_begin, require_flush, position);
        const bool eos_valid = sink->end_of_stream() && sink->end_of_stream_running() &&
                               sink->end_of_stream_count() == eos_begin + 1;
        const bool reset_state_valid = diagnostics_status == S_OK && classifier_end == 0 &&
                                       stream_end == 0 &&
                                       (admission_state == LAVOpenJocAdmissionStockEac3 ||
                                        admission_state == LAVOpenJocAdmissionStockOpenJocFallback);
        if (!source_exact || !output_exact || !events_valid || !eos_valid || !reset_state_valid ||
            !sink->sample_contracts_valid() || !sink->allocator_contract_valid())
        {
            std::wprintf(L"TASK3_LIFECYCLE_FAILURE label=%ls stage=raw-seek-signature "
                         L"source_exact=%d output_exact=%d events_valid=%d eos_valid=%d "
                         L"reset_state_valid=%d diagnostics_hr=0x%08lx "
                         L"classifier_before=%llu classifier_after=%llu stream_before=%llu "
                         L"stream_after=%llu admission=%u events=%hs\n",
                         epoch_label, source_exact, output_exact, events_valid, eos_valid,
                         reset_state_valid, static_cast<unsigned long>(diagnostics_status),
                         static_cast<unsigned long long>(classifier_begin),
                         static_cast<unsigned long long>(classifier_end),
                         static_cast<unsigned long long>(stream_begin),
                         static_cast<unsigned long long>(stream_end),
                         static_cast<unsigned int>(admission_state), event_trace.c_str());
            return E_UNEXPECTED;
        }
        std::wprintf(L"TASK3_LIFECYCLE_UNSUPPORTED operation=raw-container-absolute-seek "
                     L"label=%ls position=%lld samples=0 bytes=0 classifier_input_bytes=0 "
                     L"stream_input_bytes=0 admission=Stock decoder "
                     L"empty_eos_resolution=Stock decoder no_stock_sample_delivered=1 events=%hs "
                     L"source_type=%hs output_type=%hs\n",
                     epoch_label, static_cast<long long>(position), event_trace.c_str(),
                     SerializeMediaType(source_type).c_str(),
                     SerializeMediaType(output_type).c_str());
        *outcome = LifecycleEpochOutcome::Unsupported;
        return S_OK;
    }
    if (all_samples.size() <= sample_begin || all_bytes.size() <= byte_begin ||
        diagnostics_status != S_OK)
    {
        std::wprintf(L"TASK3_LIFECYCLE_FAILURE label=%ls stage=empty-epoch "
                     L"samples_before=%llu samples_after=%llu bytes_before=%llu bytes_after=%llu "
                     L"diagnostics_hr=0x%08lx classifier_before=%llu classifier_after=%llu "
                     L"stream_before=%llu stream_after=%llu admission=%u events=%hs\n",
                     epoch_label, static_cast<unsigned long long>(sample_begin),
                     static_cast<unsigned long long>(all_samples.size()),
                     static_cast<unsigned long long>(byte_begin),
                     static_cast<unsigned long long>(all_bytes.size()),
                     static_cast<unsigned long>(diagnostics_status),
                     static_cast<unsigned long long>(classifier_begin),
                     static_cast<unsigned long long>(classifier_end),
                     static_cast<unsigned long long>(stream_begin),
                     static_cast<unsigned long long>(stream_end),
                     static_cast<unsigned int>(admission_state), event_trace.c_str());
        return E_UNEXPECTED;
    }
    std::uint64_t epoch_bytes = 0;
    REFERENCE_TIME first_start = all_samples[sample_begin].start;
    REFERENCE_TIME last_stop = first_start;
    for (std::size_t index = sample_begin; index < all_samples.size(); ++index)
    {
        const auto &sample = all_samples[index];
        const REFERENCE_TIME timestamp_gap =
            sample.start >= last_stop ? sample.start - last_stop : last_stop - sample.start;
        if (sample.length <= 0 || sample.start >= sample.stop ||
            (index != sample_begin && timestamp_gap > 1) ||
            epoch_bytes > (std::numeric_limits<std::uint64_t>::max)() -
                              static_cast<std::uint64_t>(sample.length))
            return E_UNEXPECTED;
        epoch_bytes += static_cast<std::uint64_t>(sample.length);
        last_stop = sample.stop;
    }
    const std::uint64_t byte_delta = static_cast<std::uint64_t>(all_bytes.size() - byte_begin);
    const auto absolute_difference = [](const REFERENCE_TIME left,
                                        const REFERENCE_TIME right) {
        return left >= right ? static_cast<std::uint64_t>(left - right)
                             : static_cast<std::uint64_t>(right - left);
    };
    // E-AC-3 uses 1536 samples per 48 kHz access unit: exactly 32 ms.
    // Accept at most one access-unit of alignment around either the declared
    // media-time origin or a segment-relative zero origin.
    constexpr std::uint64_t kTimestampOriginTolerance = 320000;
    const bool coherent_time_base =
        absolute_difference(first_start, 0) <= kTimestampOriginTolerance ||
        absolute_difference(first_start, position) <= kTimestampOriginTolerance;
    const bool source_exact = ExactConnectionTypes(source_output, audio_input, source_type);
    const bool output_exact = ExactConnectionTypes(audio_output, sink->input(), output_type);
    const bool events_valid =
        ValidateLifecycleEpoch(epoch_events, event_begin, require_flush, position);
    const bool eos_valid = sink->end_of_stream() && sink->end_of_stream_running() &&
                           sink->end_of_stream_count() == eos_begin + 1;
    const bool sample_contracts_valid = sink->sample_contracts_valid();
    const bool allocator_contract_valid = sink->allocator_contract_valid();
    if (!source_exact || !output_exact || !events_valid || !eos_valid ||
        epoch_bytes != byte_delta || !coherent_time_base || !sample_contracts_valid ||
        !allocator_contract_valid)
    {
        std::wprintf(L"TASK3_LIFECYCLE_FAILURE label=%ls stage=post-eos source_exact=%d "
                     L"output_exact=%d events_valid=%d eos_valid=%d epoch_bytes=%llu "
                     L"byte_delta=%llu coherent_time_base=%d sample_contracts=%d allocator=%d "
                     L"first_start=%lld position=%lld events=%hs\n",
                     epoch_label, source_exact, output_exact, events_valid, eos_valid,
                     static_cast<unsigned long long>(epoch_bytes),
                     static_cast<unsigned long long>(byte_delta), coherent_time_base,
                     sample_contracts_valid, allocator_contract_valid,
                     static_cast<long long>(first_start), static_cast<long long>(position),
                      event_trace.c_str());
        return E_UNEXPECTED;
    }
    std::wprintf(L"TASK3_LIFECYCLE_EPOCH label=%ls position=%lld samples=%llu bytes=%llu "
                 L"first_start=%lld last_stop=%lld events=%hs source_type=%hs output_type=%hs\n",
                 epoch_label, static_cast<long long>(position),
                 static_cast<unsigned long long>(all_samples.size() - sample_begin),
                 static_cast<unsigned long long>(byte_delta), static_cast<long long>(first_start),
                  static_cast<long long>(last_stop), event_trace.c_str(),
                  SerializeMediaType(source_type).c_str(), SerializeMediaType(output_type).c_str());
    *outcome = LifecycleEpochOutcome::Supported;
    return S_OK;
}

HRESULT RunOneOpenJocLifecycle(const PrivateComModule &audio,
                               const PrivateComModule &splitter,
                               const FixtureIdentity &fixture,
                               const LAVOpenJocOutputPolicy policy,
                               const bool full_sequence)
{
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    if (!contract)
        return E_INVALIDARG;
    const bool raw_container = SameText(fixture.final_path.extension().native(), L".ec3");
    const CMediaType target = BuildStrictTarget(*contract);
    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source_filter;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType exact_eac3;
    HRESULT status = CreateGraphForFixture(
        audio, splitter, fixture.final_path, policy, true, graph.put(), source_filter.put(),
        audio_filter.put(), source_output.put(), audio_input.put(), audio_output.put(),
        &exact_eac3);
    HRESULT sink_status = S_OK;
    auto *sink = new (std::nothrow) StrictCaptureSink(target, false, {}, &sink_status);
    if (SUCCEEDED(status) && (!sink || FAILED(sink_status)))
        status = FAILED(sink_status) ? sink_status : E_OUTOFMEMORY;
    ComOwner<IBaseFilter> sink_owner;
    if (SUCCEEDED(status))
    {
        sink->AddRef();
        sink_owner.attach(static_cast<IBaseFilter *>(sink));
    }
    else if (sink)
    {
        delete sink;
        sink = nullptr;
    }
    if (SUCCEEDED(status))
        status = AttachCaptureSink(graph.get(), audio_output.get(), sink, target);
    if (SUCCEEDED(status) &&
        (!ExactConnectionTypes(source_output.get(), audio_input.get(), exact_eac3) ||
         !ExactConnectionTypes(audio_output.get(), sink->input(), target) ||
         !GraphContainsExactly(graph.get(), 3)))
        status = E_UNEXPECTED;

    ComOwner<IMediaControl> control;
    ComOwner<IMediaEvent> events;
    ComOwner<IMediaSeeking> seeking;
    ComOwner<ILAVOpenJocDiagnostics> diagnostics;
    ComOwner<ILAVOpenJocStatus> admission;
    ComOwner<ILAVAudioStatus> audio_status;
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaControl,
                                       reinterpret_cast<void **>(control.put()));
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaEvent,
                                       reinterpret_cast<void **>(events.put()));
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaSeeking,
                                       reinterpret_cast<void **>(seeking.put()));
    if (SUCCEEDED(status))
        status = audio_filter->QueryInterface(__uuidof(ILAVOpenJocDiagnostics),
                                              reinterpret_cast<void **>(diagnostics.put()));
    if (SUCCEEDED(status))
        status = audio_filter->QueryInterface(__uuidof(ILAVOpenJocStatus),
                                              reinterpret_cast<void **>(admission.put()));
    if (SUCCEEDED(status))
        status = audio_filter->QueryInterface(__uuidof(ILAVAudioStatus),
                                              reinterpret_cast<void **>(audio_status.put()));
    if (SUCCEEDED(status) &&
        (!SameControllingUnknown(audio_filter.get(), diagnostics.get()) ||
         !SameControllingUnknown(audio_filter.get(), admission.get()) ||
         !SameControllingUnknown(audio_filter.get(), audio_status.get())))
        status = E_UNEXPECTED;

    LONGLONG duration = 0;
    LifecycleEpochOutcome epoch_outcome = LifecycleEpochOutcome::Failed;
    std::size_t supported_nonzero_seeks = 0;
    std::size_t unsupported_nonzero_seeks = 0;
    if (SUCCEEDED(status) &&
        (seeking->GetDuration(&duration) != S_OK || duration <= 0 ||
         FAILED(status = WaitForLifecycleEpoch(
                    control.get(), events.get(), seeking.get(), sink, source_output.get(),
                    audio_input.get(), audio_output.get(), exact_eac3, target, false, 0, false,
                    true, full_sequence ? L"initial" : L"rebuild", diagnostics.get(),
                    admission.get(), false, &epoch_outcome))))
        status = FAILED(status) ? status : E_UNEXPECTED;
    if (SUCCEEDED(status) && epoch_outcome != LifecycleEpochOutcome::Supported)
        status = E_UNEXPECTED;

    auto verify_live_joc = [&]() {
        ULONGLONG classifier = 0;
        ULONGLONG stream = 0;
        const char *format = nullptr;
        int channels = 0;
        int sample_rate = 0;
        DWORD mask = 0;
        const HRESULT diagnostics_status =
            diagnostics->GetOpenJocInputByteCounts(&classifier, &stream);
        const LAVOpenJocAdmissionState admission_state =
            admission->GetOpenJocAdmissionState();
        const HRESULT details_status =
            audio_status->GetOutputDetails(&format, &channels, &sample_rate, &mask);
        std::wprintf(L"TASK3_LIVE_JOC_STATUS policy=%hs diagnostics_hr=0x%08lx "
                     L"classifier_input_bytes=%llu stream_input_bytes=%llu admission=%u "
                     L"details_hr=0x%08lx format=%hs channels=%d rate=%d mask=0x%08lx\n",
                     contract->property_page_label,
                     static_cast<unsigned long>(diagnostics_status),
                     static_cast<unsigned long long>(classifier),
                     static_cast<unsigned long long>(stream),
                     static_cast<unsigned int>(admission_state),
                     static_cast<unsigned long>(details_status), format ? format : "<null>",
                     channels, sample_rate, static_cast<unsigned long>(mask));
        return diagnostics_status == S_OK && classifier > 0 && stream > 0 &&
               admission_state == LAVOpenJocAdmissionOpenJoc && details_status == S_OK &&
               format && std::strcmp(format, "32bit Float") == 0 &&
               channels == static_cast<int>(contract->channel_count) && sample_rate == 48000 &&
               mask == contract->windows_channel_mask;
    };
    if (SUCCEEDED(status) && !verify_live_joc())
        status = E_UNEXPECTED;

    if (SUCCEEDED(status) && full_sequence)
    {
        const std::array<REFERENCE_TIME, 3> positions = {
            duration / 4, duration * 3 / 4, duration / 4};
        const std::array<const wchar_t *, 3> labels = {
            L"seek-25", L"seek-75", L"seek-25-backward"};
        for (std::size_t index = 0; index < positions.size(); ++index)
        {
            const REFERENCE_TIME position = positions[index];
            epoch_outcome = LifecycleEpochOutcome::Failed;
            if (FAILED(status = WaitForLifecycleEpoch(
                           control.get(), events.get(), seeking.get(), sink,
                           source_output.get(), audio_input.get(), audio_output.get(), exact_eac3,
                           target, true, position, true, true, labels[index], diagnostics.get(),
                           admission.get(), raw_container, &epoch_outcome)) ||
                (epoch_outcome == LifecycleEpochOutcome::Supported && !verify_live_joc()))
            {
                if (SUCCEEDED(status))
                    status = E_UNEXPECTED;
                break;
            }
            if (epoch_outcome == LifecycleEpochOutcome::Supported)
                ++supported_nonzero_seeks;
            else if (epoch_outcome == LifecycleEpochOutcome::Unsupported)
                ++unsupported_nonzero_seeks;
        }
        const bool seek_matrix_valid = raw_container
                                           ? ((supported_nonzero_seeks == positions.size() &&
                                               unsupported_nonzero_seeks == 0) ||
                                              (supported_nonzero_seeks == 0 &&
                                               unsupported_nonzero_seeks == positions.size()))
                                           : (supported_nonzero_seeks == positions.size() &&
                                              unsupported_nonzero_seeks == 0);
        if (SUCCEEDED(status) && !seek_matrix_valid)
        {
            std::wprintf(L"TASK3_LIFECYCLE_FAILURE stage=mixed-seek-outcomes fixture=%ls "
                         L"supported=%llu unsupported=%llu\n",
                         fixture.final_path.c_str(),
                         static_cast<unsigned long long>(supported_nonzero_seeks),
                         static_cast<unsigned long long>(unsupported_nonzero_seeks));
            status = E_UNEXPECTED;
        }
    }
    if (SUCCEEDED(status) && control->Stop() != S_OK)
        status = E_FAIL;
    if (SUCCEEDED(status) && full_sequence)
    {
        epoch_outcome = LifecycleEpochOutcome::Failed;
        status = WaitForLifecycleEpoch(
            control.get(), events.get(), seeking.get(), sink, source_output.get(),
            audio_input.get(), audio_output.get(), exact_eac3, target, true, 0, false, true,
            L"stop-seek0-run", diagnostics.get(), admission.get(), false, &epoch_outcome);
        if (SUCCEEDED(status) &&
            (epoch_outcome != LifecycleEpochOutcome::Supported || !verify_live_joc()))
            status = E_UNEXPECTED;
        const HRESULT stop_status = control->Stop();
        if (SUCCEEDED(status) && stop_status != S_OK)
            status = E_FAIL;
    }
    if (SUCCEEDED(status) &&
        (!sink->sample_contracts_valid() || !sink->allocator_contract_valid() ||
         sink->sample_count() == 0 || sink->bytes().empty() ||
         !FixtureIdentityMatches(fixture)))
        status = E_UNEXPECTED;
    if (SUCCEEDED(status) && full_sequence)
        std::wprintf(L"TASK3_LIFECYCLE_ROW_EVIDENCE fixture=%ls policy=%hs "
                     L"nonzero_absolute_seek=%ls supported_count=%llu unsupported_count=%llu\n",
                     fixture.final_path.c_str(), contract->property_page_label,
                     unsupported_nonzero_seeks != 0 ? L"UNSUPPORTED_RAW_CONTAINER_OPERATION"
                                                    : L"SUPPORTED",
                     static_cast<unsigned long long>(supported_nonzero_seeks),
                     static_cast<unsigned long long>(unsupported_nonzero_seeks));
    return status;
}

HRESULT DisconnectPinPair(IGraphBuilder *graph, IPin *pin)
{
    if (!graph || !pin)
        return E_POINTER;
    ComOwner<IPin> peer;
    HRESULT status = pin->ConnectedTo(peer.put());
    if (FAILED(status))
        return status;
    status = graph->Disconnect(pin);
    if (SUCCEEDED(status))
        status = graph->Disconnect(peer.get());
    return status;
}

HRESULT RunLiveStatusSourceSwap(const PrivateComModule &audio,
                                const PrivateComModule &splitter,
                                const FixtureIdentity &joc_fixture,
                                const FixtureIdentity &ordinary_fixture,
                                const LAVOpenJocOutputPolicy policy)
{
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    if (!contract)
        return E_INVALIDARG;
    const CMediaType target = BuildStrictTarget(*contract);
    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> joc_source;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> joc_source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType joc_input_type;
    HRESULT status = CreateGraphForFixture(
        audio, splitter, joc_fixture.final_path, policy, true, graph.put(), joc_source.put(),
        audio_filter.put(), joc_source_output.put(), audio_input.put(), audio_output.put(),
        &joc_input_type);
    HRESULT sink_status = S_OK;
    auto *joc_sink = new (std::nothrow) StrictCaptureSink(target, false, {}, &sink_status);
    if (SUCCEEDED(status) && (!joc_sink || FAILED(sink_status)))
        status = FAILED(sink_status) ? sink_status : E_OUTOFMEMORY;
    ComOwner<IBaseFilter> joc_sink_owner;
    if (SUCCEEDED(status))
    {
        joc_sink->AddRef();
        joc_sink_owner.attach(static_cast<IBaseFilter *>(joc_sink));
    }
    else if (joc_sink)
    {
        delete joc_sink;
        joc_sink = nullptr;
    }
    if (SUCCEEDED(status))
        status = AttachCaptureSink(graph.get(), audio_output.get(), joc_sink, target);
    ComOwner<IMediaControl> control;
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaControl,
                                       reinterpret_cast<void **>(control.put()));
    OAFilterState state = State_Stopped;
    if (SUCCEEDED(status) &&
        (control->Run() != S_OK || control->GetState(10000, &state) != S_OK ||
         state != State_Running ||
         WaitForSingleObject(joc_sink->end_of_stream_event(), 30000) != WAIT_OBJECT_0))
        status = E_FAIL;
    if (SUCCEEDED(status) &&
        (!ExactConnectionTypes(joc_source_output.get(), audio_input.get(), joc_input_type) ||
         !ExactConnectionTypes(audio_output.get(), joc_sink->input(), target) ||
         joc_sink->sample_count() == 0 || joc_sink->bytes().empty() ||
         !joc_sink->sample_contracts_valid() || !joc_sink->allocator_contract_valid() ||
         joc_sink->end_of_stream_count() != 1 || !joc_sink->end_of_stream_running()))
        status = E_UNEXPECTED;
    std::unique_ptr<LiveStatusPageBinding> binding;
    if (SUCCEEDED(status))
    {
        binding.reset(new (std::nothrow) LiveStatusPageBinding(audio_filter.get()));
        if (!binding || FAILED(binding->status()) ||
            !SameControllingUnknown(audio_filter.get(), binding->identity()) ||
            !binding->Verify(policy, LAVOpenJocAdmissionOpenJoc,
                             static_cast<WORD>(contract->channel_count), 48000,
                             contract->windows_channel_mask))
            status = E_UNEXPECTED;
    }
    ULONGLONG joc_classifier = 0;
    ULONGLONG joc_stream = 0;
    if (SUCCEEDED(status) &&
        (!binding->ReadCounters(&joc_classifier, &joc_stream) || joc_classifier == 0 ||
         joc_stream == 0))
        status = E_UNEXPECTED;
    if (SUCCEEDED(status) && control->Stop() != S_OK)
        status = E_FAIL;

    if (SUCCEEDED(status))
        status = DisconnectPinPair(graph.get(), joc_source_output.get());
    if (SUCCEEDED(status))
        status = DisconnectPinPair(graph.get(), audio_output.get());
    if (SUCCEEDED(status))
        status = graph->RemoveFilter(joc_source.get());
    if (SUCCEEDED(status))
        status = graph->RemoveFilter(static_cast<IBaseFilter *>(joc_sink));
    joc_source_output.put();
    joc_source.put();
    joc_sink_owner.put();
    joc_sink = nullptr;

    ComOwner<IBaseFilter> stock_source;
    ComOwner<IPin> stock_source_output;
    CMediaType stock_input_type;
    if (SUCCEEDED(status))
        status = splitter.CreateInstance(IID_IBaseFilter,
                                         reinterpret_cast<void **>(stock_source.put()));
    if (SUCCEEDED(status))
        status = graph->AddFilter(stock_source.get(), L"Private ordinary E-AC-3 source");
    ComOwner<ILAVFSettings> splitter_settings;
    if (SUCCEEDED(status))
        status = stock_source->QueryInterface(__uuidof(ILAVFSettings),
                                              reinterpret_cast<void **>(splitter_settings.put()));
    if (SUCCEEDED(status))
        status = splitter_settings->SetRuntimeConfig(TRUE);
    ComOwner<IFileSourceFilter> file_source;
    if (SUCCEEDED(status))
        status = stock_source->QueryInterface(IID_IFileSourceFilter,
                                              reinterpret_cast<void **>(file_source.put()));
    if (SUCCEEDED(status))
        status = file_source->Load(ordinary_fixture.final_path.c_str(), nullptr);
    if (SUCCEEDED(status) && !CurrentFileMatches(file_source.get(), ordinary_fixture.final_path))
        status = E_UNEXPECTED;
    if (SUCCEEDED(status))
        status = FindSingleDolbyCompressedSourcePin(stock_source.get(), stock_source_output.put(),
                                                    &stock_input_type);
    if (SUCCEEDED(status))
        status = graph->ConnectDirect(stock_source_output.get(), audio_input.get(),
                                      &stock_input_type);

    HRESULT stock_sink_status = S_OK;
    auto *stock_sink = new (std::nothrow)
        StrictCaptureSink(CMediaType{}, false, {}, &stock_sink_status, true);
    if (SUCCEEDED(status) && (!stock_sink || FAILED(stock_sink_status)))
        status = FAILED(stock_sink_status) ? stock_sink_status : E_OUTOFMEMORY;
    ComOwner<IBaseFilter> stock_sink_owner;
    if (SUCCEEDED(status))
    {
        stock_sink->AddRef();
        stock_sink_owner.attach(static_cast<IBaseFilter *>(stock_sink));
    }
    else if (stock_sink)
    {
        delete stock_sink;
        stock_sink = nullptr;
    }
    if (SUCCEEDED(status))
        status = graph->AddFilter(static_cast<IBaseFilter *>(stock_sink),
                                  L"Same-instance stock capture sink");
    if (SUCCEEDED(status))
        status = graph->ConnectDirect(audio_output.get(), stock_sink->input(), nullptr);
    const CMediaType stock_output_type = stock_sink ? stock_sink->expected_type() : CMediaType{};
    if (SUCCEEDED(status) &&
        (!ExactConnectionTypes(stock_source_output.get(), audio_input.get(), stock_input_type) ||
         !ExactConnectionTypes(audio_output.get(), stock_sink->input(), stock_output_type) ||
         !GraphContainsExactly(graph.get(), 3) ||
         !SameControllingUnknown(audio_filter.get(), binding->identity())))
        status = E_UNEXPECTED;
    state = State_Stopped;
    if (SUCCEEDED(status) &&
        (control->Run() != S_OK || control->GetState(10000, &state) != S_OK ||
         state != State_Running ||
         WaitForSingleObject(stock_sink->end_of_stream_event(), 30000) != WAIT_OBJECT_0))
        status = E_FAIL;
    if (SUCCEEDED(status))
    {
        const auto *wave = stock_output_type.formattype == FORMAT_WaveFormatEx &&
                                   stock_output_type.pbFormat &&
                                   stock_output_type.cbFormat >= sizeof(WAVEFORMATEX)
                               ? reinterpret_cast<const WAVEFORMATEX *>(stock_output_type.pbFormat)
                               : nullptr;
        const DWORD mask = wave && wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                   stock_output_type.cbFormat >= sizeof(WAVEFORMATEXTENSIBLE)
                               ? reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(
                                     stock_output_type.pbFormat)->dwChannelMask
                               : (wave && wave->nChannels == 2 ? 3u : 0u);
        if (!wave || !binding->Verify(policy, LAVOpenJocAdmissionStockEac3, wave->nChannels,
                                      wave->nSamplesPerSec, mask))
            status = E_UNEXPECTED;
    }
    ULONGLONG stock_classifier = 0;
    ULONGLONG stock_stream = 0;
    std::vector<Digest> channel_digests;
    if (SUCCEEDED(status) &&
        (!binding->ReadCounters(&stock_classifier, &stock_stream) || stock_classifier == 0 ||
         stock_stream != 0 || stock_sink->sample_count() == 0 || stock_sink->bytes().empty() ||
         stock_sink->end_of_stream_count() != 1 || !stock_sink->end_of_stream_running() ||
         !stock_sink->sample_contracts_valid() || !stock_sink->allocator_contract_valid() ||
         !ExactConnectionTypes(stock_source_output.get(), audio_input.get(), stock_input_type) ||
         !ExactConnectionTypes(audio_output.get(), stock_sink->input(), stock_output_type) ||
         !PairwiseDistinctPcmChannelDigests(stock_output_type, stock_sink->bytes(),
                                            &channel_digests)))
        status = E_UNEXPECTED;
    std::wprintf(L"TASK3_LIVE_STATUS phase=joc_to_stock_same_instance policy=%hs "
                 L"joc_classifier=%llu joc_stream=%llu stock_classifier=%llu stock_stream=%llu\n",
                 contract->property_page_label,
                 static_cast<unsigned long long>(joc_classifier),
                 static_cast<unsigned long long>(joc_stream),
                 static_cast<unsigned long long>(stock_classifier),
                 static_cast<unsigned long long>(stock_stream));
    const HRESULT stop_status = control ? control->Stop() : E_POINTER;
    if (SUCCEEDED(status) && stop_status != S_OK)
        status = FAILED(stop_status) ? stop_status : E_FAIL;
    if (SUCCEEDED(status) &&
        (!FixtureIdentityMatches(joc_fixture) || !FixtureIdentityMatches(ordinary_fixture)))
        status = E_UNEXPECTED;
    return status;
}

HRESULT RunRegistryPolicyRecreation(const PrivateComModule &audio,
                                    const PrivateComModule &splitter,
                                    const FixtureIdentity &fixture,
                                    VolatileCurrentUserOverride *registry,
                                    const LAVOpenJocOutputPolicy policy)
{
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    if (!registry || !registry->WritePolicy(policy) || !contract)
        return E_INVALIDARG;
    const CMediaType target = BuildStrictTarget(*contract);
    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source_filter;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType source_type;
    HRESULT status = CreateGraphForFixture(
        audio, splitter, fixture.final_path, policy, false, graph.put(), source_filter.put(),
        audio_filter.put(), source_output.put(), audio_input.put(), audio_output.put(),
        &source_type, false, false);
    ComOwner<ILAVOpenJocSettings> settings;
    LAVOpenJocOutputPolicy loaded = LAVOpenJocOutputPolicy::Stereo;
    if (SUCCEEDED(status))
        status = audio_filter->QueryInterface(__uuidof(ILAVOpenJocSettings),
                                              reinterpret_cast<void **>(settings.put()));
    if (SUCCEEDED(status))
        status = settings->GetOutputPolicy(&loaded);
    if (SUCCEEDED(status) && loaded != policy)
        status = E_UNEXPECTED;
    HRESULT sink_status = S_OK;
    auto *sink = new (std::nothrow) StrictCaptureSink(target, false, {}, &sink_status);
    if (SUCCEEDED(status) && (!sink || FAILED(sink_status)))
        status = FAILED(sink_status) ? sink_status : E_OUTOFMEMORY;
    ComOwner<IBaseFilter> sink_owner;
    if (SUCCEEDED(status))
    {
        sink->AddRef();
        sink_owner.attach(static_cast<IBaseFilter *>(sink));
    }
    else if (sink)
    {
        delete sink;
        sink = nullptr;
    }
    if (SUCCEEDED(status))
        status = AttachCaptureSink(graph.get(), audio_output.get(), sink, target);
    ComOwner<IMediaControl> control;
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaControl,
                                       reinterpret_cast<void **>(control.put()));
    OAFilterState state = State_Stopped;
    if (SUCCEEDED(status) &&
        (control->Run() != S_OK || control->GetState(10000, &state) != S_OK ||
         state != State_Running ||
         WaitForSingleObject(sink->end_of_stream_event(), 30000) != WAIT_OBJECT_0 ||
         !ExactConnectionTypes(audio_output.get(), sink->input(), target) ||
         sink->sample_count() == 0 || !sink->end_of_stream_running()))
        status = E_UNEXPECTED;
    const HRESULT stop_status = control ? control->Stop() : E_POINTER;
    if (SUCCEEDED(status) && stop_status != S_OK)
        status = FAILED(stop_status) ? stop_status : E_FAIL;
    std::wprintf(L"TASK3_REGISTRY_RECREATION policy=%hs type=%hs samples=%llu eos=%llu\n",
                 contract->property_page_label, SerializeMediaType(target).c_str(),
                 static_cast<unsigned long long>(sink ? sink->sample_count() : 0),
                 static_cast<unsigned long long>(sink ? sink->end_of_stream_count() : 0));
    return status;
}

HRESULT RunSameFilterPolicyRenegotiation(const PrivateComModule &audio,
                                         const PrivateComModule &splitter,
                                         const FixtureIdentity &fixture)
{
    constexpr std::array<LAVOpenJocOutputPolicy, LAV_OPENJOC_OUTPUT_CONTRACT_COUNT> policies = {
        LAVOpenJocOutputPolicy::Stereo, LAVOpenJocOutputPolicy::Binaural,
        LAVOpenJocOutputPolicy::Layout51,
        LAVOpenJocOutputPolicy::Layout71, LAVOpenJocOutputPolicy::Layout512,
        LAVOpenJocOutputPolicy::Layout514, LAVOpenJocOutputPolicy::Layout712,
        LAVOpenJocOutputPolicy::Layout714};
    std::vector<CMediaType> transitions;
    for (std::size_t index = 1; index < policies.size(); ++index)
        transitions.push_back(BuildStrictTarget(*FindLAVOpenJocOutputContract(policies[index])));
    const CMediaType initial = BuildStrictTarget(*FindLAVOpenJocOutputContract(policies.front()));
    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source_filter;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType source_type;
    HRESULT status = CreateGraphForFixture(
        audio, splitter, fixture.final_path, policies.front(), true, graph.put(),
        source_filter.put(), audio_filter.put(), source_output.put(), audio_input.put(),
        audio_output.put(), &source_type);
    HRESULT sink_status = S_OK;
    auto *sink = new (std::nothrow)
        StrictCaptureSink(initial, false, transitions, &sink_status, false, true);
    if (SUCCEEDED(status) && (!sink || FAILED(sink_status)))
        status = FAILED(sink_status) ? sink_status : E_OUTOFMEMORY;
    ComOwner<IBaseFilter> sink_owner;
    if (SUCCEEDED(status))
    {
        sink->AddRef();
        sink_owner.attach(static_cast<IBaseFilter *>(sink));
    }
    else if (sink)
    {
        delete sink;
        sink = nullptr;
    }
    if (SUCCEEDED(status))
        status = AttachCaptureSink(graph.get(), audio_output.get(), sink, initial);
    ComOwner<IMediaControl> control;
    ComOwner<IMediaEvent> events;
    ComOwner<IMediaSeeking> seeking;
    ComOwner<ILAVOpenJocSettings> settings;
    ComOwner<ILAVOpenJocStatus> admission;
    ComOwner<ILAVOpenJocDiagnostics> diagnostics;
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaControl,
                                       reinterpret_cast<void **>(control.put()));
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaEvent,
                                       reinterpret_cast<void **>(events.put()));
    if (SUCCEEDED(status))
        status = graph->QueryInterface(IID_IMediaSeeking,
                                       reinterpret_cast<void **>(seeking.put()));
    if (SUCCEEDED(status))
        status = audio_filter->QueryInterface(__uuidof(ILAVOpenJocSettings),
                                              reinterpret_cast<void **>(settings.put()));
    if (SUCCEEDED(status))
        status = audio_filter->QueryInterface(__uuidof(ILAVOpenJocStatus),
                                              reinterpret_cast<void **>(admission.put()));
    if (SUCCEEDED(status))
        status = audio_filter->QueryInterface(__uuidof(ILAVOpenJocDiagnostics),
                                               reinterpret_cast<void **>(diagnostics.put()));
    if (SUCCEEDED(status) &&
        (!SameControllingUnknown(audio_filter.get(), settings.get()) ||
         !SameControllingUnknown(audio_filter.get(), admission.get()) ||
         !SameControllingUnknown(audio_filter.get(), diagnostics.get())))
        status = E_UNEXPECTED;
    LifecycleEpochOutcome epoch_outcome = LifecycleEpochOutcome::Failed;
    if (SUCCEEDED(status))
        status = WaitForLifecycleEpoch(control.get(), events.get(), seeking.get(), sink,
                                       source_output.get(), audio_input.get(), audio_output.get(),
                                       source_type, initial, false, 0, false, true,
                                       L"policy-initial-stereo", diagnostics.get(), admission.get(),
                                       false, &epoch_outcome);
    if (SUCCEEDED(status) && epoch_outcome != LifecycleEpochOutcome::Supported)
        status = E_UNEXPECTED;
    ULONGLONG initial_classifier = 0;
    ULONGLONG initial_stream = 0;
    if (SUCCEEDED(status) &&
        (admission->GetOpenJocAdmissionState() != LAVOpenJocAdmissionOpenJoc ||
         diagnostics->GetOpenJocInputByteCounts(&initial_classifier, &initial_stream) != S_OK ||
         initial_classifier == 0 || initial_stream == 0))
        status = E_UNEXPECTED;
    std::wprintf(L"TASK3_POLICY_RENEGOTIATION policy=Stereo samples=%llu "
                 L"admission=OpenJoc classifier_input_bytes=%llu stream_input_bytes=%llu\n",
                 static_cast<unsigned long long>(sink ? sink->sample_count() : 0),
                 static_cast<unsigned long long>(initial_classifier),
                 static_cast<unsigned long long>(initial_stream));
    if (SUCCEEDED(status) && control->Stop() != S_OK)
        status = E_FAIL;
    for (std::size_t index = 1; SUCCEEDED(status) && index < policies.size(); ++index)
    {
        OAFilterState stopped_state = State_Running;
        if (control->GetState(10000, &stopped_state) != S_OK || stopped_state != State_Stopped)
        {
            status = E_UNEXPECTED;
            break;
        }
        const auto policy = policies[index];
        const CMediaType expected = BuildStrictTarget(*FindLAVOpenJocOutputContract(policy));
        status = settings->SetOutputPolicy(policy);
        LAVOpenJocOutputPolicy actual = policies.front();
        if (SUCCEEDED(status))
            status = settings->GetOutputPolicy(&actual);
        if (SUCCEEDED(status) && actual != policy)
            status = E_UNEXPECTED;
        if (SUCCEEDED(status))
            status = DisconnectPinPair(graph.get(), audio_output.get());
        if (SUCCEEDED(status))
            status = graph->ConnectDirect(audio_output.get(), sink->input(), &expected);
        if (SUCCEEDED(status) &&
            (!ExactConnectionTypes(audio_output.get(), sink->input(), expected) ||
             !ExactConnectionTypes(source_output.get(), audio_input.get(), source_type) ||
             !GraphContainsExactly(graph.get(), 3)))
            status = E_UNEXPECTED;
        if (SUCCEEDED(status))
            std::wprintf(L"TASK3_POLICY_RECONNECT policy=%hs type=%hs graph_filters=3\n",
                         FindLAVOpenJocOutputContract(policy)->property_page_label,
                         SerializeMediaType(expected).c_str());
        const std::wstring epoch_label =
            L"policy-" + Utf8ToWide(FindLAVOpenJocOutputContract(policy)->property_page_label);
        if (SUCCEEDED(status))
        {
            epoch_outcome = LifecycleEpochOutcome::Failed;
            status = WaitForLifecycleEpoch(
                control.get(), events.get(), seeking.get(), sink, source_output.get(),
                audio_input.get(), audio_output.get(), source_type, expected, true, 0, false, true,
                epoch_label.c_str(), diagnostics.get(), admission.get(), false, &epoch_outcome);
        }
        if (SUCCEEDED(status) && epoch_outcome != LifecycleEpochOutcome::Supported)
            status = E_UNEXPECTED;
        ULONGLONG classifier = 0;
        ULONGLONG stream = 0;
        if (SUCCEEDED(status) &&
            (admission->GetOpenJocAdmissionState() != LAVOpenJocAdmissionOpenJoc ||
             diagnostics->GetOpenJocInputByteCounts(&classifier, &stream) != S_OK ||
             classifier == 0 || stream == 0))
            status = E_UNEXPECTED;
        if (SUCCEEDED(status) &&
            (!openjoc_harness_core::ExactMediaTypeEqual(expected, sink->expected_type()) ||
             !ExactConnectionTypes(audio_output.get(), sink->input(), expected)))
            status = E_UNEXPECTED;
        const HRESULT stop_status = control->Stop();
        if (SUCCEEDED(status) && stop_status != S_OK)
            status = FAILED(stop_status) ? stop_status : E_FAIL;
        std::wprintf(L"TASK3_POLICY_RENEGOTIATION policy=%hs samples=%llu type=%hs "
                     L"admission=OpenJoc classifier_input_bytes=%llu stream_input_bytes=%llu\n",
                     FindLAVOpenJocOutputContract(policy)->property_page_label,
                     static_cast<unsigned long long>(sink->sample_count()),
                     SerializeMediaType(expected).c_str(),
                     static_cast<unsigned long long>(classifier),
                     static_cast<unsigned long long>(stream));
    }
    return status;
}

HRESULT ReturnInjectedFailureAfterLiveSettingsRead(const PrivateComModule &audio,
                                                   RegistryRestoreObservation *observation,
                                                   std::wstring *temporary_path)
{
    if (!observation || !temporary_path)
        return E_POINTER;
    VolatileCurrentUserOverride registry(observation);
    *temporary_path = registry.temporary_path();
    if (!registry.ready() || !registry.WritePolicy(LAVOpenJocOutputPolicy::Layout714))
        return E_UNEXPECTED;
    ComOwner<IBaseFilter> filter;
    HRESULT status = audio.CreateInstance(IID_IBaseFilter,
                                          reinterpret_cast<void **>(filter.put()));
    ComOwner<ILAVOpenJocSettings> settings;
    if (SUCCEEDED(status))
        status = filter->QueryInterface(__uuidof(ILAVOpenJocSettings),
                                        reinterpret_cast<void **>(settings.put()));
    LAVOpenJocOutputPolicy actual = LAVOpenJocOutputPolicy::Stereo;
    if (SUCCEEDED(status))
        status = settings->GetOutputPolicy(&actual);
    if (FAILED(status) || actual != LAVOpenJocOutputPolicy::Layout714)
        return FAILED(status) ? status : E_UNEXPECTED;
    return E_FAIL;
}

bool TestInjectedRegistryFailureRestoration(const PrivateComModule &audio)
{
    std::vector<RegistrySnapshotEntry> before;
    if (!SnapshotOpenJocRegistry(&before))
        return false;
    std::wstring temporary_path;
    RegistryRestoreObservation observation;
    const HRESULT injected_failure =
        ReturnInjectedFailureAfterLiveSettingsRead(audio, &observation, &temporary_path);
    std::vector<RegistrySnapshotEntry> after;
    HKEY unexpected = nullptr;
    const LONG absent = RegOpenKeyExW(HKEY_CURRENT_USER, temporary_path.c_str(), 0, KEY_READ,
                                      &unexpected);
    if (unexpected)
        RegCloseKey(unexpected);
    return injected_failure == E_FAIL && observation.attempted && observation.succeeded &&
           SnapshotOpenJocRegistry(&after) && after == before &&
           (absent == ERROR_FILE_NOT_FOUND || absent == ERROR_PATH_NOT_FOUND);
}

HRESULT RunOpenJocLifecycleMatrix(const std::filesystem::path &runtime_dir,
                                  const std::filesystem::path &manifest_path,
                                  const std::filesystem::path &fixture_dir)
{
    std::vector<StagedRecord> records;
    if (!ReadStagedManifest(runtime_dir, manifest_path, &records))
        return E_INVALIDARG;
    const StagedRecord *audio_record = FindRecord(records, StagedKind::Module, L"LAVAudio.ax");
    const StagedRecord *splitter_record = FindRecord(records, StagedKind::Module, L"LAVSplitter.ax");
    FixtureIdentity raw;
    FixtureIdentity mp4;
    FixtureIdentity ordinary;
    if (!audio_record || !splitter_record ||
        !BuildFixtureIdentity(fixture_dir / L"joc.lifecycle.ec3", &raw) ||
        !BuildFixtureIdentity(fixture_dir / L"joc.lifecycle.mp4", &mp4) ||
        !BuildFixtureIdentity(fixture_dir / L"ordinary.fingerprint.eac3", &ordinary))
        return E_INVALIDARG;
    constexpr std::array<LAVOpenJocOutputPolicy, LAV_OPENJOC_OUTPUT_CONTRACT_COUNT> policies = {
        LAVOpenJocOutputPolicy::Stereo, LAVOpenJocOutputPolicy::Binaural,
        LAVOpenJocOutputPolicy::Layout51,
        LAVOpenJocOutputPolicy::Layout71, LAVOpenJocOutputPolicy::Layout512,
        LAVOpenJocOutputPolicy::Layout514, LAVOpenJocOutputPolicy::Layout712,
        LAVOpenJocOutputPolicy::Layout714};
    HRESULT status = S_OK;
    {
        LoadedDependenciesOwner dependencies;
        const std::wstring runtime_final = FinalPathForFile(runtime_dir);
        ScopedActivationContext activation(audio_record->final_path, runtime_final);
        if (!activation.active())
            status = HRESULT_FROM_WIN32(GetLastError());
        PrivateComModule audio(audio_record->final_path, kTargetLavAudio);
        if (SUCCEEDED(status) &&
            (FAILED(audio.status()) || !LoadStagedDependencies(records, dependencies.put())))
            status = E_UNEXPECTED;
        if (SUCCEEDED(status) && !TestInjectedRegistryFailureRestoration(audio))
            status = E_UNEXPECTED;
    }
    if (FAILED(status))
        return status;
    VolatileCurrentUserOverride registry;
    if (!registry.ready() || !registry.WritePolicy(LAVOpenJocOutputPolicy::Stereo))
    {
        const bool restored = registry.Restore();
        return restored ? E_UNEXPECTED : E_FAIL;
    }
    {
        LoadedDependenciesOwner dependencies;
        const std::wstring runtime_final = FinalPathForFile(runtime_dir);
        ScopedActivationContext activation(audio_record->final_path, runtime_final);
        if (!activation.active())
            status = HRESULT_FROM_WIN32(GetLastError());
        PrivateComModule audio(audio_record->final_path, kTargetLavAudio);
        PrivateComModule splitter(splitter_record->final_path, kLavSplitterSource);
        if (SUCCEEDED(status) &&
            (FAILED(audio.status()) || FAILED(splitter.status()) ||
             !LoadStagedDependencies(records, dependencies.put())))
            status = E_UNEXPECTED;
        for (const FixtureIdentity *fixture : {&raw, &mp4})
        {
            for (const auto policy : policies)
            {
                if (SUCCEEDED(status))
                    status = RunOneOpenJocLifecycle(audio, splitter, *fixture, policy, true);
                if (SUCCEEDED(status))
                    status = RunOneOpenJocLifecycle(audio, splitter, *fixture, policy, false);
                if (FAILED(status))
                    break;
                std::wprintf(L"TASK3_LIFECYCLE_COMPLETE fixture=%ls policy=%hs\n",
                             fixture->final_path.c_str(),
                             FindLAVOpenJocOutputContract(policy)->property_page_label);
            }
            if (FAILED(status))
                break;
        }
        if (SUCCEEDED(status))
        {
            for (const auto policy : policies)
            {
                status = RunLiveStatusSourceSwap(audio, splitter, raw, ordinary, policy);
                if (FAILED(status))
                    break;
            }
        }
        if (SUCCEEDED(status))
            status = RunSameFilterPolicyRenegotiation(audio, splitter, raw);
        if (SUCCEEDED(status))
        {
            for (const auto policy : policies)
            {
                status = RunRegistryPolicyRecreation(audio, splitter, raw, &registry, policy);
                if (FAILED(status))
                    break;
            }
        }
        if (SUCCEEDED(status))
        {
            const bool runtime_identity = RuntimeIdentityMatches(records);
            const bool raw_identity = FixtureIdentityMatches(raw);
            const bool mp4_identity = FixtureIdentityMatches(mp4);
            const bool ordinary_identity = FixtureIdentityMatches(ordinary);
            std::wprintf(L"TASK3_FINAL_IDENTITY runtime=%d raw=%d mp4=%d ordinary=%d\n",
                         runtime_identity ? 1 : 0, raw_identity ? 1 : 0,
                         mp4_identity ? 1 : 0, ordinary_identity ? 1 : 0);
            if (!runtime_identity || !raw_identity || !mp4_identity || !ordinary_identity)
                status = E_UNEXPECTED;
        }
    }
    const bool registry_restored = registry.Restore();
    std::wprintf(L"TASK3_FINAL_REGISTRY_RESTORE result=%d prior_status=0x%08lx\n",
                 registry_restored ? 1 : 0, static_cast<unsigned long>(status));
    if (!registry_restored && SUCCEEDED(status))
        status = E_UNEXPECTED;
    return status;
}


bool ExactPinConnectionType(IPin *pin, const AM_MEDIA_TYPE &expected)
{
    if (!pin)
        return false;
    CMediaType actual;
    return pin->ConnectionMediaType(&actual) == S_OK &&
           openjoc_harness_core::ExactMediaTypeEqual(expected, actual);
}

HRESULT RunPositiveControlledCase(const PrivateComModule &audio_module,
                                  const PrivateComModule &splitter_module,
                                  const FixtureIdentity &fixture,
                                  const FixtureIdentity &raw_oracle_fixture,
                                  const LAVOpenJocOutputPolicy policy)
{
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    if (!contract)
        return E_INVALIDARG;
    const CMediaType target = BuildStrictTarget(*contract);
    if (!IsExactLAVOpenJocStrictMediaType(*contract, target) ||
        !FixtureIdentityMatches(fixture) || !FixtureIdentityMatches(raw_oracle_fixture))
        return E_UNEXPECTED;
    const auto *wave = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(target.pbFormat);
    if (!wave || target.cbFormat < sizeof(WAVEFORMATEXTENSIBLE))
        return E_UNEXPECTED;
    std::ostringstream channel_order_builder;
    for (std::uint32_t channel = 0; channel < contract->channel_count; ++channel)
    {
        if (!contract->openjoc_semantic_labels[channel])
            return E_UNEXPECTED;
        if (channel > 0)
            channel_order_builder << ',';
        channel_order_builder << contract->openjoc_semantic_labels[channel];
    }
    const std::string channel_order = channel_order_builder.str();
    std::vector<BYTE> oracle;
    if (!BuildOracleBytes(raw_oracle_fixture.final_path, policy, &oracle))
        return E_UNEXPECTED;

    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source_filter;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType exact_eac3;
    HRESULT status = CreateGraphForFixture(audio_module, splitter_module, fixture.final_path, policy,
                                           true, graph.put(), source_filter.put(), audio_filter.put(),
                                           source_output.put(), audio_input.put(), audio_output.put(),
                                           &exact_eac3);
    if (FAILED(status))
        return status;

    HRESULT sink_status = S_OK;
    auto *sink =
        new (std::nothrow) StrictCaptureSink(target, false, std::vector<CMediaType>{}, &sink_status);
    if (!sink)
        return E_OUTOFMEMORY;
    if (FAILED(sink_status))
    {
        delete sink;
        return sink_status;
    }
    sink->AddRef();
    ComOwner<IBaseFilter> sink_owner;
    sink_owner.attach(static_cast<IBaseFilter *>(sink));
    status = AttachCaptureSink(graph.get(), audio_output.get(), sink, target);
    const bool initial_output_exact = ExactPinConnectionType(audio_output.get(), target);
    const bool initial_input_exact = ExactPinConnectionType(sink->input(), target);
    if (FAILED(status) || !initial_output_exact || !initial_input_exact ||
        !GraphContainsExactly(graph.get(), 3))
        return FAILED(status) ? status : E_UNEXPECTED;

    ComOwner<IMediaControl> control;
    ComOwner<IMediaEvent> events;
    if (FAILED(status = graph->QueryInterface(IID_IMediaControl,
                                              reinterpret_cast<void **>(control.put()))) ||
        FAILED(status = graph->QueryInterface(IID_IMediaEvent,
                                              reinterpret_cast<void **>(events.put()))))
        return status;
    OAFilterState state = State_Stopped;
    const HRESULT pause_status = control->Pause();
    const HRESULT pause_state_status = control->GetState(10000, &state);
    const bool paused = pause_status == S_OK && pause_state_status == S_OK && state == State_Paused;
    state = State_Stopped;
    const HRESULT run_status = control->Run();
    const HRESULT run_state_status = control->GetState(10000, &state);
    const bool running = run_status == S_OK && run_state_status == S_OK && state == State_Running;
    const DWORD eos_wait = WaitForSingleObject(sink->end_of_stream_event(), 30000);
    const HRESULT stop_status = control->Stop();
    HRESULT graph_error_status = S_OK;
    const bool graph_error = DrainGraphErrors(events.get(), &graph_error_status);
    const bool post_stream_exact = ExactConnectionTypes(audio_output.get(), sink->input(), target) &&
                                   ExactConnectionTypes(source_output.get(), audio_input.get(),
                                                        exact_eac3) &&
                                   GraphContainsExactly(graph.get(), 3);

    const std::vector<CMediaType> connections = sink->receive_connections();
    const std::vector<CMediaType> queries = sink->query_accepts();
    const std::vector<CMediaType> attached = sink->sample_attached_types();
    const std::vector<CMediaType> set_types = sink->set_media_types();
    const std::vector<BYTE> captured = sink->bytes();
    const bool receive_exact = connections.size() == 1 &&
                               openjoc_harness_core::ExactMediaTypeEqual(target,
                                                                        connections.front());
    bool query_types_exact = true;
    for (const auto &query : queries)
        query_types_exact = query_types_exact &&
                            openjoc_harness_core::ExactMediaTypeEqual(target, query);
    bool sample_types_exact = true;
    for (const auto &sample_type : attached)
        sample_types_exact = sample_types_exact &&
                             openjoc_harness_core::ExactMediaTypeEqual(target, sample_type);
    const bool set_type_exact = set_types.size() == 1 &&
                                openjoc_harness_core::ExactMediaTypeEqual(target,
                                                                         set_types.front());
    const bool distinct = openjoc_harness_core::FingerprintsArePairwiseDistinct(
        openjoc_harness_core::InterleavedFingerprints(captured, contract->channel_count));
    const bool per_channel_oracle_equal =
        openjoc_harness_core::InterleavedChannelBytes(captured, contract->channel_count) ==
        openjoc_harness_core::InterleavedChannelBytes(oracle, contract->channel_count);
    const bool per_channel_digests_equal =
        ChannelDigestsEqualAndDistinct(captured, oracle, contract->channel_count);

    openjoc_harness_core::EvidenceInputs evidence;
    evidence.requested_type_exact = IsExactLAVOpenJocStrictMediaType(*contract, target);
    evidence.receive_type_exact = receive_exact;
    evidence.output_type_exact = initial_output_exact;
    evidence.input_type_exact = initial_input_exact;
    evidence.post_stream_type_exact = post_stream_exact;
    evidence.sample_types_exact = sample_types_exact && set_type_exact && query_types_exact;
    evidence.exact_connection = receive_exact && post_stream_exact;
    evidence.paused = paused;
    evidence.running = running;
    evidence.running_sample = sink->running_sample_count() > 0;
    evidence.allocator_valid = sink->allocator_contract_valid();
    evidence.timestamps_complete = sink->sample_contracts_valid();
    evidence.samples = sink->sample_count();
    evidence.bytes = captured.size();
    evidence.end_of_stream_count = eos_wait == WAIT_OBJECT_0 ? sink->end_of_stream_count() : 0;
    evidence.end_of_stream_running = sink->end_of_stream_running();
    evidence.graph_error = graph_error;
    const bool complete =
        openjoc_harness_core::ClassifyControlledEvidence(evidence) ==
            openjoc_harness_core::ControlledEvidenceState::ControlledSinkComplete &&
        stop_status == S_OK && captured == oracle && per_channel_oracle_equal &&
        per_channel_digests_equal && distinct && FixtureIdentityMatches(fixture) &&
        FixtureIdentityMatches(raw_oracle_fixture);
    if (!complete)
    {
        std::fwprintf(stderr,
                      L"controlled case failed: fixture=%ls policy=%hs status=0x%08lx "
                      L"paused=%d running=%d samples=%llu bytes=%llu eos=%d graph_error=0x%08lx\n",
                      fixture.final_path.c_str(), contract->property_page_label,
                      static_cast<unsigned long>(status), paused, running,
                      static_cast<unsigned long long>(sink->sample_count()),
                      static_cast<unsigned long long>(captured.size()),
                      evidence.end_of_stream_count == 1,
                      static_cast<unsigned long>(graph_error_status));
        return E_FAIL;
    }
    const std::string fixture_sha = DigestHex(fixture.sha256);
    const std::string oracle_sha = DigestHex(raw_oracle_fixture.sha256);
    std::wprintf(L"CONTROLLED_SINK_COMPLETE fixture_path=%ls fixture_sha256=%hs "
                 L"oracle_sha256=%hs policy=%hs channels=%u mask=0x%08x channel_order=%hs "
                 L"format_tag=0x%04x subtype=IEEE_FLOAT sample_rate=%lu bits=%u valid_bits=%u "
                 L"block_align=%u avg_bytes_per_sec=%lu actual_frame_size=%u "
                 L"checked_buffer_sizing=1 allocator_contract_valid=1 frame_aligned=1 "
                 L"full_interleaved_oracle_equal=1 per_channel_oracle_equal=1 "
                 L"per_channel_digests_pairwise_distinct=1 proposals=1 fallback_proposals=0 "
                 L"type_mutations=0 eos=1 samples=%llu bytes=%llu\n",
                 fixture.final_path.c_str(), fixture_sha.c_str(), oracle_sha.c_str(),
                 contract->property_page_label,
                 contract->channel_count, contract->windows_channel_mask, channel_order.c_str(),
                 wave->Format.wFormatTag,
                 static_cast<unsigned long>(wave->Format.nSamplesPerSec),
                 wave->Format.wBitsPerSample, wave->Samples.wValidBitsPerSample,
                 wave->Format.nBlockAlign,
                 static_cast<unsigned long>(wave->Format.nAvgBytesPerSec),
                 wave->Format.nBlockAlign,
                 static_cast<unsigned long long>(sink->sample_count()),
                 static_cast<unsigned long long>(captured.size()));
    return S_OK;
}

HRESULT RunDynamicRejectionTrap(const PrivateComModule &audio_module,
                                const PrivateComModule &splitter_module,
                                const FixtureIdentity &fixture)
{
    constexpr LAVOpenJocOutputPolicy kPolicy = LAVOpenJocOutputPolicy::Layout714;
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(kPolicy);
    const CMediaType target = BuildStrictTarget(*contract);
    const CMediaType bootstrap = BuildPcmType(2, 0x00000003u, false);
    if (!IsExactLAVOpenJocStrictMediaType(*contract, target) || !IsPcmType(bootstrap) ||
        !FixtureIdentityMatches(fixture))
        return E_UNEXPECTED;

    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source_filter;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType exact_eac3;
    HRESULT status = CreateGraphForFixture(audio_module, splitter_module, fixture.final_path, kPolicy,
                                           false, graph.put(), source_filter.put(), audio_filter.put(),
                                           source_output.put(), audio_input.put(), audio_output.put(),
                                           &exact_eac3);
    if (FAILED(status))
        return status;
    const std::vector<CMediaType> fallbacks = {
        BuildPcmType(contract->channel_count, contract->windows_channel_mask, false),
        BuildPcmType(6, 0x0000003fu, true), BuildPcmType(8, 0x0000063fu, true),
        BuildPcmType(2, 0x00000003u, true), bootstrap};
    HRESULT sink_status = S_OK;
    auto *sink = new (std::nothrow)
        StrictCaptureSink(target, true, fallbacks, &sink_status);
    if (!sink)
        return E_OUTOFMEMORY;
    if (FAILED(sink_status))
    {
        delete sink;
        return sink_status;
    }
    sink->AddRef();
    ComOwner<IBaseFilter> sink_owner;
    sink_owner.attach(static_cast<IBaseFilter *>(sink));
    status = AttachCaptureSink(graph.get(), audio_output.get(), sink, bootstrap);
    if (FAILED(status) || !ExactConnectionTypes(audio_output.get(), sink->input(), bootstrap) ||
        !GraphContainsExactly(graph.get(), 3))
        return FAILED(status) ? status : E_UNEXPECTED;

    // The bootstrap graph is fully connected before the strict target is
    // selected. Runtime config was already enabled, so this cannot read or
    // write the user's persisted policy.
    if (FAILED(status = SetOpenJocPolicy(audio_filter.get(), kPolicy)))
        return status;

    for (const auto &fallback : fallbacks)
    {
        if (sink->input()->QueryAccept(&fallback) != S_OK)
            return E_UNEXPECTED;
    }
    sink->ResetQueries();

    ComOwner<IMediaControl> control;
    ComOwner<IMediaEvent> events;
    if (FAILED(status = graph->QueryInterface(IID_IMediaControl,
                                              reinterpret_cast<void **>(control.put()))) ||
        FAILED(status = graph->QueryInterface(IID_IMediaEvent,
                                              reinterpret_cast<void **>(events.put()))))
        return status;
    OAFilterState state = State_Stopped;
    const HRESULT pause_status = control->Pause();
    const HRESULT pause_state_status = control->GetState(10000, &state);
    const bool paused = pause_status == S_OK && pause_state_status == S_OK && state == State_Paused;
    state = State_Stopped;
    const HRESULT run_status = control->Run();
    const HRESULT run_state_status = control->GetState(10000, &state);
    const bool running = run_status == S_OK && run_state_status == S_OK && state == State_Running;
    const DWORD rejected = WaitForSingleObject(sink->rejection_event(), 30000);
    std::uint64_t stable_serial = 0;
    const bool quiescent = rejected == WAIT_OBJECT_0 &&
                           WaitForSinkQuiescence(sink, &stable_serial);
    state = State_Stopped;
    const HRESULT quiescent_state_status = control->GetState(1000, &state);
    const bool running_after_quiescence =
        quiescent_state_status == S_OK && state == State_Running;
    const std::vector<CMediaType> queries_before_stop = sink->query_accepts();
    const std::vector<CMediaType> connections_before_stop = sink->receive_connections();
    const std::vector<CMediaType> set_types_before_stop = sink->set_media_types();
    const std::vector<CMediaType> attached_before_stop = sink->sample_attached_types();
    const std::uint64_t samples_before_stop = sink->sample_observation_count();
    const bool exact_before_stop =
        queries_before_stop.size() == 1 &&
        openjoc_harness_core::ExactMediaTypeEqual(target, queries_before_stop.front()) &&
        connections_before_stop.size() == 1 &&
        openjoc_harness_core::ExactMediaTypeEqual(bootstrap,
                                                  connections_before_stop.front()) &&
        set_types_before_stop.size() == 1 &&
        openjoc_harness_core::ExactMediaTypeEqual(bootstrap, set_types_before_stop.front()) &&
        attached_before_stop.empty() && samples_before_stop == 0 && !sink->end_of_stream();
    HRESULT graph_error_before_stop = S_OK;
    const bool pre_stop_graph_error = DrainGraphErrors(events.get(), &graph_error_before_stop);
    const bool unchanged_before_stop = quiescent && running_after_quiescence &&
        sink->mutation_serial() == stable_serial && exact_before_stop && !pre_stop_graph_error &&
        ExactConnectionTypes(audio_output.get(), sink->input(), bootstrap) &&
        ExactConnectionTypes(source_output.get(), audio_input.get(), exact_eac3) &&
        GraphContainsExactly(graph.get(), 3);
    const HRESULT stop_status = control->Stop();
    HRESULT graph_error_status = S_OK;
    const bool later_graph_error = DrainGraphErrors(events.get(), &graph_error_status);
    const std::vector<CMediaType> queries = sink->query_accepts();
    const std::vector<CMediaType> connections = sink->receive_connections();
    const std::vector<CMediaType> set_types = sink->set_media_types();
    const std::vector<CMediaType> attached = sink->sample_attached_types();
    const bool no_later_sink_mutation = sink->mutation_serial() == stable_serial &&
                                        queries.size() == queries_before_stop.size() &&
                                        connections.size() == connections_before_stop.size() &&
                                        set_types.size() == set_types_before_stop.size() &&
                                        attached.size() == attached_before_stop.size() &&
                                        sink->sample_observation_count() == samples_before_stop;
    const bool unchanged_after_stop =
        ExactConnectionTypes(audio_output.get(), sink->input(), bootstrap) &&
        ExactConnectionTypes(source_output.get(), audio_input.get(), exact_eac3) &&
        GraphContainsExactly(graph.get(), 3);
    const bool exact_single_dynamic =
        queries.size() == 1 && openjoc_harness_core::ExactMediaTypeEqual(target, queries.front());
    const bool exact_single_bootstrap =
        connections.size() == 1 &&
        openjoc_harness_core::ExactMediaTypeEqual(bootstrap, connections.front());
    const bool exact_single_bootstrap_set =
        set_types.size() == 1 &&
        openjoc_harness_core::ExactMediaTypeEqual(bootstrap, set_types.front());
    const bool exact_failure = sink->rejected_stage() == L"QueryAccept" &&
                               sink->rejected_raw_result() == S_FALSE &&
                               sink->rejected_normalized_result() == VFW_E_TYPE_NOT_ACCEPTED &&
                               !later_graph_error;
    if (rejected != WAIT_OBJECT_0 || !paused || !running || !unchanged_before_stop ||
        stop_status != S_OK || !exact_single_dynamic || !exact_single_bootstrap ||
        !exact_single_bootstrap_set || !exact_failure || !no_later_sink_mutation ||
        !unchanged_after_stop ||
        sink->sample_observation_count() != 0 || !sink->bytes().empty() || !attached.empty() ||
        sink->end_of_stream() || !FixtureIdentityMatches(fixture))
    {
        std::fwprintf(stderr,
                      L"dynamic rejection trap failed: wait=%lu queries=%llu samples=%llu "
                      L"stage=%ls raw=0x%08lx normalized=0x%08lx pre_graph=0x%08lx "
                      L"later_graph=0x%08lx quiescent=%d running_after=%d\n",
                      static_cast<unsigned long>(rejected),
                      static_cast<unsigned long long>(queries.size()),
                      static_cast<unsigned long long>(sink->sample_observation_count()),
                      sink->rejected_stage().c_str(),
                      static_cast<unsigned long>(sink->rejected_raw_result()),
                      static_cast<unsigned long>(sink->rejected_normalized_result()),
                      static_cast<unsigned long>(graph_error_before_stop),
                      static_cast<unsigned long>(graph_error_status), quiescent,
                      running_after_quiescence);
        return E_FAIL;
    }
    const std::string fixture_sha = DigestHex(fixture.sha256);
    std::wprintf(L"CONTROLLED_SINK_COMPLETE dynamic_rejection fixture_path=%ls "
                 L"fixture_sha256=%hs policy=7.1.4 "
                 L"stage=QueryAccept raw=0x%08lx hr=0x%08lx quiescence_ms=3000 "
                 L"proposals=1 samples=0\n",
                 fixture.final_path.c_str(), fixture_sha.c_str(),
                 static_cast<unsigned long>(sink->rejected_raw_result()),
                 static_cast<unsigned long>(sink->rejected_normalized_result()));
    return S_OK;
}

HRESULT RunControlledSinkMatrix(const std::filesystem::path &runtime_dir,
                                const std::filesystem::path &manifest_path,
                                const std::filesystem::path &fixture_dir)
{
    std::vector<StagedRecord> records;
    if (!ReadStagedManifest(runtime_dir, manifest_path, &records))
        return E_INVALIDARG;
    const StagedRecord *audio_record = FindRecord(records, StagedKind::Module, L"LAVAudio.ax");
    const StagedRecord *splitter_record = FindRecord(records, StagedKind::Module, L"LAVSplitter.ax");
    if (!audio_record || !splitter_record)
        return E_UNEXPECTED;
    const std::wstring runtime_final = FinalPathForFile(runtime_dir);
    ScopedActivationContext activation(audio_record->final_path, runtime_final);
    if (!activation.active())
        return HRESULT_FROM_WIN32(GetLastError());
    PrivateComModule audio(audio_record->final_path, kTargetLavAudio);
    PrivateComModule splitter(splitter_record->final_path, kLavSplitterSource);
    if (FAILED(audio.status()) || FAILED(splitter.status()) ||
        !SamePath(audio.path(), audio_record->final_path) || audio.sha256() != audio_record->sha256 ||
        !SamePath(splitter.path(), splitter_record->final_path) ||
        splitter.sha256() != splitter_record->sha256)
        return E_UNEXPECTED;
    std::vector<HMODULE> dependencies;
    if (!LoadStagedDependencies(records, &dependencies))
        return E_UNEXPECTED;

    constexpr std::array<LAVOpenJocOutputPolicy, LAV_OPENJOC_OUTPUT_CONTRACT_COUNT> policies = {
        LAVOpenJocOutputPolicy::Stereo,   LAVOpenJocOutputPolicy::Binaural,
        LAVOpenJocOutputPolicy::Layout51,
        LAVOpenJocOutputPolicy::Layout71, LAVOpenJocOutputPolicy::Layout512,
        LAVOpenJocOutputPolicy::Layout514, LAVOpenJocOutputPolicy::Layout712,
        LAVOpenJocOutputPolicy::Layout714};
    const std::filesystem::path raw_path = fixture_dir / L"joc.fingerprint.ec3";
    const std::filesystem::path mp4_path = fixture_dir / L"joc.fingerprint.mp4";
    FixtureIdentity raw;
    FixtureIdentity mp4;
    HRESULT status = S_OK;
    if (!BuildFixtureIdentity(raw_path, &raw) || !BuildFixtureIdentity(mp4_path, &mp4))
        status = E_INVALIDARG;
    if (SUCCEEDED(status))
    {
        const std::string raw_sha = DigestHex(raw.sha256);
        const std::string mp4_sha = DigestHex(mp4.sha256);
        std::wprintf(L"FIXTURE_IDENTITY kind=raw path=%ls sha256=%hs\n", raw.final_path.c_str(),
                     raw_sha.c_str());
        std::wprintf(L"FIXTURE_IDENTITY kind=mp4 path=%ls sha256=%hs\n", mp4.final_path.c_str(),
                     mp4_sha.c_str());
    }
    for (const auto *fixture : {&raw, &mp4})
    {
        for (const auto policy : policies)
        {
            if (SUCCEEDED(status))
                status = RunPositiveControlledCase(audio, splitter, *fixture, raw, policy);
        }
    }
    if (SUCCEEDED(status))
        status = RunDynamicRejectionTrap(audio, splitter, raw);
    if (SUCCEEDED(status) && !RuntimeIdentityMatches(records))
        status = E_UNEXPECTED;
    FreeModules(&dependencies);
    return status;
}

HRESULT RunLegacyCoreGraphCase(const std::filesystem::path &runtime_dir,
                               const std::filesystem::path &manifest_path,
                               const std::filesystem::path &fixture_path,
                               const LAVOpenJocOutputPolicy policy)
{
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    if (!contract || !fixture_path.is_absolute())
        return E_INVALIDARG;

    std::vector<unsigned char> fixture_bytes;
    FixtureIdentity fixture;
    if (!ReadFixtureBytes(fixture_path, &fixture_bytes) ||
        !BuildFixtureIdentity(fixture_path, &fixture) ||
        !FixtureIdentityMatches(fixture))
    {
        std::fwprintf(stderr, L"LEGACY_CORE_GRAPH_FAILURE stage=fixture-read fixture=%ls\n",
                      fixture_path.c_str());
        return E_INVALIDARG;
    }

    std::vector<BYTE> oracle;
    if (!BuildLegacyCoreOracleBytes(fixture_path, policy, &oracle))
    {
        std::fwprintf(stderr, L"LEGACY_CORE_GRAPH_FAILURE stage=build-oracle fixture=%ls policy=%u\n",
                      fixture_path.c_str(), static_cast<unsigned int>(policy));
        return E_UNEXPECTED;
    }

    std::vector<StagedRecord> records;
    if (!ReadStagedManifest(runtime_dir, manifest_path, &records))
    {
        std::fwprintf(stderr, L"LEGACY_CORE_GRAPH_FAILURE stage=read-manifest runtime=%ls manifest=%ls\n",
                      runtime_dir.c_str(), manifest_path.c_str());
        return E_INVALIDARG;
    }
    const StagedRecord *audio_record = FindRecord(records, StagedKind::Module, L"LAVAudio.ax");
    const StagedRecord *splitter_record = FindRecord(records, StagedKind::Module, L"LAVSplitter.ax");
    if (!audio_record || !splitter_record)
        return E_UNEXPECTED;

    const std::wstring runtime_final = FinalPathForFile(runtime_dir);
    ScopedActivationContext activation(audio_record->final_path, runtime_final);
    if (!activation.active())
        return HRESULT_FROM_WIN32(GetLastError());
    PrivateComModule audio(audio_record->final_path, kTargetLavAudio);
    PrivateComModule splitter(splitter_record->final_path, kLavSplitterSource);
    LoadedDependenciesOwner dependencies;
    if (FAILED(audio.status()) || FAILED(splitter.status()) ||
        !LoadStagedDependencies(records, dependencies.put()))
        return E_UNEXPECTED;

    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source_filter;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType source_type;
    HRESULT status = CreateGraphForFixture(
        audio, splitter, fixture_path, policy, true, graph.put(), source_filter.put(),
        audio_filter.put(), source_output.put(), audio_input.put(), audio_output.put(),
        &source_type, false);
    if (FAILED(status))
    {
        std::fwprintf(stderr, L"LEGACY_CORE_GRAPH_FAILURE stage=create-graph hr=0x%08lx fixture=%ls\n",
                      static_cast<unsigned long>(status), fixture_path.c_str());
        return status;
    }

    const bool source_is_ac3 = source_type.subtype == MEDIASUBTYPE_DOLBY_AC3;
    const bool source_is_eac3 = source_type.subtype == kDolbyDdPlus;
    if (!source_is_ac3 && !source_is_eac3)
        return E_UNEXPECTED;

    const CMediaType target = BuildStrictTarget(*contract);
    if (!IsExactLAVOpenJocStrictMediaType(*contract, target) ||
        !ExactConnectionTypes(source_output.get(), audio_input.get(), source_type))
        return E_UNEXPECTED;

    ComOwner<ILAVOpenJocDiagnostics> diagnostics;
    ComOwner<ILAVOpenJocStatus> admission;
    if (FAILED(status = audio_filter->QueryInterface(
                  __uuidof(ILAVOpenJocDiagnostics), reinterpret_cast<void **>(diagnostics.put()))) ||
        FAILED(status = audio_filter->QueryInterface(
                  __uuidof(ILAVOpenJocStatus), reinterpret_cast<void **>(admission.put()))) ||
        !SameControllingUnknown(audio_filter.get(), diagnostics.get()) ||
        admission->GetOpenJocAdmissionState() != LAVOpenJocAdmissionUndecided)
        return FAILED(status) ? status : E_UNEXPECTED;

    HRESULT sink_status = S_OK;
    auto *sink = new (std::nothrow) StrictCaptureSink(target, false, {}, &sink_status, true);
    if (!sink)
        return E_OUTOFMEMORY;
    if (FAILED(sink_status))
    {
        delete sink;
        return sink_status;
    }
    sink->AddRef();
    ComOwner<IBaseFilter> sink_owner;
    sink_owner.attach(static_cast<IBaseFilter *>(sink));
    if (FAILED(status = AttachCaptureSink(graph.get(), audio_output.get(), sink, target)) ||
        !ExactConnectionTypes(audio_output.get(), sink->input(), target) ||
        !GraphContainsExactly(graph.get(), 3))
        return FAILED(status) ? status : E_UNEXPECTED;

    ComOwner<IMediaControl> control;
    ComOwner<IMediaEvent> events;
    if (FAILED(status = graph->QueryInterface(IID_IMediaControl,
                                              reinterpret_cast<void **>(control.put()))) ||
        FAILED(status = graph->QueryInterface(IID_IMediaEvent,
                                              reinterpret_cast<void **>(events.put()))))
        return status;

    OAFilterState state = State_Stopped;
    if (control->Run() != S_OK || control->GetState(10000, &state) != S_OK ||
        state != State_Running ||
        WaitForSingleObject(sink->end_of_stream_event(), 30000) != WAIT_OBJECT_0)
        return E_FAIL;

    ULONGLONG classifier_bytes = 0;
    ULONGLONG stream_bytes = 0;
    HRESULT graph_error = S_OK;
    const bool graph_failed = DrainGraphErrors(events.get(), &graph_error);
    const bool counters_valid = diagnostics->GetOpenJocInputByteCounts(
                                    &classifier_bytes, &stream_bytes) == S_OK &&
                                classifier_bytes > 0 &&
                                stream_bytes == static_cast<ULONGLONG>(fixture_bytes.size());
    const bool admission_valid = admission->GetOpenJocAdmissionState() == LAVOpenJocAdmissionOpenJoc;
    const bool output_valid = ExactConnectionTypes(audio_output.get(), sink->input(), target) &&
                              sink->sample_count() > 0 && !sink->bytes().empty() &&
                              sink->bytes() == oracle && sink->sample_contracts_valid() &&
                              sink->allocator_contract_valid() && sink->end_of_stream() &&
                              sink->end_of_stream_count() == 1 && sink->end_of_stream_running();
    const HRESULT stop_status = control->Stop();
    const bool complete = !graph_failed && SUCCEEDED(graph_error) && SUCCEEDED(stop_status) &&
                          source_is_ac3 && counters_valid && admission_valid && output_valid;
    std::wprintf(L"LEGACY_CORE_GRAPH_%ls fixture=%ls subtype=%ls codec_id=%ls policy=%hs "
                 L"classifier_input_bytes=%llu stream_input_bytes=%llu samples=%llu bytes=%llu "
                 L"eos=%llu graph_error=0x%08lx\n",
                 complete ? L"COMPLETE" : L"FAILURE", fixture.final_path.c_str(),
                 source_is_ac3 ? L"AC3" : L"EAC3", source_is_ac3 ? L"AV_CODEC_ID_AC3" : L"AV_CODEC_ID_EAC3",
                 contract->property_page_label, static_cast<unsigned long long>(classifier_bytes),
                 static_cast<unsigned long long>(stream_bytes),
                 static_cast<unsigned long long>(sink->sample_count()),
                 static_cast<unsigned long long>(sink->bytes().size()),
                 static_cast<unsigned long long>(sink->end_of_stream_count()),
                 static_cast<unsigned long>(graph_error));
    return complete ? S_OK : E_FAIL;
}

HRESULT RunAc3BitstreamGraphCase(const std::filesystem::path &runtime_dir,
                                 const std::filesystem::path &manifest_path,
                                 const std::filesystem::path &fixture_path)
{
    std::vector<unsigned char> fixture_bytes;
    if (!ReadFixtureBytes(fixture_path, &fixture_bytes))
        return E_INVALIDARG;
    std::vector<StagedRecord> records;
    if (!ReadStagedManifest(runtime_dir, manifest_path, &records))
        return E_INVALIDARG;
    const StagedRecord *audio_record = FindRecord(records, StagedKind::Module, L"LAVAudio.ax");
    const StagedRecord *splitter_record = FindRecord(records, StagedKind::Module, L"LAVSplitter.ax");
    if (!audio_record || !splitter_record)
        return E_UNEXPECTED;

    const std::wstring runtime_final = FinalPathForFile(runtime_dir);
    ScopedActivationContext activation(audio_record->final_path, runtime_final);
    if (!activation.active())
        return HRESULT_FROM_WIN32(GetLastError());
    PrivateComModule audio(audio_record->final_path, kTargetLavAudio);
    PrivateComModule splitter(splitter_record->final_path, kLavSplitterSource);
    LoadedDependenciesOwner dependencies;
    if (FAILED(audio.status()) || FAILED(splitter.status()) ||
        !LoadStagedDependencies(records, dependencies.put()))
        return E_UNEXPECTED;

    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source_filter;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType source_type;
    HRESULT status = CreateGraphForFixture(
        audio, splitter, fixture_path, LAVOpenJocOutputPolicy::Stereo, false,
        graph.put(), source_filter.put(), audio_filter.put(), source_output.put(),
        audio_input.put(), audio_output.put(), &source_type, false, true, true);
    if (FAILED(status))
        return status;
    if (source_type.subtype != MEDIASUBTYPE_DOLBY_AC3)
        return E_UNEXPECTED;

    ComOwner<ILAVOpenJocDiagnostics> diagnostics;
    ComOwner<ILAVOpenJocStatus> admission;
    if (FAILED(status = audio_filter->QueryInterface(
                  __uuidof(ILAVOpenJocDiagnostics), reinterpret_cast<void **>(diagnostics.put()))) ||
        FAILED(status = audio_filter->QueryInterface(
                  __uuidof(ILAVOpenJocStatus), reinterpret_cast<void **>(admission.put()))) ||
        admission->GetOpenJocAdmissionState() != LAVOpenJocAdmissionUndecided)
        return FAILED(status) ? status : E_UNEXPECTED;

    HRESULT sink_status = S_OK;
    auto *sink = new (std::nothrow) StrictCaptureSink(CMediaType{}, false, {}, &sink_status, true);
    if (!sink)
        return E_OUTOFMEMORY;
    if (FAILED(sink_status))
    {
        delete sink;
        return sink_status;
    }
    sink->AddRef();
    ComOwner<IBaseFilter> sink_owner;
    sink_owner.attach(static_cast<IBaseFilter *>(sink));
    if (FAILED(status = graph->AddFilter(static_cast<IBaseFilter *>(sink), L"AC-3 bitstream sink")) ||
        FAILED(status = graph->ConnectDirect(audio_output.get(), sink->input(), nullptr)))
        return status;

    ComOwner<IMediaControl> control;
    ComOwner<IMediaEvent> events;
    if (FAILED(status = graph->QueryInterface(IID_IMediaControl,
                                              reinterpret_cast<void **>(control.put()))) ||
        FAILED(status = graph->QueryInterface(IID_IMediaEvent,
                                              reinterpret_cast<void **>(events.put()))))
        return status;
    OAFilterState state = State_Stopped;
    if (control->Run() != S_OK || control->GetState(10000, &state) != S_OK ||
        state != State_Running ||
        WaitForSingleObject(sink->end_of_stream_event(), 30000) != WAIT_OBJECT_0)
        return E_FAIL;

    ULONGLONG classifier_bytes = 0;
    ULONGLONG stream_bytes = 0;
    const HRESULT graph_error = [&]() {
        HRESULT first_error = S_OK;
        return DrainGraphErrors(events.get(), &first_error) ? first_error : S_OK;
    }();
    const bool counters_valid = diagnostics->GetOpenJocInputByteCounts(
                                    &classifier_bytes, &stream_bytes) == S_OK &&
                                classifier_bytes == 0 && stream_bytes == 0;
    const bool output_valid = IsIec61937Ac3Type(sink->expected_type()) && sink->sample_count() > 0 &&
                              !sink->bytes().empty() && sink->end_of_stream() &&
                              sink->end_of_stream_count() == 1 && sink->end_of_stream_running();
    const HRESULT stop_status = control->Stop();
    const bool complete = SUCCEEDED(graph_error) && SUCCEEDED(stop_status) && counters_valid && output_valid;
    std::wprintf(L"AC3_BITSTREAM_%ls fixture=%ls classifier_input_bytes=%llu "
                 L"stream_input_bytes=%llu samples=%llu bytes=%llu eos=%llu graph_error=0x%08lx\n",
                 complete ? L"COMPLETE" : L"FAILURE", fixture_path.c_str(),
                 static_cast<unsigned long long>(classifier_bytes),
                 static_cast<unsigned long long>(stream_bytes),
                 static_cast<unsigned long long>(sink->sample_count()),
                 static_cast<unsigned long long>(sink->bytes().size()),
                 static_cast<unsigned long long>(sink->end_of_stream_count()),
                 static_cast<unsigned long>(graph_error));
    return complete ? S_OK : E_FAIL;
}

class Task4TestSample final : public IMediaSample
{
  public:
    explicit Task4TestSample(const long capacity, const std::size_t guard_bytes = 32)
        : capacity_(capacity), storage_(capacity >= 0 ? static_cast<std::size_t>(capacity) + guard_bytes
                                                     : guard_bytes,
                                       kCanary)
    {
    }

    STDMETHODIMP QueryInterface(REFIID iid, void **value) override
    {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        if (iid != IID_IUnknown && iid != IID_IMediaSample)
            return E_NOINTERFACE;
        *value = static_cast<IMediaSample *>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++reference_count_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ++release_count_;
        return reference_count_ > 0 ? --reference_count_ : 0;
    }
    STDMETHODIMP GetPointer(BYTE **buffer) override
    {
        if (!buffer)
            return E_POINTER;
        ++get_pointer_count_;
        *buffer = storage_.data();
        return S_OK;
    }
    STDMETHODIMP_(LONG) GetSize() override { return capacity_; }
    STDMETHODIMP GetTime(REFERENCE_TIME *, REFERENCE_TIME *) override { return E_NOTIMPL; }
    STDMETHODIMP SetTime(REFERENCE_TIME *, REFERENCE_TIME *) override { return S_OK; }
    STDMETHODIMP IsSyncPoint() override { return S_FALSE; }
    STDMETHODIMP SetSyncPoint(BOOL) override { return S_OK; }
    STDMETHODIMP IsPreroll() override { return S_FALSE; }
    STDMETHODIMP SetPreroll(BOOL) override { return S_OK; }
    STDMETHODIMP_(LONG) GetActualDataLength() override { return actual_length_; }
    STDMETHODIMP SetActualDataLength(LONG length) override
    {
        ++set_actual_length_count_;
        if (length < 0 || length > capacity_)
            return VFW_E_BUFFER_OVERFLOW;
        actual_length_ = length;
        return S_OK;
    }
    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE **media_type) override
    {
        if (!media_type)
            return E_POINTER;
        *media_type = nullptr;
        return S_FALSE;
    }
    STDMETHODIMP SetMediaType(AM_MEDIA_TYPE *) override
    {
        ++set_media_type_count_;
        return S_OK;
    }
    STDMETHODIMP IsDiscontinuity() override { return S_FALSE; }
    STDMETHODIMP SetDiscontinuity(BOOL) override { return S_OK; }
    STDMETHODIMP GetMediaTime(LONGLONG *, LONGLONG *) override { return E_NOTIMPL; }
    STDMETHODIMP SetMediaTime(LONGLONG *, LONGLONG *) override { return S_OK; }

    static constexpr BYTE kCanary = 0xa5;
    const std::vector<BYTE> &storage() const { return storage_; }
    std::uint64_t release_count() const { return release_count_; }
    std::uint64_t set_actual_length_count() const { return set_actual_length_count_; }
    std::uint64_t set_media_type_count() const { return set_media_type_count_; }

  private:
    ULONG reference_count_ = 1;
    long capacity_ = 0;
    long actual_length_ = 0;
    std::vector<BYTE> storage_;
    std::uint64_t release_count_ = 0;
    std::uint64_t get_pointer_count_ = 0;
    std::uint64_t set_actual_length_count_ = 0;
    std::uint64_t set_media_type_count_ = 0;
};

HRESULT CopyIntoTask4Sample(IMediaSample *sample, const BYTE *payload, const long required_bytes,
                            std::uint64_t *set_actual_length_count,
                            std::uint64_t *copy_count)
{
    if (!sample || !payload || !set_actual_length_count || !copy_count || required_bytes < 0)
        return E_INVALIDARG;
    if (required_bytes > sample->GetSize())
        return VFW_E_BUFFER_UNDERFLOW;
    BYTE *destination = nullptr;
    HRESULT status = sample->GetPointer(&destination);
    if (FAILED(status) || !destination)
        return FAILED(status) ? status : E_POINTER;
    status = sample->SetActualDataLength(required_bytes);
    if (FAILED(status))
        return status;
    ++*set_actual_length_count;
    std::memcpy(destination, payload, static_cast<std::size_t>(required_bytes));
    ++*copy_count;
    return S_OK;
}

struct Task4BoundaryCounters
{
    std::uint64_t query_count = 0;
    std::uint64_t reconnect_count = 0;
    std::uint64_t acquire_count = 0;
    std::uint64_t sample_type_count = 0;
    std::uint64_t output_type_count = 0;
    std::uint64_t set_actual_length_count = 0;
    std::uint64_t copy_count = 0;
    std::uint64_t deliver_count = 0;
};

bool TestTask4AllocatorBoundaries()
{
    const LAVOpenJocOutputContract *contract =
        FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout714);
    if (!contract)
        return false;
    const CMediaType exact_type = BuildStrictTarget(*contract);
    constexpr std::size_t frames = 256;
    std::size_t checked_bytes = 0;
    long required_bytes = 0;
    if (!CheckedLAVOpenJocPcmByteCount(frames,
                                       static_cast<std::uint16_t>(contract->channel_count * sizeof(float)),
                                       &checked_bytes) ||
        !CheckedLAVOpenJocLongNarrow(checked_bytes, &required_bytes) || required_bytes <= 1)
        return false;
    std::vector<BYTE> payload(checked_bytes);
    for (std::size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<BYTE>((index * 37u + 11u) & 0xffu);

    auto operations_for = [&](Task4TestSample &test_sample, Task4BoundaryCounters &counters) {
        LAVOpenJocStrictDeliveryOperations operations;
        operations.query_accept = [&](const AM_MEDIA_TYPE &) {
            ++counters.query_count;
            return S_OK;
        };
        operations.reconnect = [&](long, const AM_MEDIA_TYPE &) {
            ++counters.reconnect_count;
            return S_OK;
        };
        operations.acquire_sample = [&](LAVOpenJocStrictAcquiredSample *acquired) {
            ++counters.acquire_count;
            acquired->handle = static_cast<IMediaSample *>(&test_sample);
            acquired->capacity = test_sample.GetSize();
            return test_sample.GetPointer(&acquired->data);
        };
        operations.release_attached_type = [](AM_MEDIA_TYPE *) {};
        operations.release_sample = [](void *handle) {
            static_cast<IMediaSample *>(handle)->Release();
        };
        operations.set_sample_media_type = [&](void *handle, const AM_MEDIA_TYPE &candidate) {
            ++counters.sample_type_count;
            return static_cast<IMediaSample *>(handle)->SetMediaType(
                const_cast<AM_MEDIA_TYPE *>(&candidate));
        };
        operations.set_output_media_type = [&](const AM_MEDIA_TYPE &) {
            ++counters.output_type_count;
            return S_OK;
        };
        operations.deliver = [&](void *handle, BYTE *, long bytes) {
            ++counters.deliver_count;
            return CopyIntoTask4Sample(static_cast<IMediaSample *>(handle), payload.data(), bytes,
                                       &counters.set_actual_length_count, &counters.copy_count);
        };
        return operations;
    };

    Task4TestSample short_sample(required_bytes - 1);
    const std::vector<BYTE> canary = short_sample.storage();
    Task4BoundaryCounters short_counters;
    auto short_operations = operations_for(short_sample, short_counters);
    const HRESULT short_status = DeliverLAVOpenJocStrictMediaType(
        contract, exact_type, true, required_bytes, short_operations);
    if (short_status != VFW_E_BUFFER_UNDERFLOW || short_sample.storage() != canary ||
        short_sample.GetActualDataLength() != 0 || short_sample.release_count() != 1 ||
        short_sample.set_media_type_count() != 0 || short_counters.sample_type_count != 0 ||
        short_counters.output_type_count != 0 ||
        short_counters.set_actual_length_count != 0 || short_counters.copy_count != 0 ||
        short_counters.deliver_count != 0)
        return false;
    std::wprintf(L"TASK4_ALLOCATOR_BOUNDARY case=required_minus_one required=%ld capacity=%ld "
                 L"hr=0x%08lx set_type=0 set_actual=0 copy=0 deliver=0 release=1 canary=1\n",
                 required_bytes, required_bytes - 1, static_cast<unsigned long>(short_status));

    Task4TestSample exact_sample(required_bytes);
    Task4BoundaryCounters exact_counters;
    auto exact_operations = operations_for(exact_sample, exact_counters);
    const HRESULT exact_status = DeliverLAVOpenJocStrictMediaType(
        contract, exact_type, true, required_bytes, exact_operations);
    const bool payload_exact =
        std::equal(payload.begin(), payload.end(), exact_sample.storage().begin());
    const bool guard_exact = std::all_of(exact_sample.storage().begin() + required_bytes,
                                         exact_sample.storage().end(),
                                         [](BYTE value) { return value == Task4TestSample::kCanary; });
    if (exact_status != S_OK || exact_sample.GetActualDataLength() != required_bytes ||
        exact_sample.release_count() != 1 || exact_sample.set_media_type_count() != 1 ||
        exact_counters.sample_type_count != 1 || exact_counters.output_type_count != 1 ||
        exact_counters.set_actual_length_count != 1 || exact_counters.copy_count != 1 ||
        exact_counters.deliver_count != 1 || !payload_exact || !guard_exact)
        return false;
    std::wprintf(L"TASK4_ALLOCATOR_BOUNDARY case=exact_capacity required=%ld capacity=%ld "
                 L"hr=0x%08lx set_type=1 set_actual=1 copy=1 deliver=1 release=1 "
                 L"payload=exact canary=1\n",
                 required_bytes, required_bytes, static_cast<unsigned long>(exact_status));

    std::uint32_t sample_sum = 19;
    std::size_t byte_count = 23;
    long narrowed = 29;
    long grown = 31;
    const std::size_t safe_frames =
        (std::numeric_limits<std::size_t>::max)() / static_cast<std::size_t>(48);
    const long allocator_safe = (std::numeric_limits<long>::max)() -
                                (std::numeric_limits<long>::max)() / 3;
    const bool arithmetic_ok =
        !CheckedLAVOpenJocSampleAdd((std::numeric_limits<std::uint32_t>::max)(), 1, &sample_sum) &&
        sample_sum == 0 &&
        CheckedLAVOpenJocPcmByteCount(safe_frames, 48, &byte_count) &&
        byte_count == safe_frames * 48 &&
        !CheckedLAVOpenJocPcmByteCount(safe_frames + 1, 48, &byte_count) && byte_count == 0 &&
        !CheckedLAVOpenJocPcmByteCount((std::numeric_limits<std::size_t>::max)(), 48,
                                       &byte_count) &&
        CheckedLAVOpenJocLongNarrow(
            static_cast<std::size_t>((std::numeric_limits<long>::max)()), &narrowed) &&
        narrowed == (std::numeric_limits<long>::max)() &&
        !CheckedLAVOpenJocLongNarrow(
            static_cast<std::size_t>((std::numeric_limits<long>::max)()) + 1u, &narrowed) &&
        narrowed == 0 && CheckedLAVOpenJocAllocatorGrowth(allocator_safe, &grown) &&
        grown == (std::numeric_limits<long>::max)() &&
        !CheckedLAVOpenJocAllocatorGrowth(allocator_safe + 1, &grown) && grown == 0;
    if (!arithmetic_ok)
        return false;
    std::wprintf(L"TASK4_SAMPLE_OVERFLOW sample_add=checked pcm_safe=%llu pcm_overflow=failed "
                 L"long_max=checked allocator_safe=%ld allocator_overflow=failed\n",
                 static_cast<unsigned long long>(safe_frames), allocator_safe);

    std::uint64_t flush_count = 0;
    std::uint64_t metadata_count = 0;
    std::uint64_t swap_count = 0;
    std::uint64_t append_count = 0;
    LAVOpenJocQueueTransactionOperations queue_operations;
    queue_operations.flush = [&]() { ++flush_count; return S_OK; };
    queue_operations.prepare_metadata = [&]() { ++metadata_count; return S_OK; };
    queue_operations.swap_buffer = [&]() { ++swap_count; };
    queue_operations.append_buffer = [&]() { ++append_count; return S_OK; };
    const LAVOpenJocQueueTransactionInput queue_input{
        true, (std::numeric_limits<std::uint32_t>::max)(), 1, 101, 202, 48000};
    const LAVOpenJocQueueTransactionResult sentinel{0x13579bdfu, 0x2468ace0};
    LAVOpenJocQueueTransactionResult queue_result = sentinel;
    const HRESULT queue_status =
        ExecuteLAVOpenJocQueueTransaction(queue_input, queue_operations, &queue_result);
    if (queue_status != HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW) ||
        queue_result.sample_count != sentinel.sample_count ||
        queue_result.start_time != sentinel.start_time || flush_count != 0 ||
        metadata_count != 0 || swap_count != 0 || append_count != 0)
        return false;
    std::wprintf(L"TASK4_QUEUE_OVERFLOW hr=0x%08lx sentinel=unchanged flush=0 metadata=0 "
                 L"swap=0 append=0\n",
                 static_cast<unsigned long>(queue_status));
    return true;
}

bool TestTask4CycleEvidence()
{
    using openjoc_harness_core::Task4CycleEvidence;
    using openjoc_harness_core::Task4CycleEvidenceIsValid;
    using openjoc_harness_core::Task4CycleMatchesBaseline;
    Task4CycleEvidence baseline;
    baseline.source_type_exact = baseline.output_type_exact =
        baseline.graph_exactly_three_filters = baseline.paused = baseline.running =
            baseline.no_graph_error = baseline.timestamps_valid =
                baseline.sample_contracts_valid = baseline.allocator_valid =
                    baseline.runtime_identity_valid = baseline.fixture_identity_valid =
                        baseline.policy_exact = baseline.end_of_stream_running = true;
    baseline.samples = 4;
    baseline.bytes = 4096;
    baseline.timestamp_count = 4;
    baseline.payload_digest[0] = 0x5a;
    baseline.first_timestamp = 0;
    baseline.last_timestamp = 320000;
    baseline.end_of_stream_count = 1;
    baseline.allocator_requested_capacity = 4096;
    baseline.allocator_actual_capacity = 8192;
    baseline.sample_capacity_high_water = 8192;
    baseline.sample_actual_length_high_water = 1024;
    baseline.checked_total_bytes = 4096;
    baseline.actual_length_total = 4096;
    if (!Task4CycleEvidenceIsValid(baseline) || !Task4CycleMatchesBaseline(baseline, baseline))
        return false;

    auto rejects = [&](Task4CycleEvidence changed) {
        return !Task4CycleEvidenceIsValid(changed) ||
               !Task4CycleMatchesBaseline(baseline, changed);
    };
    std::vector<Task4CycleEvidence> boolean_failures;
    auto add_boolean_failure = [&](bool Task4CycleEvidence::*field) {
        Task4CycleEvidence changed = baseline;
        changed.*field = false;
        boolean_failures.push_back(changed);
    };
    add_boolean_failure(&Task4CycleEvidence::source_type_exact);
    add_boolean_failure(&Task4CycleEvidence::output_type_exact);
    add_boolean_failure(&Task4CycleEvidence::graph_exactly_three_filters);
    add_boolean_failure(&Task4CycleEvidence::paused);
    add_boolean_failure(&Task4CycleEvidence::running);
    add_boolean_failure(&Task4CycleEvidence::no_graph_error);
    add_boolean_failure(&Task4CycleEvidence::timestamps_valid);
    add_boolean_failure(&Task4CycleEvidence::sample_contracts_valid);
    add_boolean_failure(&Task4CycleEvidence::allocator_valid);
    add_boolean_failure(&Task4CycleEvidence::runtime_identity_valid);
    add_boolean_failure(&Task4CycleEvidence::fixture_identity_valid);
    add_boolean_failure(&Task4CycleEvidence::policy_exact);
    add_boolean_failure(&Task4CycleEvidence::end_of_stream_running);
    for (const auto &changed : boolean_failures)
        if (!rejects(changed))
            return false;

    Task4CycleEvidence changed = baseline;
    changed.samples++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.bytes++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.timestamp_count++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.payload_digest[0] ^= 1;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.first_timestamp++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.last_timestamp++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.end_of_stream_count++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.allocator_requested_capacity++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.allocator_actual_capacity++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.sample_capacity_high_water++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.sample_actual_length_high_water++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.checked_total_bytes++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.actual_length_total++;
    if (!rejects(changed)) return false;
    changed = baseline;
    changed.samples = 0;
    if (Task4CycleEvidenceIsValid(changed)) return false;
    changed = baseline;
    changed.actual_length_total = changed.bytes - 1;
    if (Task4CycleEvidenceIsValid(changed)) return false;
    return true;
}

bool TestTask4WorkingSetTrends()
{
    using openjoc_harness_core::Task4TrendEvidence;
    using openjoc_harness_core::WorkingSetTrendIsBounded;
    constexpr std::size_t page = 4096;
    std::vector<std::size_t> flat(128, page * 1000);
    std::vector<std::size_t> noise(128, page * 1000);
    std::vector<std::size_t> linear(128, page * 1000);
    std::vector<std::size_t> slow_linear(128, page * 1000);
    std::vector<std::size_t> quartile_steps(128, page * 1000);
    std::vector<std::size_t> slow_quartile_steps(128, page * 1000);
    std::vector<std::size_t> late_step_only(128, page * 1000);
    std::vector<std::size_t> allocator_high_state_frequency(128, page * 1000);
    std::vector<std::size_t> tail_linear(128, page * 1000);
    std::vector<std::size_t> early_step_then_plateau(128, page * 1000);
    for (std::size_t index = 0; index < 128; ++index)
    {
        noise[index] += index % 2 == 0 ? page : 0;
        linear[index] += index * page;
        slow_linear[index] += (index / 4) * page;
        if (index >= 32) quartile_steps[index] += page * 43;
        if (index >= 64) quartile_steps[index] += page * 43;
        if (index >= 96) quartile_steps[index] += page * 43;
        if (index >= 32) slow_quartile_steps[index] += page * 16;
        if (index >= 64) slow_quartile_steps[index] += page * 16;
        if (index >= 96) slow_quartile_steps[index] += page * 16;
        if (index >= 32) late_step_only[index] += page;
        if (index >= 64) late_step_only[index] += page;
        if (index >= 96) late_step_only[index] += page * 128;
        const std::size_t quartile = index / 32;
        if (index % 32 >= 9)
            allocator_high_state_frequency[index] += page * 43 * quartile;
        if (index >= 64) tail_linear[index] += (index - 64) * page;
        if (index >= 32) early_step_then_plateau[index] += page * 200;
    }
    std::vector<std::size_t> incomplete(flat.begin(), flat.end() - 1);
    std::vector<std::size_t> excessive = flat;
    excessive.push_back(flat.back());
    Task4TrendEvidence evidence;
    if (!WorkingSetTrendIsBounded(flat, page, &evidence) || evidence.gate_a ||
        evidence.gate_b || evidence.tail_gate)
        return false;
    if (!WorkingSetTrendIsBounded(noise, page, &evidence) || evidence.gate_a ||
        evidence.gate_b || evidence.tail_gate)
        return false;
    if (WorkingSetTrendIsBounded(linear, page, &evidence) || !evidence.gate_a)
        return false;
    if (WorkingSetTrendIsBounded(slow_linear, page, &evidence) || !evidence.gate_a)
        return false;
    if (WorkingSetTrendIsBounded(quartile_steps, page, &evidence) || !evidence.gate_b)
        return false;
    if (WorkingSetTrendIsBounded(slow_quartile_steps, page, &evidence) || !evidence.gate_a)
        return false;
    if (!WorkingSetTrendIsBounded(late_step_only, page, &evidence) || evidence.gate_b)
        return false;
    if (!WorkingSetTrendIsBounded(allocator_high_state_frequency, page, &evidence) ||
        evidence.gate_b)
        return false;
    if (WorkingSetTrendIsBounded(tail_linear, page, &evidence) || evidence.gate_a ||
        evidence.gate_b || !evidence.tail_gate)
        return false;
    if (!WorkingSetTrendIsBounded(early_step_then_plateau, page, &evidence))
        return false;
    if (WorkingSetTrendIsBounded(incomplete, page, &evidence) ||
        WorkingSetTrendIsBounded(excessive, page, &evidence) ||
        WorkingSetTrendIsBounded(flat, 0, &evidence))
        return false;
    return true;
}

struct Task4ProcessMemory
{
    std::size_t working_set = 0;
    std::size_t private_usage = 0;
};

bool ReadTask4ProcessMemory(Task4ProcessMemory *memory)
{
    if (!memory)
        return false;
    *memory = {};
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                              sizeof(counters)))
        return false;
    memory->working_set = counters.WorkingSetSize;
    memory->private_usage = counters.PrivateUsage;
    return memory->working_set > 0 && memory->private_usage > 0;
}

HRESULT RunOneTask4GraphCycle(
    const PrivateComModule &audio, const PrivateComModule &splitter,
    const FixtureIdentity &fixture, const LAVOpenJocOutputPolicy policy,
    const bool runtime_identity_valid, const bool fixture_identity_valid,
    openjoc_harness_core::Task4CycleEvidence *evidence)
{
    if (!evidence)
        return E_POINTER;
    *evidence = {};
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    if (!contract || !runtime_identity_valid || !fixture_identity_valid)
    {
        std::wprintf(L"TASK4_CYCLE_DIAGNOSTIC stage=precondition policy_value=%u "
                     L"contract=%d runtime_identity=%d fixture_identity=%d hr=0x%08lx\n",
                     static_cast<unsigned>(policy), contract ? 1 : 0,
                     runtime_identity_valid ? 1 : 0, fixture_identity_valid ? 1 : 0,
                     static_cast<unsigned long>(E_INVALIDARG));
        return E_INVALIDARG;
    }
    const CMediaType target = BuildStrictTarget(*contract);
    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source_filter;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    CMediaType exact_eac3;
    HRESULT status = CreateGraphForFixture(audio, splitter, fixture.final_path, policy, true,
                                           graph.put(), source_filter.put(), audio_filter.put(),
                                           source_output.put(), audio_input.put(), audio_output.put(),
                                           &exact_eac3);
    if (FAILED(status))
    {
        std::wprintf(L"TASK4_CYCLE_DIAGNOSTIC stage=create_graph policy=%hs hr=0x%08lx\n",
                     contract->property_page_label, static_cast<unsigned long>(status));
        return status;
    }
    HRESULT sink_status = S_OK;
    auto *sink = new (std::nothrow)
        StrictCaptureSink(target, false, std::vector<CMediaType>{}, &sink_status);
    if (!sink)
    {
        std::wprintf(L"TASK4_CYCLE_DIAGNOSTIC stage=create_sink policy=%hs "
                     L"hr=0x%08lx\n",
                     contract->property_page_label, static_cast<unsigned long>(E_OUTOFMEMORY));
        return E_OUTOFMEMORY;
    }
    if (FAILED(sink_status))
    {
        std::wprintf(L"TASK4_CYCLE_DIAGNOSTIC stage=create_sink policy=%hs hr=0x%08lx\n",
                     contract->property_page_label, static_cast<unsigned long>(sink_status));
        delete sink;
        return sink_status;
    }
    sink->AddRef();
    ComOwner<IBaseFilter> sink_owner;
    sink_owner.attach(static_cast<IBaseFilter *>(sink));
    status = AttachCaptureSink(graph.get(), audio_output.get(), sink, target);
    if (FAILED(status))
    {
        std::wprintf(L"TASK4_CYCLE_DIAGNOSTIC stage=attach_sink policy=%hs hr=0x%08lx\n",
                     contract->property_page_label, static_cast<unsigned long>(status));
        return status;
    }
    const bool graph_exact = GraphContainsExactly(graph.get(), 3);
    const bool source_exact = ExactConnectionTypes(source_output.get(), audio_input.get(), exact_eac3);
    const CMediaType negotiated = target;
    const bool output_exact = ExactConnectionTypes(audio_output.get(), sink->input(), negotiated);

    ComOwner<ILAVOpenJocSettings> settings;
    LAVOpenJocOutputPolicy actual_policy = LAVOpenJocOutputPolicy::Stereo;
    const bool policy_exact =
        audio_filter->QueryInterface(__uuidof(ILAVOpenJocSettings),
                                     reinterpret_cast<void **>(settings.put())) == S_OK &&
        settings->GetOutputPolicy(&actual_policy) == S_OK && actual_policy == policy;
    ComOwner<IMediaControl> control;
    ComOwner<IMediaEvent> events;
    if (FAILED(status = graph->QueryInterface(IID_IMediaControl,
                                              reinterpret_cast<void **>(control.put()))) ||
        FAILED(status = graph->QueryInterface(IID_IMediaEvent,
                                              reinterpret_cast<void **>(events.put()))))
    {
        std::wprintf(L"TASK4_CYCLE_DIAGNOSTIC stage=query_graph_control policy=%hs "
                     L"hr=0x%08lx\n",
                     contract->property_page_label, static_cast<unsigned long>(status));
        return status;
    }
    OAFilterState state = State_Stopped;
    const HRESULT pause_status = control->Pause();
    const HRESULT pause_state_status = control->GetState(10000, &state);
    const OAFilterState pause_state = state;
    const bool paused = pause_status == S_OK && pause_state_status == S_OK && state == State_Paused;
    state = State_Stopped;
    const HRESULT run_status = control->Run();
    const HRESULT run_state_status = control->GetState(10000, &state);
    const OAFilterState run_state = state;
    const bool running = run_status == S_OK && run_state_status == S_OK && state == State_Running;
    const DWORD eos_wait = WaitForSingleObject(sink->end_of_stream_event(), 30000);
    const HRESULT stop_status = control->Stop();
    HRESULT graph_error_status = S_OK;
    const bool graph_error = DrainGraphErrors(events.get(), &graph_error_status);

    const std::vector<CapturedSampleEvidence> samples = sink->samples();
    const std::vector<BYTE> bytes = sink->bytes();
    std::size_t checked_total = 0;
    std::uint64_t actual_total = 0;
    long capacity_high_water = 0;
    long actual_high_water = 0;
    const std::uint16_t block_align =
        static_cast<std::uint16_t>(contract->channel_count * sizeof(float));
    bool checked_lengths = !samples.empty() && block_align > 0;
    for (const auto &sample : samples)
    {
        if (sample.length <= 0 || sample.capacity <= 0 ||
            sample.length % static_cast<long>(block_align) != 0)
        {
            checked_lengths = false;
            break;
        }
        const std::size_t frames = static_cast<std::size_t>(sample.length) / block_align;
        std::size_t checked_sample = 0;
        long narrowed_sample = 0;
        if (!CheckedLAVOpenJocPcmByteCount(frames, block_align, &checked_sample) ||
            !CheckedLAVOpenJocLongNarrow(checked_sample, &narrowed_sample) ||
            narrowed_sample != sample.length || checked_total >
                (std::numeric_limits<std::size_t>::max)() - checked_sample ||
            actual_total > (std::numeric_limits<std::uint64_t>::max)() -
                               static_cast<std::uint64_t>(sample.length))
        {
            checked_lengths = false;
            break;
        }
        checked_total += checked_sample;
        actual_total += static_cast<std::uint64_t>(sample.length);
        capacity_high_water = (std::max)(capacity_high_water, sample.capacity);
        actual_high_water = (std::max)(actual_high_water, sample.length);
    }
    const ALLOCATOR_PROPERTIES requested = sink->allocator_requested_properties();
    const ALLOCATOR_PROPERTIES actual = sink->allocator_actual_properties();
    Digest digest{};
    const bool digest_ok = Sha256Bytes(bytes, &digest);
    bool attached_types_exact = true;
    for (const auto &attached : sink->sample_attached_types())
        attached_types_exact = attached_types_exact &&
                               openjoc_harness_core::ExactMediaTypeEqual(negotiated, attached);
    bool set_types_exact = true;
    for (const auto &set_type : sink->set_media_types())
        set_types_exact = set_types_exact &&
                          openjoc_harness_core::ExactMediaTypeEqual(negotiated, set_type);

    evidence->source_type_exact = source_exact &&
                                  ExactConnectionTypes(source_output.get(), audio_input.get(), exact_eac3);
    evidence->output_type_exact = output_exact && attached_types_exact && set_types_exact &&
                                  ExactConnectionTypes(audio_output.get(), sink->input(), negotiated);
    evidence->graph_exactly_three_filters = graph_exact && GraphContainsExactly(graph.get(), 3);
    evidence->paused = paused;
    evidence->running = running;
    evidence->no_graph_error = !graph_error && stop_status == S_OK &&
                               graph_error_status == S_OK && eos_wait == WAIT_OBJECT_0;
    evidence->timestamps_valid = sink->sample_contracts_valid();
    evidence->sample_contracts_valid = sink->sample_contracts_valid() && checked_lengths;
    evidence->allocator_valid = sink->allocator_contract_valid();
    evidence->runtime_identity_valid = runtime_identity_valid;
    evidence->fixture_identity_valid = fixture_identity_valid;
    evidence->policy_exact = policy_exact;
    evidence->end_of_stream_running = sink->end_of_stream_running();
    evidence->samples = sink->sample_count();
    evidence->bytes = bytes.size();
    evidence->timestamp_count = sink->timestamp_observation_count();
    evidence->payload_digest = digest;
    evidence->first_timestamp = samples.empty() ? 0 : samples.front().start;
    evidence->last_timestamp = samples.empty() ? 0 : samples.back().stop;
    evidence->end_of_stream_count = sink->end_of_stream_count();
    evidence->allocator_requested_capacity = requested.cbBuffer;
    evidence->allocator_actual_capacity = actual.cbBuffer;
    evidence->sample_capacity_high_water = capacity_high_water;
    evidence->sample_actual_length_high_water = actual_high_water;
    evidence->checked_total_bytes = checked_total;
    evidence->actual_length_total = actual_total;
    const bool evidence_valid = openjoc_harness_core::Task4CycleEvidenceIsValid(*evidence);
    if (!digest_ok || !evidence_valid)
    {
        std::wprintf(
            L"TASK4_CYCLE_DIAGNOSTIC stage=evidence policy=%hs "
            L"pause_hr=0x%08lx pause_state_hr=0x%08lx pause_state=%d "
            L"run_hr=0x%08lx run_state_hr=0x%08lx run_state=%d "
            L"stop_hr=0x%08lx graph_error_hr=0x%08lx eos_wait=%lu "
            L"source_exact=%d output_exact=%d graph3=%d paused=%d running=%d "
            L"no_error=%d timestamps=%d sample_contracts=%d allocator=%d "
            L"runtime=%d fixture=%d policy_exact=%d eos_running=%d digest_ok=%d "
            L"samples=%llu bytes=%llu timestamp_count=%llu first=%lld last=%lld "
            L"eos=%llu requested=%ld actual=%ld GetSize_high_water=%ld "
            L"ActualDataLength_high_water=%ld checked_bytes=%llu actual_length_bytes=%llu\n",
            contract->property_page_label, static_cast<unsigned long>(pause_status),
            static_cast<unsigned long>(pause_state_status), static_cast<int>(pause_state),
            static_cast<unsigned long>(run_status), static_cast<unsigned long>(run_state_status),
            static_cast<int>(run_state), static_cast<unsigned long>(stop_status),
            static_cast<unsigned long>(graph_error_status), static_cast<unsigned long>(eos_wait),
            evidence->source_type_exact ? 1 : 0, evidence->output_type_exact ? 1 : 0,
            evidence->graph_exactly_three_filters ? 1 : 0, evidence->paused ? 1 : 0,
            evidence->running ? 1 : 0, evidence->no_graph_error ? 1 : 0,
            evidence->timestamps_valid ? 1 : 0, evidence->sample_contracts_valid ? 1 : 0,
            evidence->allocator_valid ? 1 : 0, evidence->runtime_identity_valid ? 1 : 0,
            evidence->fixture_identity_valid ? 1 : 0, evidence->policy_exact ? 1 : 0,
            evidence->end_of_stream_running ? 1 : 0, digest_ok ? 1 : 0,
            static_cast<unsigned long long>(evidence->samples),
            static_cast<unsigned long long>(evidence->bytes),
            static_cast<unsigned long long>(evidence->timestamp_count),
            static_cast<long long>(evidence->first_timestamp),
            static_cast<long long>(evidence->last_timestamp),
            static_cast<unsigned long long>(evidence->end_of_stream_count),
            evidence->allocator_requested_capacity, evidence->allocator_actual_capacity,
            evidence->sample_capacity_high_water, evidence->sample_actual_length_high_water,
            static_cast<unsigned long long>(evidence->checked_total_bytes),
            static_cast<unsigned long long>(evidence->actual_length_total));
    }
    return digest_ok && evidence_valid ? S_OK : E_FAIL;
}

std::wstring Task4SequenceText(const std::vector<std::size_t> &values)
{
    std::wostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
            stream << L',';
        stream << static_cast<unsigned long long>(values[index]);
    }
    return stream.str();
}

void LogTask4Trend(const wchar_t *metric, const LAVOpenJocOutputContract &contract,
                   const std::size_t page_size, const std::vector<std::size_t> &values,
                   const openjoc_harness_core::Task4TrendEvidence &trend, const bool bounded)
{
    const std::wstring sequence = Task4SequenceText(values);
    const wchar_t *verdict =
        values.size() != 128 ? L"INCOMPLETE" : bounded ? L"BOUNDED" : L"GROWTH_DETECTED";
    std::wprintf(
        L"TASK4_MEMORY_TREND policy=%hs metric=%ls page_size=%llu samples=%llu "
        L"full_slope=%.3f full_r2=%.6f tail_slope=%.3f tail_r2=%.6f "
        L"q1=%llu q2=%llu q3=%llu q4=%llu lq1=%llu lq2=%llu lq3=%llu lq4=%llu "
        L"h1=%llu h2=%llu "
        L"gate_a_slope_pages=0.125 gate_a_r2=0.90 gate_a_delta_pages=16 "
        L"gate_b_quantile=p25 gate_b_step_pages=43 gate_b_delta_pages=128 "
        L"tail_slope_pages=1 tail_r2=0.85 "
        L"tail_delta_pages=32 gate_a=%d gate_b=%d tail_gate=%d verdict=%ls sequence=%ls\n",
        contract.property_page_label, metric, static_cast<unsigned long long>(page_size),
        static_cast<unsigned long long>(values.size()), trend.full_slope, trend.full_r_squared,
        trend.tail_slope, trend.tail_r_squared,
        static_cast<unsigned long long>(trend.quartile_medians[0]),
        static_cast<unsigned long long>(trend.quartile_medians[1]),
        static_cast<unsigned long long>(trend.quartile_medians[2]),
        static_cast<unsigned long long>(trend.quartile_medians[3]),
        static_cast<unsigned long long>(trend.quartile_lower_quartiles[0]),
        static_cast<unsigned long long>(trend.quartile_lower_quartiles[1]),
        static_cast<unsigned long long>(trend.quartile_lower_quartiles[2]),
        static_cast<unsigned long long>(trend.quartile_lower_quartiles[3]),
        static_cast<unsigned long long>(trend.tail_half_medians[0]),
        static_cast<unsigned long long>(trend.tail_half_medians[1]), trend.gate_a ? 1 : 0,
        trend.gate_b ? 1 : 0, trend.tail_gate ? 1 : 0, verdict, sequence.c_str());
}

HRESULT RunTask4AllocatorPerformance(const std::filesystem::path &runtime_dir,
                                     const std::filesystem::path &manifest_path,
                                     const std::filesystem::path &fixture_path)
{
    constexpr std::size_t kTask4WarmupCycles = 16;
    constexpr std::size_t kTask4MeasuredCycles = 128;
    const bool allocator_self_test = TestTask4AllocatorBoundaries();
    const bool cycle_self_test = TestTask4CycleEvidence();
    const bool trend_self_test = TestTask4WorkingSetTrends();
    std::wprintf(L"TASK4_SELF_TEST allocator=%d cycle=%d trend=%d\n",
                 allocator_self_test ? 1 : 0, cycle_self_test ? 1 : 0,
                 trend_self_test ? 1 : 0);
    if (!allocator_self_test || !cycle_self_test || !trend_self_test)
        return E_UNEXPECTED;
    std::vector<StagedRecord> records;
    if (!ReadStagedManifest(runtime_dir, manifest_path, &records))
        return E_INVALIDARG;
    const StagedRecord *audio_record = FindRecord(records, StagedKind::Module, L"LAVAudio.ax");
    const StagedRecord *splitter_record = FindRecord(records, StagedKind::Module, L"LAVSplitter.ax");
    if (!audio_record || !splitter_record)
        return E_UNEXPECTED;
    const std::wstring runtime_final = FinalPathForFile(runtime_dir);
    ScopedActivationContext activation(audio_record->final_path, runtime_final);
    if (!activation.active())
        return HRESULT_FROM_WIN32(GetLastError());
    PrivateComModule audio(audio_record->final_path, kTargetLavAudio);
    PrivateComModule splitter(splitter_record->final_path, kLavSplitterSource);
    std::wprintf(L"TASK4_STAGE activation=%d audio_hr=0x%08lx splitter_hr=0x%08lx\n",
                 activation.active() ? 1 : 0, static_cast<unsigned long>(audio.status()),
                 static_cast<unsigned long>(splitter.status()));
    if (FAILED(audio.status()) || FAILED(splitter.status()))
        return E_UNEXPECTED;
    std::vector<HMODULE> dependencies;
    if (!LoadStagedDependencies(records, &dependencies))
        return E_UNEXPECTED;
    FixtureIdentity fixture;
    const bool fixture_identity_valid = BuildFixtureIdentity(fixture_path, &fixture) &&
                                        FixtureIdentityMatches(fixture);
    const bool runtime_identity_valid = RuntimeIdentityMatches(records);
    std::wprintf(L"TASK4_STAGE dependencies=%llu fixture_identity=%d runtime_identity=%d\n",
                 static_cast<unsigned long long>(dependencies.size()),
                 fixture_identity_valid ? 1 : 0, runtime_identity_valid ? 1 : 0);
    HRESULT status = fixture_identity_valid && runtime_identity_valid ? S_OK : E_UNEXPECTED;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::size_t page_size = system_info.dwPageSize;
    constexpr std::array<LAVOpenJocOutputPolicy, 3> policies = {
        LAVOpenJocOutputPolicy::Stereo, LAVOpenJocOutputPolicy::Layout51,
        LAVOpenJocOutputPolicy::Layout714};
    for (const auto policy : policies)
    {
        if (FAILED(status))
            break;
        const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
        if (!contract)
        {
            status = E_UNEXPECTED;
            break;
        }
        openjoc_harness_core::Task4CycleEvidence warmup_baseline;
        bool have_warmup_baseline = false;
        for (std::size_t cycle = 0; cycle < kTask4WarmupCycles && SUCCEEDED(status); ++cycle)
        {
            openjoc_harness_core::Task4CycleEvidence current;
            status = RunOneTask4GraphCycle(audio, splitter, fixture, policy,
                                           runtime_identity_valid, fixture_identity_valid, &current);
            if (FAILED(status))
                std::wprintf(L"TASK4_CYCLE_STATUS policy=%hs phase=warmup cycle=%llu "
                             L"hr=0x%08lx\n",
                             contract->property_page_label,
                             static_cast<unsigned long long>(cycle),
                             static_cast<unsigned long>(status));
            if (SUCCEEDED(status))
            {
                if (!have_warmup_baseline)
                {
                    warmup_baseline = current;
                    have_warmup_baseline = true;
                }
                else if (!openjoc_harness_core::Task4CycleMatchesBaseline(warmup_baseline, current))
                    status = E_FAIL;
            }
        }
        std::vector<std::size_t> working_set;
        std::vector<std::size_t> private_usage;
        working_set.reserve(kTask4MeasuredCycles);
        private_usage.reserve(kTask4MeasuredCycles);
        openjoc_harness_core::Task4CycleEvidence measured_baseline;
        bool have_measured_baseline = false;
        const ULONGLONG started = GetTickCount64();
        for (std::size_t cycle = 0; cycle < kTask4MeasuredCycles && SUCCEEDED(status); ++cycle)
        {
            openjoc_harness_core::Task4CycleEvidence current;
            status = RunOneTask4GraphCycle(audio, splitter, fixture, policy,
                                           runtime_identity_valid, fixture_identity_valid, &current);
            Task4ProcessMemory memory;
            const bool memory_read = ReadTask4ProcessMemory(&memory);
            const DWORD memory_error = memory_read ? ERROR_SUCCESS : GetLastError();
            if (FAILED(status))
            {
                std::wprintf(L"TASK4_CYCLE_STATUS policy=%hs phase=measured cycle=%llu "
                             L"hr=0x%08lx\n",
                             contract->property_page_label,
                             static_cast<unsigned long long>(cycle),
                             static_cast<unsigned long>(status));
                break;
            }
            if (!have_measured_baseline)
            {
                measured_baseline = current;
                have_measured_baseline = true;
            }
            else if (!openjoc_harness_core::Task4CycleMatchesBaseline(measured_baseline, current))
            {
                status = E_FAIL;
                break;
            }
            if (!memory_read)
            {
                status = HRESULT_FROM_WIN32(memory_error ? memory_error : ERROR_INVALID_DATA);
                break;
            }
            working_set.push_back(memory.working_set);
            private_usage.push_back(memory.private_usage);
        }
        const ULONGLONG elapsed_ms = GetTickCount64() - started;
        openjoc_harness_core::Task4TrendEvidence working_set_trend;
        openjoc_harness_core::Task4TrendEvidence private_usage_trend;
        const bool working_set_bounded =
            working_set.size() == kTask4MeasuredCycles &&
            openjoc_harness_core::WorkingSetTrendIsBounded(working_set, page_size,
                                                            &working_set_trend);
        const bool private_usage_bounded =
            private_usage.size() == kTask4MeasuredCycles &&
            openjoc_harness_core::WorkingSetTrendIsBounded(private_usage, page_size,
                                                            &private_usage_trend);
        LogTask4Trend(L"WorkingSetSize", *contract, page_size, working_set,
                      working_set_trend, working_set_bounded);
        LogTask4Trend(L"PrivateUsage", *contract, page_size, private_usage,
                      private_usage_trend, private_usage_bounded);
        if (FAILED(status) || !have_measured_baseline || !working_set_bounded ||
            !private_usage_bounded ||
            !openjoc_harness_core::Task4CycleEvidenceIsValid(measured_baseline))
        {
            status = E_FAIL;
            break;
        }
        std::wprintf(
            L"TASK4_PERFORMANCE_ROW controlled_sink=1 renderer_state=UNVERIFIED "
            L"support_inference=none policy=%hs channels=%u mask=0x%08x warmup=%llu "
            L"cycles=%llu elapsed_ms=%llu samples_per_cycle=%llu bytes_per_cycle=%llu "
            L"timestamp_count=%llu first_timestamp=%lld last_timestamp=%lld eos=%llu "
            L"allocator_requested=%ld allocator_actual=%ld GetSize_high_water=%ld "
            L"ActualDataLength_high_water=%ld checked_bytes=%llu actual_length_bytes=%llu "
            L"working_set_verdict=BOUNDED private_usage_verdict=BOUNDED\n",
            contract->property_page_label, contract->channel_count,
            contract->windows_channel_mask,
            static_cast<unsigned long long>(kTask4WarmupCycles),
            static_cast<unsigned long long>(kTask4MeasuredCycles),
            static_cast<unsigned long long>(elapsed_ms),
            static_cast<unsigned long long>(measured_baseline.samples),
            static_cast<unsigned long long>(measured_baseline.bytes),
            static_cast<unsigned long long>(measured_baseline.timestamp_count),
            static_cast<long long>(measured_baseline.first_timestamp),
            static_cast<long long>(measured_baseline.last_timestamp),
            static_cast<unsigned long long>(measured_baseline.end_of_stream_count),
            measured_baseline.allocator_requested_capacity,
            measured_baseline.allocator_actual_capacity,
            measured_baseline.sample_capacity_high_water,
            measured_baseline.sample_actual_length_high_water,
            static_cast<unsigned long long>(measured_baseline.checked_total_bytes),
            static_cast<unsigned long long>(measured_baseline.actual_length_total));
        if (policy == LAVOpenJocOutputPolicy::Layout714)
            std::wprintf(L"TASK4_ALLOCATOR_HIGH_WATER policy=7.1.4 requested=%ld actual=%ld "
                         L"GetSize=%ld ActualDataLength=%ld checked_bytes=%llu\n",
                         measured_baseline.allocator_requested_capacity,
                         measured_baseline.allocator_actual_capacity,
                         measured_baseline.sample_capacity_high_water,
                         measured_baseline.sample_actual_length_high_water,
                         static_cast<unsigned long long>(measured_baseline.checked_total_bytes));
    }
    if (SUCCEEDED(status) && !RuntimeIdentityMatches(records))
        status = E_UNEXPECTED;
    FreeModules(&dependencies);
    if (SUCCEEDED(status))
        std::wprintf(L"TASK4_CONTROL_COMPLETE controlled_sink=1 renderer_state=UNVERIFIED "
                     L"support_inference=none allocator_boundaries=PASS performance_rows=3\n");
    return status;
}

bool TestExactMediaTypeComparison()
{
    WAVEFORMATEXTENSIBLE first_format{};
    first_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    first_format.Format.nChannels = 2;
    first_format.Format.nSamplesPerSec = 48000;
    first_format.Format.wBitsPerSample = 32;
    first_format.Format.nBlockAlign = 8;
    first_format.Format.nAvgBytesPerSec = 384000;
    first_format.Format.cbSize = 22;
    first_format.Samples.wValidBitsPerSample = 32;
    first_format.dwChannelMask = 3;
    first_format.SubFormat = MEDIASUBTYPE_IEEE_FLOAT;
    WAVEFORMATEXTENSIBLE second_format = first_format;
    AM_MEDIA_TYPE left{};
    left.majortype = MEDIATYPE_Audio;
    left.subtype = MEDIASUBTYPE_IEEE_FLOAT;
    left.bFixedSizeSamples = TRUE;
    left.lSampleSize = sizeof(float) * 2;
    left.formattype = FORMAT_WaveFormatEx;
    left.cbFormat = sizeof(first_format);
    left.pbFormat = reinterpret_cast<BYTE *>(&first_format);
    AM_MEDIA_TYPE right = left;
    right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    if (!openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;

    right.majortype = GUID_NULL;
    if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;
    right = left;
    right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    right.subtype = GUID_NULL;
    if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;
    right = left;
    right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    right.bFixedSizeSamples = FALSE;
    if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;
    right = left;
    right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    right.bTemporalCompression = TRUE;
    if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;
    right = left;
    right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    ++right.lSampleSize;
    if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;
    right = left;
    right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    right.formattype = GUID_NULL;
    if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;
    right = left;
    right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    right.pUnk = reinterpret_cast<IUnknown *>(&right);
    if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;
    right = left;
    right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    --right.cbFormat;
    if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;

    AM_MEDIA_TYPE zero_left = left;
    AM_MEDIA_TYPE zero_right = right;
    zero_left.cbFormat = zero_right.cbFormat = 0;
    zero_left.pbFormat = zero_right.pbFormat = nullptr;
    if (!openjoc_harness_core::ExactMediaTypeEqual(zero_left, zero_right))
        return false;
    zero_right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    if (openjoc_harness_core::ExactMediaTypeEqual(zero_left, zero_right))
        return false;
    AM_MEDIA_TYPE null_left = left;
    AM_MEDIA_TYPE null_right = left;
    null_left.pbFormat = nullptr;
    null_right.pbFormat = nullptr;
    if (openjoc_harness_core::ExactMediaTypeEqual(null_left, null_right))
        return false;
    null_right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    if (openjoc_harness_core::ExactMediaTypeEqual(null_left, null_right))
        return false;

    const auto original_bytes = reinterpret_cast<const BYTE *>(&first_format);
    auto mutable_bytes = reinterpret_cast<BYTE *>(&second_format);
    for (std::size_t index = 0; index < sizeof(second_format); ++index)
    {
        second_format = first_format;
        mutable_bytes[index] ^= 0x01u;
        right = left;
        right.pbFormat = mutable_bytes;
        if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
            return false;
        mutable_bytes[index] = original_bytes[index];
    }
    second_format = first_format;
    right = left;
    right.pbFormat = reinterpret_cast<BYTE *>(&second_format);
    return openjoc_harness_core::ExactMediaTypeEqual(left, right);
}

bool TestPureHelpers()
{
    using namespace openjoc_harness_core;
    if (!TestTask4WorkingSetTrends() || !FingerprintsArePairwiseDistinct(
            {{0.0f, 1.0f, 2.0f}, {1.0f, 2.0f, 3.0f}, {2.0f, 3.0f, 4.0f}}) ||
        FingerprintsArePairwiseDistinct({{0.0f, 1.0f}, {0.0f, 1.0f}}) ||
        !FingerprintsArePairwiseDistinct(
            InterleavedFingerprints(std::vector<BYTE>{0, 0, 0, 0, 0, 0, 0x80, 0x3f}, 2)))
        return false;
    EvidenceInputs inputs;
    if (ClassifyControlledEvidence(inputs) != ControlledEvidenceState::Incomplete)
        return false;
    // A legal mask alone, QueryAccept alone, and a paused/running graph with
    // zero delivered samples all remain incomplete evidence.
    inputs.requested_type_exact = inputs.receive_type_exact = inputs.output_type_exact =
        inputs.input_type_exact = inputs.post_stream_type_exact = inputs.sample_types_exact =
            inputs.exact_connection = true;
    if (ClassifyControlledEvidence(inputs) != ControlledEvidenceState::Incomplete)
        return false;
    inputs.paused = inputs.running = inputs.running_sample = inputs.allocator_valid =
        inputs.timestamps_complete = inputs.end_of_stream_running = true;
    inputs.end_of_stream_count = 1;
    if (ClassifyControlledEvidence(inputs) != ControlledEvidenceState::Incomplete)
        return false;
    inputs.samples = 1;
    inputs.bytes = 8;
    if (ClassifyControlledEvidence(inputs) != ControlledEvidenceState::ControlledSinkComplete)
        return false;

    NativeProbeEvidence probe;
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;

    probe.runtime_identity = probe.fixture_identity = true;
    probe.connect_direct_status = VFW_E_TYPE_NOT_ACCEPTED;
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;
    probe.connect_attempted = true;
    if (ClassifyNativeProbe(probe) != NativeProbeState::ExactRejection)
        return false;
    probe.connect_direct_status = VFW_E_UNSUPPORTED_AUDIO;
    if (ClassifyNativeProbe(probe) != NativeProbeState::ExactRejection)
        return false;
    probe.connect_direct_status = E_ACCESSDENIED;
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;
    probe.connect_direct_status = VFW_E_CANNOT_CONNECT;
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;
    probe.connect_direct_status = VFW_E_NO_ACCEPTABLE_TYPES;
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;
    probe.connect_direct_status = HRESULT_FROM_WIN32(ERROR_NOT_READY);
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;

    probe.connect_direct_status = S_OK;
    probe.graph_setup_complete = true;
    NativeTypeAggregateEvidence type_evidence;
    type_evidence = AccumulateNativeTypeObservation(type_evidence, true, S_OK, S_OK,
                                                    true, true, true);
    type_evidence = AccumulateNativeTypeObservation(type_evidence, true, S_OK, S_OK,
                                                    true, true, true);
    probe.type_observations = type_evidence;
    probe.pause_call_status = probe.pause_state_status = probe.run_call_status =
        probe.run_state_status = probe.wait_completion_status = probe.renderer_stats_status =
            probe.diagnostics_status = probe.stop_status = S_OK;
    probe.pause_state = State_Paused;
    probe.run_state = State_Running;
    probe.completion_code = EC_COMPLETE;
    probe.renderer_last_buffer_duration = 1;
    probe.classifier_bytes = probe.stream_bytes = 1;
    probe.initial_eos_complete = probe.seek_25_complete = probe.forward_seek_complete =
        probe.backward_seek_complete = probe.reopen_complete = true;
    if (ClassifyNativeProbe(probe) != NativeProbeState::StreamObserved)
        return false;

    probe.runtime_identity = false;
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;
    probe.runtime_identity = true;
    probe.fixture_identity = false;
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;
    probe.fixture_identity = true;
    probe.reopen_complete = false;
    if (ClassifyNativeProbe(probe) != NativeProbeState::InitialStreamObserved)
        return false;
    probe.reopen_complete = true;

    probe.renderer_last_buffer_duration = 0;
    probe.midstream_renderer_stats_status = S_OK;
    probe.midstream_last_buffer_duration = 1;
    probe.seek_25_complete = probe.forward_seek_complete =
        probe.backward_seek_complete = false;
    if (ClassifyNativeProbe(probe) != NativeProbeState::InitialStreamObserved)
        return false;
    probe.renderer_last_buffer_duration = 1;
    probe.seek_25_complete = probe.forward_seek_complete =
        probe.backward_seek_complete = true;

    NativeTypeAggregateEvidence unexecuted;
    unexecuted = AccumulateNativeTypeObservation(unexecuted, false, E_FAIL, E_FAIL,
                                                false, false, false);
    if (unexecuted.observation_count != 0)
        return false;
    probe.type_observations = type_evidence;
    if (ClassifyNativeProbe(probe) != NativeProbeState::StreamObserved)
        return false;

    NativeTypeAggregateEvidence seek_mutation = type_evidence;
    seek_mutation = AccumulateNativeTypeObservation(seek_mutation, true, S_OK, S_OK,
                                                    true, false, false);
    probe.type_observations = seek_mutation;
    if (ClassifyNativeProbe(probe) != NativeProbeState::TypeMutation)
        return false;

    NativeTypeAggregateEvidence reopen_mutation = type_evidence;
    for (int index = 0; index < 4; ++index)
    {
        reopen_mutation = AccumulateNativeTypeObservation(
            reopen_mutation, true, S_OK, S_OK, true, true, true);
    }
    reopen_mutation = AccumulateNativeTypeObservation(reopen_mutation, true, S_OK, S_OK,
                                                      false, true, false);
    probe.type_observations = reopen_mutation;
    if (ClassifyNativeProbe(probe) != NativeProbeState::TypeMutation)
        return false;

    NativeTypeAggregateEvidence query_failure = type_evidence;
    query_failure = AccumulateNativeTypeObservation(query_failure, true, E_FAIL, S_OK,
                                                    false, true, false);
    probe.type_observations = query_failure;
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;
    query_failure = type_evidence;
    query_failure = AccumulateNativeTypeObservation(query_failure, true, S_OK, E_FAIL,
                                                    true, false, false);
    probe.type_observations = query_failure;
    if (ClassifyNativeProbe(probe) != NativeProbeState::Unverified)
        return false;
    NativeTypeAggregateEvidence mixed_type_evidence = seek_mutation;
    mixed_type_evidence = AccumulateNativeTypeObservation(
        mixed_type_evidence, true, E_FAIL, S_OK, false, true, false);
    probe.type_observations = mixed_type_evidence;
    if (ClassifyNativeProbe(probe) != NativeProbeState::TypeMutation)
        return false;
    probe.type_observations = type_evidence;

    NativeSeekEpochWitness epoch;
    epoch.pre_drain_status = S_OK;
    epoch.diagnostics_before_status = epoch.diagnostics_after_status = S_OK;
    epoch.classifier_bytes_before = epoch.classifier_bytes_after = 10;
    epoch.stream_bytes_before = epoch.stream_bytes_after = 20;
    epoch.position_before_run_status = epoch.position_after_completion_status = S_OK;
    epoch.position_before_run = 25;
    epoch.position_after_completion = 75;
    epoch.fresh_completion = true;
    epoch.renderer_stats_status = S_OK;
    epoch.renderer_last_buffer_duration = 1;
    epoch.renderer_discontinuities_before_status =
        epoch.renderer_discontinuities_after_status = S_OK;
    epoch.renderer_discontinuities_before = epoch.renderer_discontinuities_after = 4;
    probe.seek_25_complete = NativeSeekEpochWitnessIsComplete(epoch);
    if (probe.seek_25_complete ||
        ClassifyNativeProbe(probe) != NativeProbeState::InitialStreamObserved)
        return false;
    epoch.classifier_bytes_after = 11;
    epoch.stream_bytes_after = 21;
    if (NativeSeekEpochWitnessIsComplete(epoch))
        return false;
    epoch.renderer_discontinuities_after = 5;
    epoch.position_after_completion = epoch.position_before_run;
    if (NativeSeekEpochWitnessIsComplete(epoch))
        return false;
    epoch.position_after_completion = 75;
    epoch.prior_graph_error = true;
    if (NativeSeekEpochWitnessIsComplete(epoch))
        return false;
    epoch.prior_graph_error = false;
    probe.seek_25_complete = probe.forward_seek_complete = probe.backward_seek_complete =
        NativeSeekEpochWitnessIsComplete(epoch);
    return probe.seek_25_complete &&
           ClassifyNativeProbe(probe) == NativeProbeState::StreamObserved;
}

bool TestStrictCaptureSinkPolicy()
{
    const LAVOpenJocOutputContract *contract =
        FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout714);
    if (!contract || BindRendererMoniker(nullptr, nullptr) != E_INVALIDARG)
        return false;
    const CMediaType target = BuildStrictTarget(*contract);
    const CMediaType fallback = BuildPcmType(8, 0x0000063fu, true);
    HRESULT status = S_OK;
    std::unique_ptr<StrictCaptureSink> positive(
        new (std::nothrow)
            StrictCaptureSink(target, false, std::vector<CMediaType>{}, &status));
    if (target.pUnk != nullptr || !positive || FAILED(status) ||
        positive->input()->QueryAccept(&target) != S_OK ||
        positive->input()->QueryAccept(&fallback) != S_FALSE)
        return false;
    status = S_OK;
    std::unique_ptr<StrictCaptureSink> rejection(
        new (std::nothrow)
            StrictCaptureSink(target, true, std::vector<CMediaType>{fallback}, &status));
    return rejection && SUCCEEDED(status) && rejection->Run(0) == S_OK &&
           rejection->input()->QueryAccept(&target) == S_FALSE &&
           rejection->input()->QueryAccept(&fallback) == S_OK &&
           rejection->rejected_stage() == L"QueryAccept" &&
           rejection->rejected_normalized_result() == VFW_E_TYPE_NOT_ACCEPTED;
}

struct NativeTypeObservation
{
    HRESULT output_status = E_UNEXPECTED;
    HRESULT renderer_input_status = E_UNEXPECTED;
    CMediaType output_type;
    CMediaType renderer_input_type;
    bool output_exact = false;
    bool renderer_input_exact = false;
    bool peer_equal = false;
};

NativeTypeObservation ObserveNativeConnectionTypes(IPin *audio_output, IPin *renderer_input,
                                                   const CMediaType &requested)
{
    NativeTypeObservation result;
    if (!audio_output || !renderer_input)
        return result;
    result.output_status = audio_output->ConnectionMediaType(&result.output_type);
    result.renderer_input_status =
        renderer_input->ConnectionMediaType(&result.renderer_input_type);
    if (result.output_status == S_OK)
        result.output_exact =
            openjoc_harness_core::ExactMediaTypeEqual(requested, result.output_type);
    if (result.renderer_input_status == S_OK)
        result.renderer_input_exact =
            openjoc_harness_core::ExactMediaTypeEqual(requested, result.renderer_input_type);
    if (result.output_status == S_OK && result.renderer_input_status == S_OK)
        result.peer_equal = openjoc_harness_core::ExactMediaTypeEqual(
            result.output_type, result.renderer_input_type);
    return result;
}

bool NativeTypeObservationIsExact(const NativeTypeObservation &value)
{
    return value.output_status == S_OK && value.renderer_input_status == S_OK &&
           value.output_exact && value.renderer_input_exact && value.peer_equal;
}

openjoc_harness_core::NativeTypeAggregateEvidence AccumulateObservedNativeType(
    openjoc_harness_core::NativeTypeAggregateEvidence aggregate,
    const bool executed, const NativeTypeObservation &observation)
{
    return openjoc_harness_core::AccumulateNativeTypeObservation(
        aggregate, executed, observation.output_status,
        observation.renderer_input_status, observation.output_exact,
        observation.renderer_input_exact, observation.peer_equal);
}

struct NativeRendererGraph
{
    ComOwner<IGraphBuilder> graph;
    ComOwner<IBaseFilter> source;
    ComOwner<IBaseFilter> audio_filter;
    ComOwner<IPin> source_output;
    ComOwner<IPin> audio_input;
    ComOwner<IPin> audio_output;
    ComOwner<IBaseFilter> renderer;
    ComOwner<IPin> renderer_input;
    ComOwner<IMediaControl> control;
    ComOwner<IMediaEvent> events;
    ComOwner<IMediaSeeking> seeking;
    ComOwner<ILAVOpenJocDiagnostics> diagnostics;
    CMediaType exact_eac3;
    HRESULT create_graph_status = E_UNEXPECTED;
    HRESULT bind_renderer_status = E_UNEXPECTED;
    HRESULT add_renderer_status = E_UNEXPECTED;
    HRESULT find_renderer_pin_status = E_UNEXPECTED;
    bool connect_attempted = false;
    HRESULT connect_status = E_UNEXPECTED;
    HRESULT control_status = E_UNEXPECTED;
    HRESULT events_status = E_UNEXPECTED;
    HRESULT seeking_status = E_UNEXPECTED;
    HRESULT diagnostics_interface_status = E_UNEXPECTED;
    bool graph_exactly_three_filters = false;
};

struct NativeGraphSetupEvidence
{
    HRESULT create_graph_status = E_UNEXPECTED;
    HRESULT bind_renderer_status = E_UNEXPECTED;
    HRESULT add_renderer_status = E_UNEXPECTED;
    HRESULT find_renderer_pin_status = E_UNEXPECTED;
    bool connect_attempted = false;
    HRESULT connect_status = E_UNEXPECTED;
    HRESULT control_status = E_UNEXPECTED;
    HRESULT events_status = E_UNEXPECTED;
    HRESULT seeking_status = E_UNEXPECTED;
    HRESULT diagnostics_interface_status = E_UNEXPECTED;
    bool graph_exactly_three_filters = false;
};

NativeGraphSetupEvidence SnapshotNativeGraphSetup(const NativeRendererGraph &value)
{
    return {value.create_graph_status,
            value.bind_renderer_status,
            value.add_renderer_status,
            value.find_renderer_pin_status,
            value.connect_attempted,
            value.connect_status,
            value.control_status,
            value.events_status,
            value.seeking_status,
            value.diagnostics_interface_status,
            value.graph_exactly_three_filters};
}

void BuildNativeRendererGraph(const PrivateComModule &audio,
                              const PrivateComModule &splitter,
                              const FixtureIdentity &fixture,
                              const LAVOpenJocOutputPolicy policy,
                              const wchar_t *renderer_moniker,
                              const CMediaType &requested,
                              NativeRendererGraph *result)
{
    if (!result)
        return;
    result->create_graph_status = CreateGraphForFixture(
        audio, splitter, fixture.final_path, policy, true, result->graph.put(),
        result->source.put(), result->audio_filter.put(), result->source_output.put(),
        result->audio_input.put(), result->audio_output.put(), &result->exact_eac3);
    if (SUCCEEDED(result->create_graph_status))
        result->bind_renderer_status =
            BindRendererMoniker(renderer_moniker, result->renderer.put());
    if (SUCCEEDED(result->bind_renderer_status))
        result->add_renderer_status =
            result->graph->AddFilter(result->renderer.get(), L"Named renderer under exact test");
    if (SUCCEEDED(result->add_renderer_status))
        result->find_renderer_pin_status =
            FindSingleOwnedPin(result->renderer.get(), PINDIR_INPUT,
                               result->renderer_input.put());
    if (SUCCEEDED(result->find_renderer_pin_status))
    {
        result->connect_attempted = true;
        result->connect_status = result->graph->ConnectDirect(
            result->audio_output.get(), result->renderer_input.get(), &requested);
    }
    if (result->connect_status == S_OK)
    {
        result->graph_exactly_three_filters = GraphContainsExactly(result->graph.get(), 3);
        result->control_status = result->graph->QueryInterface(
            IID_IMediaControl, reinterpret_cast<void **>(result->control.put()));
        result->events_status = result->graph->QueryInterface(
            IID_IMediaEvent, reinterpret_cast<void **>(result->events.put()));
        result->seeking_status = result->graph->QueryInterface(
            IID_IMediaSeeking, reinterpret_cast<void **>(result->seeking.put()));
        result->diagnostics_interface_status = result->audio_filter->QueryInterface(
            __uuidof(ILAVOpenJocDiagnostics),
            reinterpret_cast<void **>(result->diagnostics.put()));
    }
}

struct NativePlaybackEvidence
{
    NativeTypeObservation pre_types;
    bool stream_attempted = false;
    HRESULT pause_call_status = E_UNEXPECTED;
    HRESULT pause_state_status = E_UNEXPECTED;
    OAFilterState pause_state = State_Stopped;
    HRESULT run_call_status = E_UNEXPECTED;
    HRESULT run_state_status = E_UNEXPECTED;
    OAFilterState run_state = State_Stopped;
    HRESULT wait_status = E_UNEXPECTED;
    long completion_code = 0;
    HRESULT graph_error = S_OK;
    HRESULT midstream_wait_status = E_UNEXPECTED;
    HRESULT midstream_renderer_stats_status = E_NOINTERFACE;
    DWORD midstream_last_buffer_duration = 0;
    DWORD midstream_last_buffer_aux = 0;
    HRESULT renderer_stats_status = E_NOINTERFACE;
    DWORD last_buffer_duration = 0;
    DWORD last_buffer_aux = 0;
    HRESULT diagnostics_status = E_NOINTERFACE;
    ULONGLONG classifier_bytes = 0;
    ULONGLONG stream_bytes = 0;
    NativeTypeObservation post_types;
    bool eos_complete = false;
};

void CaptureNativeRendererStats(IBaseFilter *renderer, HRESULT *status,
                                DWORD *last_buffer_duration, DWORD *last_buffer_aux)
{
    if (!status || !last_buffer_duration || !last_buffer_aux)
        return;
    ComOwner<IAMAudioRendererStats> renderer_stats;
    const HRESULT interface_status = renderer
                                         ? renderer->QueryInterface(
                                               IID_IAMAudioRendererStats,
                                               reinterpret_cast<void **>(renderer_stats.put()))
                                         : E_POINTER;
    *status = interface_status == S_OK
                  ? renderer_stats->GetStatParam(AM_AUDREND_STAT_PARAM_LAST_BUFFER_DUR,
                                                 last_buffer_duration, last_buffer_aux)
                  : interface_status;
}

void CaptureNativeRendererWitnesses(IBaseFilter *renderer,
                                    ILAVOpenJocDiagnostics *diagnostics,
                                    NativePlaybackEvidence *result)
{
    if (!result)
        return;
    CaptureNativeRendererStats(renderer, &result->renderer_stats_status,
                               &result->last_buffer_duration, &result->last_buffer_aux);
    if (diagnostics)
    {
        result->diagnostics_status = diagnostics->GetOpenJocInputByteCounts(
            &result->classifier_bytes, &result->stream_bytes);
    }
}

void RunNativeInitialPlayback(NativeRendererGraph *native,
                              const CMediaType &requested,
                              NativePlaybackEvidence *result)
{
    if (!native || !result || native->connect_status != S_OK)
        return;
    result->pre_types = ObserveNativeConnectionTypes(
        native->audio_output.get(), native->renderer_input.get(), requested);
    const bool ready = NativeTypeObservationIsExact(result->pre_types) &&
                       native->graph_exactly_three_filters &&
                       native->control_status == S_OK && native->events_status == S_OK;
    if (ready)
    {
        result->stream_attempted = true;
        result->pause_call_status = native->control->Pause();
        if (SUCCEEDED(result->pause_call_status))
            result->pause_state_status =
                native->control->GetState(10000, &result->pause_state);
        if (SUCCEEDED(result->pause_call_status) && result->pause_state_status == S_OK &&
            result->pause_state == State_Paused)
        {
            result->run_call_status = native->control->Run();
            if (SUCCEEDED(result->run_call_status))
                result->run_state_status =
                    native->control->GetState(10000, &result->run_state);
        }
        if (SUCCEEDED(result->run_call_status) && result->run_state_status == S_OK &&
            result->run_state == State_Running)
        {
            long midstream_completion_code = 0;
            result->midstream_wait_status =
                native->events->WaitForCompletion(100, &midstream_completion_code);
            if (result->midstream_wait_status == E_ABORT)
            {
                CaptureNativeRendererStats(
                    native->renderer.get(), &result->midstream_renderer_stats_status,
                    &result->midstream_last_buffer_duration,
                    &result->midstream_last_buffer_aux);
                result->wait_status =
                    native->events->WaitForCompletion(60000, &result->completion_code);
            }
            else
            {
                result->wait_status = result->midstream_wait_status;
                result->completion_code = midstream_completion_code;
            }
        }
        if (DrainGraphErrors(native->events.get(), &result->graph_error) &&
            result->graph_error == S_OK)
            result->graph_error = E_FAIL;
    }
    CaptureNativeRendererWitnesses(native->renderer.get(), native->diagnostics.get(), result);
    result->post_types = ObserveNativeConnectionTypes(
        native->audio_output.get(), native->renderer_input.get(), requested);
    result->eos_complete = result->stream_attempted && SUCCEEDED(result->pause_call_status) &&
                           result->pause_state_status == S_OK &&
                           result->pause_state == State_Paused &&
                           SUCCEEDED(result->run_call_status) &&
                           result->run_state_status == S_OK &&
                           result->run_state == State_Running && result->wait_status == S_OK &&
                           result->completion_code == EC_COMPLETE && result->graph_error == S_OK;
}

struct NativeSeekEvidence
{
    const char *label = nullptr;
    LONGLONG requested_position = 0;
    LONGLONG actual_position = 0;
    HRESULT get_capabilities_status = E_UNEXPECTED;
    DWORD capabilities = 0;
    HRESULT check_capabilities_status = E_UNEXPECTED;
    DWORD checked_capabilities = 0;
    NativeEventDrainEvidence pre_drain;
    bool set_positions_attempted = false;
    HRESULT diagnostics_before_status = E_UNEXPECTED;
    ULONGLONG classifier_bytes_before = 0;
    ULONGLONG stream_bytes_before = 0;
    HRESULT renderer_stats_interface_status = E_NOINTERFACE;
    HRESULT renderer_discontinuities_before_status = E_UNEXPECTED;
    DWORD renderer_discontinuities_before = 0;
    DWORD renderer_discontinuities_before_aux = 0;
    HRESULT set_positions_status = E_UNEXPECTED;
    HRESULT position_before_run_status = E_UNEXPECTED;
    LONGLONG position_before_run = 0;
    HRESULT run_call_status = E_UNEXPECTED;
    HRESULT run_state_status = E_UNEXPECTED;
    OAFilterState run_state = State_Stopped;
    HRESULT wait_status = E_UNEXPECTED;
    long completion_code = 0;
    HRESULT position_after_completion_status = E_UNEXPECTED;
    LONGLONG position_after_completion = 0;
    HRESULT renderer_discontinuities_after_status = E_UNEXPECTED;
    DWORD renderer_discontinuities_after = 0;
    DWORD renderer_discontinuities_after_aux = 0;
    HRESULT graph_error = S_OK;
    NativePlaybackEvidence witnesses;
    NativeTypeObservation post_types;
    bool complete = false;
};

void RunNativeSeekEpoch(NativeRendererGraph *native, const CMediaType &requested,
                        const char *label, const LONGLONG position,
                        NativeSeekEvidence *result)
{
    if (!native || !result || !label)
        return;
    result->label = label;
    result->requested_position = position;
    result->actual_position = position;
    if (native->seeking_status != S_OK || !native->seeking || !native->control ||
        !native->events)
        return;
    ComOwner<IAMAudioRendererStats> renderer_stats;
    if (native->renderer)
    {
        result->renderer_stats_interface_status = native->renderer->QueryInterface(
            IID_IAMAudioRendererStats,
            reinterpret_cast<void **>(renderer_stats.put()));
    }
    result->get_capabilities_status =
        native->seeking->GetCapabilities(&result->capabilities);
    result->checked_capabilities =
        AM_SEEKING_CanSeekAbsolute | AM_SEEKING_CanGetDuration;
    result->check_capabilities_status =
        native->seeking->CheckCapabilities(&result->checked_capabilities);
    const DWORD required_capabilities =
        AM_SEEKING_CanSeekAbsolute | AM_SEEKING_CanGetDuration;
    if (result->get_capabilities_status == S_OK &&
        (result->capabilities & required_capabilities) == required_capabilities &&
        result->check_capabilities_status == S_OK &&
        result->checked_capabilities == required_capabilities)
    {
        result->pre_drain = DrainNativeGraphEvents(native->events.get());
        if (result->pre_drain.status == S_OK && native->diagnostics)
        {
            result->diagnostics_before_status =
                native->diagnostics->GetOpenJocInputByteCounts(
                    &result->classifier_bytes_before, &result->stream_bytes_before);
            if (result->diagnostics_before_status == S_OK)
            {
                result->renderer_discontinuities_before_status = renderer_stats
                    ? renderer_stats->GetStatParam(
                          AM_AUDREND_STAT_PARAM_DISCONTINUITIES,
                          &result->renderer_discontinuities_before,
                          &result->renderer_discontinuities_before_aux)
                    : result->renderer_stats_interface_status;
                result->set_positions_attempted = true;
                result->set_positions_status = native->seeking->SetPositions(
                    &result->actual_position, AM_SEEKING_AbsolutePositioning, nullptr,
                    AM_SEEKING_NoPositioning);
            }
        }
    }
    if (result->set_positions_status == S_OK &&
        result->actual_position == result->requested_position)
    {
        result->position_before_run_status =
            native->seeking->GetCurrentPosition(&result->position_before_run);
        if (result->position_before_run_status == S_OK)
        {
            result->run_call_status = native->control->Run();
            if (SUCCEEDED(result->run_call_status))
                result->run_state_status =
                    native->control->GetState(10000, &result->run_state);
            if (result->run_state_status == S_OK && result->run_state == State_Running)
                result->wait_status =
                    native->events->WaitForCompletion(60000, &result->completion_code);
            if (result->wait_status == S_OK && result->completion_code == EC_COMPLETE)
            {
                result->position_after_completion_status =
                    native->seeking->GetCurrentPosition(
                        &result->position_after_completion);
                result->renderer_discontinuities_after_status = renderer_stats
                    ? renderer_stats->GetStatParam(
                          AM_AUDREND_STAT_PARAM_DISCONTINUITIES,
                          &result->renderer_discontinuities_after,
                          &result->renderer_discontinuities_after_aux)
                    : result->renderer_stats_interface_status;
            }
        }
        if (DrainGraphErrors(native->events.get(), &result->graph_error) &&
            result->graph_error == S_OK)
            result->graph_error = E_FAIL;
    }
    CaptureNativeRendererWitnesses(native->renderer.get(), native->diagnostics.get(),
                                   &result->witnesses);
    result->post_types = ObserveNativeConnectionTypes(
        native->audio_output.get(), native->renderer_input.get(), requested);
    openjoc_harness_core::NativeSeekEpochWitness epoch;
    epoch.pre_drain_status = result->pre_drain.status;
    epoch.prior_graph_error = result->pre_drain.graph_error;
    epoch.diagnostics_before_status = result->diagnostics_before_status;
    epoch.classifier_bytes_before = result->classifier_bytes_before;
    epoch.stream_bytes_before = result->stream_bytes_before;
    epoch.diagnostics_after_status = result->witnesses.diagnostics_status;
    epoch.classifier_bytes_after = result->witnesses.classifier_bytes;
    epoch.stream_bytes_after = result->witnesses.stream_bytes;
    epoch.position_before_run_status = result->position_before_run_status;
    epoch.position_before_run = result->position_before_run;
    epoch.position_after_completion_status = result->position_after_completion_status;
    epoch.position_after_completion = result->position_after_completion;
    epoch.fresh_completion =
        result->wait_status == S_OK && result->completion_code == EC_COMPLETE;
    epoch.renderer_stats_status = result->witnesses.renderer_stats_status;
    epoch.renderer_last_buffer_duration = result->witnesses.last_buffer_duration;
    epoch.renderer_discontinuities_before_status =
        result->renderer_discontinuities_before_status;
    epoch.renderer_discontinuities_before = result->renderer_discontinuities_before;
    epoch.renderer_discontinuities_after_status =
        result->renderer_discontinuities_after_status;
    epoch.renderer_discontinuities_after = result->renderer_discontinuities_after;
    result->complete = result->get_capabilities_status == S_OK &&
                       (result->capabilities & required_capabilities) == required_capabilities &&
                        result->check_capabilities_status == S_OK &&
                        result->checked_capabilities == required_capabilities &&
                        result->set_positions_attempted &&
                        result->set_positions_status == S_OK &&
                       result->actual_position == result->requested_position &&
                       SUCCEEDED(result->run_call_status) && result->run_state_status == S_OK &&
                        result->run_state == State_Running && result->wait_status == S_OK &&
                        result->completion_code == EC_COMPLETE && result->graph_error == S_OK &&
                        openjoc_harness_core::NativeSeekEpochWitnessIsComplete(epoch) &&
                        NativeTypeObservationIsExact(result->post_types);
}

void AppendNativeTypeObservation(std::ostringstream &record, const char *label,
                                 const NativeTypeObservation &value)
{
    record << "type_observation\t" << label << "\toutput_hr=0x" << std::hex
           << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(value.output_status)
           << "\trenderer_input_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(value.renderer_input_status)
           << "\toutput_exact=" << std::dec << (value.output_exact ? 1 : 0)
           << "\trenderer_input_exact=" << (value.renderer_input_exact ? 1 : 0)
           << "\tpeer_equal=" << (value.peer_equal ? 1 : 0)
           << "\toutput_type=" << SerializeMediaType(value.output_type)
           << "\trenderer_input_type=" << SerializeMediaType(value.renderer_input_type)
           << "\n";
}

void AppendNativeSeekEvidence(std::ostringstream &record, const std::size_t sequence,
                               const NativeSeekEvidence &value)
{
    const ULONGLONG classifier_delta =
        value.witnesses.classifier_bytes > value.classifier_bytes_before
            ? value.witnesses.classifier_bytes - value.classifier_bytes_before
            : 0;
    const ULONGLONG stream_delta =
        value.witnesses.stream_bytes > value.stream_bytes_before
            ? value.witnesses.stream_bytes - value.stream_bytes_before
            : 0;
    const DWORD renderer_discontinuities_delta =
        value.renderer_discontinuities_after > value.renderer_discontinuities_before
            ? value.renderer_discontinuities_after - value.renderer_discontinuities_before
            : 0;
    record << "operation\t" << sequence << "\t" << (value.label ? value.label : "<none>")
           << "\tget_capabilities_hr=0x" << std::hex << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(value.get_capabilities_status)
           << "\tcapabilities=0x" << std::setw(8) << value.capabilities
           << "\tcheck_capabilities_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(value.check_capabilities_status)
           << "\tchecked=0x" << std::setw(8) << value.checked_capabilities
           << "\tpre_drain_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(value.pre_drain.status)
           << "\tpre_drain_terminal_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(value.pre_drain.terminal_status)
           << "\tpre_drain_free_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(value.pre_drain.free_status)
           << "\tpre_drain_event_count=" << std::dec << value.pre_drain.event_count
           << "\tpre_drain_completion_count=" << value.pre_drain.completion_count
           << "\tpre_drain_graph_error=" << (value.pre_drain.graph_error ? 1 : 0)
           << "\tpre_drain_first_error_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(value.pre_drain.first_error)
           << "\tdiagnostics_before_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(value.diagnostics_before_status)
           << "\tclassifier_bytes_before=" << std::dec << value.classifier_bytes_before
           << "\tstream_bytes_before=" << value.stream_bytes_before
           << "\trenderer_stats_qi_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(value.renderer_stats_interface_status)
           << "\trenderer_discontinuities_before_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(
                  value.renderer_discontinuities_before_status)
           << "\trenderer_discontinuities_before=" << std::dec
           << value.renderer_discontinuities_before
           << "\trenderer_discontinuities_before_aux="
           << value.renderer_discontinuities_before_aux
           << "\tset_positions_attempted=" << (value.set_positions_attempted ? 1 : 0)
           << "\tset_positions_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(value.set_positions_status)
           << "\trequested=" << std::dec << value.requested_position
           << "\tactual=" << value.actual_position
           << "\tposition_before_run_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(value.position_before_run_status)
           << "\tposition_before_run=" << std::dec << value.position_before_run
           << "\trun_hr=0x" << std::hex
           << std::setw(8) << static_cast<std::uint32_t>(value.run_call_status)
           << "\trun_state_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(value.run_state_status)
           << "\trun_state=" << std::dec << static_cast<int>(value.run_state)
           << "\twait_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(value.wait_status)
           << "\tcompletion_code=" << std::dec << value.completion_code
           << "\tposition_after_completion_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(value.position_after_completion_status)
           << "\tposition_after_completion=" << std::dec
           << value.position_after_completion
           << "\trenderer_discontinuities_after_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(
                  value.renderer_discontinuities_after_status)
           << "\trenderer_discontinuities_after=" << std::dec
           << value.renderer_discontinuities_after
           << "\trenderer_discontinuities_after_aux="
           << value.renderer_discontinuities_after_aux
           << "\trenderer_discontinuities_delta="
           << renderer_discontinuities_delta
           << "\tgraph_error_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(value.graph_error)
           << "\trenderer_stats_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(value.witnesses.renderer_stats_status)
           << "\tlast_buffer_duration=" << std::dec
           << value.witnesses.last_buffer_duration
           << "\tdiagnostics_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(value.witnesses.diagnostics_status)
           << "\tclassifier_bytes_after=" << std::dec << value.witnesses.classifier_bytes
           << "\tclassifier_bytes_delta=" << classifier_delta
           << "\tstream_bytes_after=" << value.witnesses.stream_bytes
           << "\tstream_bytes_delta=" << stream_delta
           << "\tcomplete=" << (value.complete ? 1 : 0) << "\n";
    AppendNativeTypeObservation(record, value.label ? value.label : "<none>",
                                value.post_types);
}

const char *NativeProbeStateText(const openjoc_harness_core::NativeProbeState state)
{
    using openjoc_harness_core::NativeProbeState;
    switch (state)
    {
    case NativeProbeState::ExactRejection:
        return "EXACT_REJECTION";
    case NativeProbeState::TypeMutation:
        return "TYPE_MUTATION";
    case NativeProbeState::InitialStreamObserved:
        return "INITIAL_STREAM_OBSERVED";
    case NativeProbeState::StreamObserved:
        return "STREAM_OBSERVED";
    default:
        return "UNVERIFIED";
    }
}

struct AudioRendererInventoryRow
{
    std::wstring moniker;
    std::wstring friendly_name;
    GUID filter_class_id{};
    HRESULT property_bag_status = E_UNEXPECTED;
    HRESULT friendly_name_status = E_UNEXPECTED;
    HRESULT bind_filter_status = E_UNEXPECTED;
    HRESULT class_id_status = E_UNEXPECTED;
};

std::string TsvField(const std::wstring &value)
{
    std::wstring sanitized = value;
    for (wchar_t &character : sanitized)
    {
        if (character == L'\t' || character == L'\r' || character == L'\n')
            character = L' ';
    }
    return WideToUtf8(sanitized);
}

std::string InventoryGuidText(const GUID &value)
{
    wchar_t buffer[40]{};
    return StringFromGUID2(value, buffer, static_cast<int>(std::size(buffer))) > 0
               ? WideToUtf8(buffer)
               : std::string{};
}

HRESULT WriteAudioRendererInventory(const std::filesystem::path &evidence_path)
{
    if (!evidence_path.is_absolute() || !std::filesystem::is_directory(evidence_path.parent_path()))
        return E_INVALIDARG;

    ComOwner<ICreateDevEnum> device_enumerator;
    HRESULT status = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ICreateDevEnum,
                                      reinterpret_cast<void **>(device_enumerator.put()));
    if (FAILED(status))
        return status;
    ComOwner<IEnumMoniker> monikers;
    status = device_enumerator->CreateClassEnumerator(CLSID_AudioRendererCategory,
                                                       monikers.put(), 0);
    if (status != S_OK && status != S_FALSE)
        return status;

    std::vector<AudioRendererInventoryRow> rows;
    if (status == S_OK)
    {
        ComOwner<IBindCtx> bind_context;
        status = CreateBindCtx(0, bind_context.put());
        if (FAILED(status))
            return status;
        for (;;)
        {
            ComOwner<IMoniker> moniker;
            ULONG fetched = 0;
            if (monikers->Next(1, moniker.put(), &fetched) != S_OK)
                break;
            AudioRendererInventoryRow row;
            LPOLESTR display_name = nullptr;
            if (moniker->GetDisplayName(bind_context.get(), nullptr, &display_name) == S_OK &&
                display_name)
            {
                row.moniker = display_name;
                CoTaskMemFree(display_name);
            }

            ComOwner<IPropertyBag> property_bag;
            row.property_bag_status = moniker->BindToStorage(
                bind_context.get(), nullptr, IID_IPropertyBag,
                reinterpret_cast<void **>(property_bag.put()));
            if (row.property_bag_status == S_OK)
            {
                VARIANT friendly_name;
                VariantInit(&friendly_name);
                row.friendly_name_status =
                    property_bag->Read(L"FriendlyName", &friendly_name, nullptr);
                if (row.friendly_name_status == S_OK && friendly_name.vt == VT_BSTR &&
                    friendly_name.bstrVal)
                    row.friendly_name = friendly_name.bstrVal;
                VariantClear(&friendly_name);
            }

            ComOwner<IBaseFilter> filter;
            row.bind_filter_status = moniker->BindToObject(
                bind_context.get(), nullptr, IID_IBaseFilter,
                reinterpret_cast<void **>(filter.put()));
            if (row.bind_filter_status == S_OK)
                row.class_id_status = filter->GetClassID(&row.filter_class_id);
            rows.push_back(std::move(row));
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
        return left.moniker < right.moniker;
    });

    std::ostringstream evidence;
    evidence << "RENDERER_INVENTORY_V1\n"
             << "count\t" << rows.size() << "\n";
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        const auto &row = rows[index];
        evidence << "renderer\t" << index
                 << "\tmoniker=" << TsvField(row.moniker)
                 << "\tfriendly_name=" << TsvField(row.friendly_name)
                 << "\tproperty_bag_hr=0x" << std::hex << std::setw(8)
                 << std::setfill('0') << static_cast<std::uint32_t>(row.property_bag_status)
                 << "\tfriendly_name_hr=0x" << std::setw(8)
                 << static_cast<std::uint32_t>(row.friendly_name_status)
                 << "\tbind_filter_hr=0x" << std::setw(8)
                 << static_cast<std::uint32_t>(row.bind_filter_status)
                 << "\tclass_id_hr=0x" << std::setw(8)
                 << static_cast<std::uint32_t>(row.class_id_status)
                 << "\tfilter_clsid=" << InventoryGuidText(row.filter_class_id) << "\n";
    }

    HANDLE output = CreateFileW(evidence_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    const std::string text = evidence.str();
    const bool wrote = WriteAll(output, text) && FlushFileBuffers(output);
    CloseHandle(output);
    if (!wrote)
    {
        DeleteFileW(evidence_path.c_str());
        return E_FAIL;
    }
    std::wprintf(L"RENDERER_INVENTORY_COMPLETE count=%llu evidence=\"%ls\"\n",
                 static_cast<unsigned long long>(rows.size()), evidence_path.c_str());
    return S_OK;
}

constexpr PROPERTYKEY kEndpointFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr PROPERTYKEY kAudioEngineDeviceFormat = {
    {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

std::string SerializeWaveFormat(
    const WAVEFORMATEX *format,
    const std::size_t available_bytes = (std::numeric_limits<std::size_t>::max)())
{
    if (!format || available_bytes < sizeof(WAVEFORMATEX) ||
        format->cbSize > available_bytes - sizeof(WAVEFORMATEX))
        return {};
    const std::size_t format_size = sizeof(WAVEFORMATEX) + format->cbSize;
    DWORD mask = 0;
    WORD valid_bits = 0;
    GUID subformat{};
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format_size >= sizeof(WAVEFORMATEXTENSIBLE))
    {
        const auto *extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        mask = extended->dwChannelMask;
        valid_bits = extended->Samples.wValidBitsPerSample;
        subformat = extended->SubFormat;
    }
    std::ostringstream result;
    result << "tag=0x" << std::hex << std::setw(4) << std::setfill('0')
           << format->wFormatTag << " channels=" << std::dec << format->nChannels
           << " rate=" << format->nSamplesPerSec
           << " avg_bytes_per_sec=" << format->nAvgBytesPerSec
           << " block_align=" << format->nBlockAlign
           << " bits=" << format->wBitsPerSample
           << " cb_size=" << format->cbSize
           << " valid_bits=" << valid_bits
           << " mask=0x" << std::hex << std::setw(8) << mask
           << " subformat=" << GuidText(subformat)
           << " bytes=" << BytesHex(reinterpret_cast<const BYTE *>(format), format_size);
    return result.str();
}

HRESULT WriteAudioEndpointCapabilities(const wchar_t *endpoint_id,
                                       const std::filesystem::path &evidence_path)
{
    if (!endpoint_id || !*endpoint_id || !evidence_path.is_absolute() ||
        !std::filesystem::is_directory(evidence_path.parent_path()))
        return E_INVALIDARG;

    ComOwner<IMMDeviceEnumerator> enumerator;
    HRESULT status = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(enumerator.put()));
    if (FAILED(status))
        return status;
    ComOwner<IMMDevice> endpoint;
    status = enumerator->GetDevice(endpoint_id, endpoint.put());
    if (FAILED(status))
        return status;
    LPWSTR observed_id = nullptr;
    status = endpoint->GetId(&observed_id);
    if (FAILED(status) || !observed_id)
        return FAILED(status) ? status : E_UNEXPECTED;
    const std::wstring endpoint_identity = observed_id;
    CoTaskMemFree(observed_id);

    DWORD endpoint_state = 0;
    const HRESULT state_status = endpoint->GetState(&endpoint_state);
    ComOwner<IPropertyStore> properties;
    const HRESULT property_store_status = endpoint->OpenPropertyStore(STGM_READ, properties.put());
    PROPVARIANT friendly_name;
    PROPVARIANT device_format;
    PropVariantInit(&friendly_name);
    PropVariantInit(&device_format);
    const HRESULT friendly_name_status =
        property_store_status == S_OK
            ? properties->GetValue(kEndpointFriendlyName, &friendly_name)
            : property_store_status;
    const HRESULT device_format_status =
        property_store_status == S_OK
            ? properties->GetValue(kAudioEngineDeviceFormat, &device_format)
            : property_store_status;

    ComOwner<IAudioClient> audio_client;
    const HRESULT audio_client_status = endpoint->Activate(
        __uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
        reinterpret_cast<void **>(audio_client.put()));
    WAVEFORMATEX *mix_format = nullptr;
    const HRESULT mix_format_status = audio_client_status == S_OK
                                          ? audio_client->GetMixFormat(&mix_format)
                                          : audio_client_status;

    std::ostringstream evidence;
    evidence << "AUDIO_ENDPOINT_CAPABILITIES_V1\n"
             << "endpoint_id\t" << TsvField(endpoint_identity) << "\n"
             << "state_hr\t0x" << std::hex << std::setw(8) << std::setfill('0')
             << static_cast<std::uint32_t>(state_status) << "\n"
             << "state\t0x" << std::setw(8) << endpoint_state << "\n"
             << "property_store_hr\t0x" << std::setw(8)
             << static_cast<std::uint32_t>(property_store_status) << "\n"
             << "friendly_name_hr\t0x" << std::setw(8)
             << static_cast<std::uint32_t>(friendly_name_status) << "\n"
             << "friendly_name\t"
             << (friendly_name_status == S_OK && friendly_name.vt == VT_LPWSTR &&
                         friendly_name.pwszVal
                     ? TsvField(friendly_name.pwszVal)
                     : std::string{})
             << "\n"
             << "device_format_hr\t0x" << std::setw(8)
             << static_cast<std::uint32_t>(device_format_status) << "\n"
             << "device_format\t"
             << (device_format_status == S_OK && device_format.vt == VT_BLOB &&
                         device_format.blob.cbSize >= sizeof(WAVEFORMATEX)
                      ? SerializeWaveFormat(
                            reinterpret_cast<const WAVEFORMATEX *>(
                                device_format.blob.pBlobData),
                            device_format.blob.cbSize)
                     : std::string{})
             << "\n"
             << "audio_client_hr\t0x" << std::setw(8)
             << static_cast<std::uint32_t>(audio_client_status) << "\n"
             << "mix_format_hr\t0x" << std::setw(8)
             << static_cast<std::uint32_t>(mix_format_status) << "\n"
             << "mix_format\t"
             << (mix_format_status == S_OK ? SerializeWaveFormat(mix_format) : std::string{})
             << "\n";

    constexpr std::array<LAVOpenJocOutputPolicy, LAV_OPENJOC_OUTPUT_CONTRACT_COUNT> policies = {
        LAVOpenJocOutputPolicy::Stereo,   LAVOpenJocOutputPolicy::Binaural,
        LAVOpenJocOutputPolicy::Layout51,
        LAVOpenJocOutputPolicy::Layout71, LAVOpenJocOutputPolicy::Layout512,
        LAVOpenJocOutputPolicy::Layout514, LAVOpenJocOutputPolicy::Layout712,
        LAVOpenJocOutputPolicy::Layout714};
    if (audio_client_status == S_OK)
    {
        for (const auto policy : policies)
        {
            const auto *contract = FindLAVOpenJocOutputContract(policy);
            const CMediaType requested = BuildStrictTarget(*contract);
            const auto *wave = reinterpret_cast<const WAVEFORMATEX *>(requested.pbFormat);
            WAVEFORMATEX *closest = nullptr;
            const HRESULT shared_status = audio_client->IsFormatSupported(
                AUDCLNT_SHAREMODE_SHARED, wave, &closest);
            const std::string closest_text = SerializeWaveFormat(closest);
            if (closest)
                CoTaskMemFree(closest);
            const HRESULT exclusive_status = audio_client->IsFormatSupported(
                AUDCLNT_SHAREMODE_EXCLUSIVE, wave, nullptr);
            evidence << "format_probe\t" << contract->property_page_label
                     << "\trequested=" << SerializeWaveFormat(wave)
                     << "\tshared_hr=0x" << std::hex << std::setw(8)
                     << static_cast<std::uint32_t>(shared_status)
                     << "\tshared_closest=" << closest_text
                     << "\texclusive_hr=0x" << std::setw(8)
                     << static_cast<std::uint32_t>(exclusive_status) << "\n";
        }
    }
    if (mix_format)
        CoTaskMemFree(mix_format);
    PropVariantClear(&device_format);
    PropVariantClear(&friendly_name);

    HANDLE output = CreateFileW(evidence_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    const std::string text = evidence.str();
    const bool wrote = WriteAll(output, text) && FlushFileBuffers(output);
    CloseHandle(output);
    if (!wrote)
    {
        DeleteFileW(evidence_path.c_str());
        return E_FAIL;
    }
    std::wprintf(L"AUDIO_ENDPOINT_CAPABILITIES_COMPLETE endpoint=\"%ls\" evidence=\"%ls\"\n",
                 endpoint_identity.c_str(), evidence_path.c_str());
    return S_OK;
}

HRESULT RunNativeRendererProbe(const std::filesystem::path &runtime_dir,
                               const std::filesystem::path &manifest_path,
                               const std::filesystem::path &fixture_path,
                               const wchar_t *renderer_moniker,
                               const LAVOpenJocOutputPolicy policy,
                               const std::filesystem::path &evidence_path)
{
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    if (!contract || !renderer_moniker || !*renderer_moniker || !evidence_path.is_absolute() ||
        !std::filesystem::is_directory(evidence_path.parent_path()))
        return E_INVALIDARG;

    std::vector<StagedRecord> records;
    FixtureIdentity fixture;
    if (!ReadStagedManifest(runtime_dir, manifest_path, &records) ||
        !BuildFixtureIdentity(fixture_path, &fixture))
        return E_INVALIDARG;
    const StagedRecord *audio_record = FindRecord(records, StagedKind::Module, L"LAVAudio.ax");
    const StagedRecord *splitter_record =
        FindRecord(records, StagedKind::Module, L"LAVSplitter.ax");
    if (!audio_record || !splitter_record)
        return E_UNEXPECTED;

    const std::wstring runtime_final = FinalPathForFile(runtime_dir);
    ScopedActivationContext activation(audio_record->final_path, runtime_final);
    if (!activation.active())
        return HRESULT_FROM_WIN32(GetLastError());
    PrivateComModule audio(audio_record->final_path, kTargetLavAudio);
    PrivateComModule splitter(splitter_record->final_path, kLavSplitterSource);
    LoadedDependenciesOwner dependencies;
    if (FAILED(audio.status()) || FAILED(splitter.status()) ||
        !LoadStagedDependencies(records, dependencies.put()))
        return E_UNEXPECTED;

    const CMediaType requested = BuildStrictTarget(*contract);
    std::unique_ptr<NativeRendererGraph> initial(new (std::nothrow) NativeRendererGraph());
    if (!initial)
        return E_OUTOFMEMORY;
    BuildNativeRendererGraph(audio, splitter, fixture, policy, renderer_moniker, requested,
                             initial.get());
    const NativeGraphSetupEvidence initial_setup = SnapshotNativeGraphSetup(*initial);

    NativePlaybackEvidence initial_playback;
    if (initial->connect_status == S_OK)
        RunNativeInitialPlayback(initial.get(), requested, &initial_playback);

    HRESULT duration_status = E_UNEXPECTED;
    LONGLONG duration = 0;
    NativeSeekEvidence seek_25;
    NativeSeekEvidence seek_forward;
    NativeSeekEvidence seek_backward;
    seek_25.label = "seek-25";
    seek_forward.label = "seek-75-forward";
    seek_backward.label = "seek-25-backward";
    if (initial_playback.eos_complete && initial->seeking_status == S_OK)
    {
        duration_status = initial->seeking->GetDuration(&duration);
        if (duration_status == S_OK && duration > 0)
        {
            RunNativeSeekEpoch(initial.get(), requested, seek_25.label, duration / 4, &seek_25);
            if (seek_25.complete)
                RunNativeSeekEpoch(initial.get(), requested, seek_forward.label,
                                   duration * 3 / 4, &seek_forward);
            if (seek_forward.complete)
                RunNativeSeekEpoch(initial.get(), requested, seek_backward.label,
                                   duration / 4, &seek_backward);
        }
    }

    HRESULT stop_status = E_UNEXPECTED;
    if (initial->control)
        stop_status = initial->control->Stop();
    initial.reset();

    NativeGraphSetupEvidence reopen_setup;
    NativePlaybackEvidence reopen_playback;
    HRESULT reopen_stop_status = E_UNEXPECTED;
    bool reopen_complete = false;
    if (initial_playback.eos_complete && seek_25.complete && seek_forward.complete &&
        seek_backward.complete && stop_status == S_OK)
    {
        NativeRendererGraph reopen;
        BuildNativeRendererGraph(audio, splitter, fixture, policy, renderer_moniker, requested,
                                 &reopen);
        reopen_setup = SnapshotNativeGraphSetup(reopen);
        if (reopen.connect_status == S_OK)
            RunNativeInitialPlayback(&reopen, requested, &reopen_playback);
        if (reopen.control)
            reopen_stop_status = reopen.control->Stop();
        reopen_complete =
            reopen_setup.create_graph_status == S_OK &&
            reopen_setup.bind_renderer_status == S_OK &&
            reopen_setup.add_renderer_status == S_OK &&
            reopen_setup.find_renderer_pin_status == S_OK &&
            reopen_setup.connect_attempted && reopen_setup.connect_status == S_OK &&
            reopen_setup.graph_exactly_three_filters &&
            NativeTypeObservationIsExact(reopen_playback.pre_types) &&
            reopen_playback.eos_complete &&
            reopen_playback.renderer_stats_status == S_OK &&
            reopen_playback.last_buffer_duration > 0 &&
            reopen_playback.diagnostics_status == S_OK &&
            reopen_playback.classifier_bytes > 0 && reopen_playback.stream_bytes > 0 &&
            NativeTypeObservationIsExact(reopen_playback.post_types) &&
            reopen_stop_status == S_OK;
    }

    std::vector<LoadedModule> loaded;
    const bool runtime_identity =
        RuntimeIdentityMatches(records) && EnumerateLoadedModules(&loaded);
    const bool fixture_identity = FixtureIdentityMatches(fixture);

    openjoc_harness_core::NativeTypeAggregateEvidence observed_types;
    const bool initial_types_executed = initial_setup.connect_status == S_OK;
    observed_types = AccumulateObservedNativeType(
        observed_types, initial_types_executed, initial_playback.pre_types);
    observed_types = AccumulateObservedNativeType(
        observed_types, initial_types_executed, initial_playback.post_types);
    observed_types = AccumulateObservedNativeType(
        observed_types, seek_25.set_positions_attempted, seek_25.post_types);
    observed_types = AccumulateObservedNativeType(
        observed_types, seek_forward.set_positions_attempted, seek_forward.post_types);
    observed_types = AccumulateObservedNativeType(
        observed_types, seek_backward.set_positions_attempted, seek_backward.post_types);
    const bool reopen_types_executed = reopen_setup.connect_status == S_OK;
    observed_types = AccumulateObservedNativeType(
        observed_types, reopen_types_executed, reopen_playback.pre_types);
    observed_types = AccumulateObservedNativeType(
        observed_types, reopen_types_executed, reopen_playback.post_types);

    openjoc_harness_core::NativeProbeEvidence evidence;
    evidence.runtime_identity = runtime_identity;
    evidence.fixture_identity = fixture_identity;
    evidence.graph_setup_complete =
        initial_setup.create_graph_status == S_OK &&
        initial_setup.bind_renderer_status == S_OK &&
        initial_setup.add_renderer_status == S_OK &&
        initial_setup.find_renderer_pin_status == S_OK &&
        initial_setup.graph_exactly_three_filters && initial_setup.control_status == S_OK &&
        initial_setup.events_status == S_OK && initial_setup.seeking_status == S_OK &&
        initial_setup.diagnostics_interface_status == S_OK;
    evidence.connect_attempted = initial_setup.connect_attempted;
    evidence.connect_direct_status = initial_setup.connect_status;
    evidence.type_observations = observed_types;
    evidence.pause_call_status = initial_playback.pause_call_status;
    evidence.pause_state_status = initial_playback.pause_state_status;
    evidence.pause_state = initial_playback.pause_state;
    evidence.run_call_status = initial_playback.run_call_status;
    evidence.run_state_status = initial_playback.run_state_status;
    evidence.run_state = initial_playback.run_state;
    evidence.wait_completion_status = initial_playback.wait_status;
    evidence.completion_code = initial_playback.completion_code;
    evidence.renderer_stats_status = initial_playback.renderer_stats_status;
    evidence.renderer_last_buffer_duration = initial_playback.last_buffer_duration;
    evidence.midstream_renderer_stats_status =
        initial_playback.midstream_renderer_stats_status;
    evidence.midstream_last_buffer_duration =
        initial_playback.midstream_last_buffer_duration;
    evidence.diagnostics_status = initial_playback.diagnostics_status;
    evidence.classifier_bytes = initial_playback.classifier_bytes;
    evidence.stream_bytes = initial_playback.stream_bytes;
    evidence.initial_eos_complete = initial_playback.eos_complete;
    evidence.seek_25_complete = seek_25.complete;
    evidence.forward_seek_complete = seek_forward.complete;
    evidence.backward_seek_complete = seek_backward.complete;
    evidence.stop_status = stop_status;
    evidence.reopen_complete = reopen_complete;
    const openjoc_harness_core::NativeProbeState probe_state =
        openjoc_harness_core::ClassifyNativeProbe(evidence);
    const char *result = NativeProbeStateText(probe_state);

    const std::string moniker_utf8 = WideToUtf8(renderer_moniker);
    const std::string fixture_utf8 = WideToUtf8(fixture.final_path.native());
    const std::string requested_text = SerializeMediaType(requested);
    if (moniker_utf8.empty() || fixture_utf8.empty() || requested_text.empty() ||
        moniker_utf8.find_first_of("\t\r\n") != std::string::npos)
        return E_INVALIDARG;
    std::ostringstream record;
    record << "NATIVE_RENDERER_PROBE_V1\n"
           << "result\t" << result << "\n"
           << "renderer_moniker\t" << moniker_utf8 << "\n"
           << "fixture_path\t" << fixture_utf8 << "\n"
           << "fixture_sha256\t" << DigestHex(fixture.sha256) << "\n"
           << "policy\t" << static_cast<std::uint32_t>(policy) << "\n"
           << "connect_attempted\t" << (initial_setup.connect_attempted ? 1 : 0) << "\n"
           << "proposal_count\t" << (initial_setup.connect_attempted ? 1 : 0) << "\n"
           << "fallback_proposals\t0\n"
           << "requested_type\t" << requested_text << "\n"
           << "create_graph_hr\t0x" << std::hex << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(initial_setup.create_graph_status) << "\n"
           << "bind_renderer_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_setup.bind_renderer_status) << "\n"
           << "add_renderer_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_setup.add_renderer_status) << "\n"
           << "find_renderer_pin_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_setup.find_renderer_pin_status) << "\n"
           << "connect_direct_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_setup.connect_status) << "\n"
           << "control_qi_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_setup.control_status) << "\n"
           << "events_qi_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_setup.events_status) << "\n"
           << "seeking_qi_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_setup.seeking_status) << "\n"
           << "diagnostics_qi_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_setup.diagnostics_interface_status) << "\n"
           << "graph_exactly_three_filters\t" << std::dec
           << (initial_setup.graph_exactly_three_filters ? 1 : 0) << "\n"
           << "graph_setup_complete\t" << (evidence.graph_setup_complete ? 1 : 0) << "\n"
           << "type_observation_count\t" << observed_types.observation_count << "\n"
           << "type_successful_observation_count\t"
           << observed_types.successful_observation_count << "\n"
           << "type_query_failure\t" << (observed_types.query_failure ? 1 : 0) << "\n"
           << "type_mismatch_observed\t"
           << (observed_types.mismatch_observed ? 1 : 0) << "\n"
           << "pre_output_type_hr\t0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.pre_types.output_status) << "\n"
           << "pre_renderer_input_type_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(
                  initial_playback.pre_types.renderer_input_status)
           << "\n"
           << "post_output_type_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.post_types.output_status) << "\n"
           << "post_renderer_input_type_hr\t0x" << std::setw(8)
           << static_cast<std::uint32_t>(
                  initial_playback.post_types.renderer_input_status)
           << "\n"
           << "operation\t1\tinitial_stream"
           << "\tstream_attempted=" << std::dec
           << (initial_playback.stream_attempted ? 1 : 0)
           << "\tpause_call_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.pause_call_status)
           << "\tpause_state_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.pause_state_status)
           << "\tpause_state=" << std::dec << static_cast<int>(initial_playback.pause_state)
           << "\trun_call_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.run_call_status)
           << "\trun_state_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.run_state_status)
           << "\trun_state=" << std::dec << static_cast<int>(initial_playback.run_state)
           << "\twait_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.wait_status)
           << "\tcompletion_code=" << std::dec << initial_playback.completion_code
           << "\tgraph_error_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.graph_error)
           << "\tmidstream_wait_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.midstream_wait_status)
           << "\tmidstream_renderer_stats_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(
                  initial_playback.midstream_renderer_stats_status)
           << "\tmidstream_last_buffer_duration=" << std::dec
           << initial_playback.midstream_last_buffer_duration
           << "\trenderer_stats_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.renderer_stats_status)
           << "\tlast_buffer_duration=" << std::dec
           << initial_playback.last_buffer_duration
           << "\tdiagnostics_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(initial_playback.diagnostics_status)
           << "\tclassifier_bytes=" << std::dec << initial_playback.classifier_bytes
           << "\tstream_bytes=" << initial_playback.stream_bytes
           << "\teos_complete=" << (initial_playback.eos_complete ? 1 : 0) << "\n"
           << "duration_hr\t0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(duration_status) << "\n"
           << "duration\t" << std::dec << duration << "\n";
    AppendNativeTypeObservation(record, "pre_stream", initial_playback.pre_types);
    AppendNativeTypeObservation(record, "post_stream", initial_playback.post_types);
    AppendNativeSeekEvidence(record, 2, seek_25);
    AppendNativeSeekEvidence(record, 3, seek_forward);
    AppendNativeSeekEvidence(record, 4, seek_backward);
    record << "operation\t5\tstop\thr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(stop_status)
           << "\tcomplete=" << std::dec << (stop_status == S_OK ? 1 : 0) << "\n"
           << "operation\t6\treopen_connect"
           << "\tcreate_graph_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(reopen_setup.create_graph_status)
           << "\tbind_renderer_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(reopen_setup.bind_renderer_status)
           << "\tadd_renderer_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(reopen_setup.add_renderer_status)
           << "\tfind_renderer_pin_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(reopen_setup.find_renderer_pin_status)
           << "\tconnect_attempted=" << std::dec
           << (reopen_setup.connect_attempted ? 1 : 0)
           << "\tproposal_count=" << (reopen_setup.connect_attempted ? 1 : 0)
           << "\tconnect_direct_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(reopen_setup.connect_status) << "\n"
           << "operation\t7\treopen_stream"
           << "\tstream_attempted=" << std::dec
           << (reopen_playback.stream_attempted ? 1 : 0)
           << "\tpause_call_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(reopen_playback.pause_call_status)
           << "\tpause_state_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(reopen_playback.pause_state_status)
           << "\trun_call_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(reopen_playback.run_call_status)
           << "\trun_state_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(reopen_playback.run_state_status)
           << "\twait_hr=0x" << std::setw(8)
           << static_cast<std::uint32_t>(reopen_playback.wait_status)
           << "\tcompletion_code=" << std::dec << reopen_playback.completion_code
           << "\trenderer_stats_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(reopen_playback.renderer_stats_status)
           << "\tlast_buffer_duration=" << std::dec
           << reopen_playback.last_buffer_duration
           << "\tdiagnostics_hr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(reopen_playback.diagnostics_status)
           << "\tclassifier_bytes=" << std::dec << reopen_playback.classifier_bytes
           << "\tstream_bytes=" << reopen_playback.stream_bytes
           << "\teos_complete=" << (reopen_playback.eos_complete ? 1 : 0) << "\n";
    AppendNativeTypeObservation(record, "reopen_pre_stream", reopen_playback.pre_types);
    AppendNativeTypeObservation(record, "reopen_post_stream", reopen_playback.post_types);
    record << "operation\t8\treopen_stop\thr=0x" << std::hex << std::setw(8)
           << static_cast<std::uint32_t>(reopen_stop_status)
           << "\tcomplete=" << std::dec << (reopen_stop_status == S_OK ? 1 : 0) << "\n"
           << "reopen_complete\t" << (reopen_complete ? 1 : 0) << "\n"
           << "runtime_identity\t" << (runtime_identity ? 1 : 0) << "\n"
           << "fixture_identity\t" << (fixture_identity ? 1 : 0) << "\n"
           << "private_module\tLAVAudio.ax\t" << DigestHex(audio.sha256()) << "\t"
           << WideToUtf8(audio.path()) << "\n"
           << "private_module\tLAVSplitter.ax\t" << DigestHex(splitter.sha256()) << "\t"
           << WideToUtf8(splitter.path()) << "\n";
    for (const auto &module : loaded)
    {
        if (FindRecord(records, StagedKind::Module, module.basename))
            record << "process_module\t" << WideToUtf8(module.basename) << "\t"
                   << DigestHex(module.sha256) << "\t" << WideToUtf8(module.final_path) << "\n";
    }

    HANDLE output = CreateFileW(evidence_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    const std::string text = record.str();
    const bool wrote = WriteAll(output, text) && FlushFileBuffers(output);
    CloseHandle(output);
    if (!wrote)
    {
        DeleteFileW(evidence_path.c_str());
        return E_FAIL;
    }
    std::wprintf(L"NATIVE_RENDERER_PROBE_COMPLETE result=%hs evidence=\"%ls\"\n", result,
                 evidence_path.c_str());
    return strcmp(result, "UNVERIFIED") == 0 ? E_UNEXPECTED : S_OK;
}

HRESULT RunSelfTest(const std::filesystem::path &runtime_dir,
                    const std::filesystem::path &manifest_path, REFCLSID audio_class_id)
{
    std::vector<StagedRecord> records;
    if (!ReadStagedManifest(runtime_dir, manifest_path, &records))
        return E_INVALIDARG;
    const StagedRecord *audio_record = FindRecord(records, StagedKind::Module, L"LAVAudio.ax");
    const StagedRecord *splitter_record = FindRecord(records, StagedKind::Module, L"LAVSplitter.ax");
    const StagedRecord *dependency_manifest =
        FindRecord(records, StagedKind::File, kDependencyManifestName);
    if (!audio_record || !splitter_record || !dependency_manifest)
        return E_UNEXPECTED;

    const std::wstring runtime_final = FinalPathForFile(runtime_dir);
    // Build the activation context from LAVAudio's embedded manifest (resource
    // 2). Its declared LAVFilters.Dependencies assembly is then resolved from
    // this exact staged directory, where the separately hashed external
    // LAVFilters.Dependencies.manifest lives.
    ScopedActivationContext activation(audio_record->final_path, runtime_final);
    if (!activation.active())
        return HRESULT_FROM_WIN32(GetLastError());

    PrivateComModule audio(audio_record->final_path, audio_class_id);
    if (FAILED(audio.status()))
    {
        std::fwprintf(stderr, L"private LAVAudio activation failed: 0x%08lx\n",
                      static_cast<unsigned long>(audio.status()));
        return audio.status();
    }
    PrivateComModule splitter(splitter_record->final_path, kLavSplitterSource);
    if (FAILED(splitter.status()))
    {
        std::fwprintf(stderr, L"private LAVSplitter activation failed: 0x%08lx\n",
                      static_cast<unsigned long>(splitter.status()));
        return splitter.status();
    }
    // CAPI and libbluray are fixed self-test inventory, so load their absolute
    // manifest paths explicitly. The filters naturally load only the private
    // FFmpeg DLLs they import; runtime identity validates that actual subset.
    std::vector<HMODULE> dependencies;
    if (!LoadStagedDependencies(records, &dependencies))
        return E_UNEXPECTED;

    IBaseFilter *audio_filter = nullptr;
    IBaseFilter *splitter_filter = nullptr;
    HRESULT status = audio.CreateInstance(IID_IBaseFilter,
                                          reinterpret_cast<void **>(&audio_filter));
    if (SUCCEEDED(status) && !audio_filter)
        status = E_UNEXPECTED;
    if (SUCCEEDED(status))
        status = splitter.CreateInstance(IID_IBaseFilter,
                                         reinterpret_cast<void **>(&splitter_filter));
    if (SUCCEEDED(status) && !splitter_filter)
        status = E_UNEXPECTED;
    if (SUCCEEDED(status) &&
        (!TestExactMediaTypeComparison() || !TestPureHelpers() || !TestStrictCaptureSinkPolicy() ||
         !TestDiagnosticsAbi(audio_filter, IsEqualGUID(audio_class_id, kTargetLavAudio)) ||
         !RuntimeIdentityMatches(records)))
        status = E_UNEXPECTED;
    Release(splitter_filter);
    Release(audio_filter);
    FreeModules(&dependencies);
    return status;
}
} // namespace openjoc_harness_shell

int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && wcscmp(argv[1], L"--classify") == 0)
    {
        std::vector<unsigned char> bytes;
        if (!openjoc_harness_shell::ReadFixtureBytes(argv[2], &bytes))
            return 66;
        LAVOpenJocDecoder decoder;
        if (!decoder.IsAvailable())
            return 67;
        const LAVOpenJocProcessResult result = decoder.Process(
            bytes.data(), bytes.size(), (std::numeric_limits<std::int64_t>::min)(), true);
        std::wprintf(L"OPENJOC_CLASSIFY fixture=%ls result=%u state=%u "
                     L"classifier_input_bytes=%llu stream_input_bytes=%llu error=%hs\n",
                     argv[2], static_cast<unsigned int>(result),
                     static_cast<unsigned int>(decoder.State()),
                     static_cast<unsigned long long>(decoder.ClassifierInputBytes()),
                     static_cast<unsigned long long>(decoder.StreamInputBytes()),
                     decoder.LastError());
        return result != LAVOpenJocProcessResult::OpenJoc &&
                       decoder.State() != LAVOpenJocState::OpenJoc &&
                       decoder.StreamInputBytes() == 0
                   ? 0
                   : 1;
    }
    if (argc == 4 && wcscmp(argv[1], L"--write-manifest") == 0)
    {
        if (!openjoc_harness_shell::WriteStagedManifest(argv[2], argv[3]))
        {
            std::fwprintf(stderr, L"staged manifest generation failed\n");
            return 1;
        }
        std::wprintf(L"staged manifest generated: %ls\n", argv[3]);
        return 0;
    }
    if (argc == 3 && wcscmp(argv[1], L"--list-audio-renderers") == 0)
    {
        const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com_status))
            return 1;
        const HRESULT inventory_status =
            openjoc_harness_shell::WriteAudioRendererInventory(argv[2]);
        CoUninitialize();
        if (FAILED(inventory_status))
        {
            std::fwprintf(stderr, L"audio renderer inventory failed: 0x%08lx\n",
                          static_cast<unsigned long>(inventory_status));
            return 1;
        }
        return 0;
    }
    if (argc == 4 && wcscmp(argv[1], L"--inspect-audio-endpoint") == 0)
    {
        const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com_status))
            return 1;
        const HRESULT inspection_status =
            openjoc_harness_shell::WriteAudioEndpointCapabilities(argv[2], argv[3]);
        CoUninitialize();
        if (FAILED(inspection_status))
        {
            std::fwprintf(stderr, L"audio endpoint inspection failed: 0x%08lx\n",
                          static_cast<unsigned long>(inspection_status));
            return 1;
        }
        return 0;
    }
    if (argc == 8 && wcscmp(argv[1], L"--native-renderer-probe") == 0)
    {
        wchar_t *policy_end = nullptr;
        const unsigned long policy_value = wcstoul(argv[6], &policy_end, 10);
        if (!policy_end || *policy_end != L'\0' ||
            policy_value >= LAV_OPENJOC_OUTPUT_CONTRACT_COUNT)
            return 64;
        const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com_status))
            return 1;
        const HRESULT probe_status = openjoc_harness_shell::RunNativeRendererProbe(
            argv[2], argv[3], argv[4], argv[5],
            static_cast<LAVOpenJocOutputPolicy>(policy_value), argv[7]);
        CoUninitialize();
        if (FAILED(probe_status))
        {
            std::fwprintf(stderr, L"native renderer probe failed: 0x%08lx\n",
                          static_cast<unsigned long>(probe_status));
            return 1;
        }
        return 0;
    }
    if (argc == 6 && wcscmp(argv[1], L"--compare-task3-evidence") == 0)
    {
        wchar_t *policy_end = nullptr;
        const unsigned long policy_value = wcstoul(argv[4], &policy_end, 10);
        const bool passthrough = wcscmp(argv[5], L"passthrough") == 0;
        const bool mode_valid = passthrough || wcscmp(argv[5], L"stock") == 0;
        const bool equal = policy_end && *policy_end == L'\0' &&
                           policy_value < LAV_OPENJOC_OUTPUT_CONTRACT_COUNT && mode_valid &&
                           openjoc_harness_shell::CompareTask3Evidence(
                               argv[2], argv[3],
                               static_cast<LAVOpenJocOutputPolicy>(policy_value), passthrough);
        if (!equal)
        {
            std::fwprintf(stderr, L"TASK3_UNVERIFIED: target/pristine evidence mismatch\n");
            return 1;
        }
        std::wprintf(L"TASK3_CONTROL_COMPLETE: exact target/pristine evidence matched; "
                     L"renderer state remains UNVERIFIED\n");
        return 0;
    }
    const bool stock_worker = argc == 8 && wcscmp(argv[1], L"--stock-eac3-worker") == 0;
    const bool passthrough_worker =
        argc == 8 && wcscmp(argv[1], L"--eac3-passthrough-worker") == 0;
    if (stock_worker || passthrough_worker)
    {
        wchar_t *policy_end = nullptr;
        const unsigned long policy_value = wcstoul(argv[7], &policy_end, 10);
        const bool target_lane = wcscmp(argv[4], L"target") == 0;
        const bool pristine_lane = wcscmp(argv[4], L"pristine") == 0;
        if ((!target_lane && !pristine_lane) || !policy_end || *policy_end != L'\0' ||
            policy_value >= LAV_OPENJOC_OUTPUT_CONTRACT_COUNT)
            return 64;
        const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com_status))
            return 1;
        const HRESULT status = openjoc_harness_shell::RunTask3StockOrPassthroughWorker(
            argv[2], argv[3], argv[5], argv[6], target_lane,
            static_cast<LAVOpenJocOutputPolicy>(policy_value), passthrough_worker);
        CoUninitialize();
        if (FAILED(status))
        {
            std::fwprintf(stderr, L"TASK3_UNVERIFIED: %ls worker failed: 0x%08lx\n",
                          stock_worker ? L"stock" : L"passthrough",
                          static_cast<unsigned long>(status));
            return 1;
        }
        std::wprintf(L"TASK3_CONTROL_COMPLETE: %ls worker passed; renderer state remains "
                     L"UNVERIFIED\n",
                     stock_worker ? L"stock" : L"passthrough");
        return 0;
    }
    const bool controlled_sink = argc == 5 && wcscmp(argv[1], L"--controlled-sink") == 0;
    const bool lifecycle = argc == 5 && wcscmp(argv[1], L"--openjoc-lifecycle") == 0;
    const bool allocator_performance =
        argc == 5 && wcscmp(argv[1], L"--allocator-performance") == 0;
    const bool ac3_bitstream = argc == 5 && wcscmp(argv[1], L"--ac3-bitstream") == 0;
    const bool legacy_core = argc == 6 && wcscmp(argv[1], L"--legacy-core") == 0;
    if (!controlled_sink && !lifecycle && !allocator_performance && !ac3_bitstream && !legacy_core &&
        (argc != 5 || wcscmp(argv[1], L"--self-test") != 0 ||
        (wcscmp(argv[4], L"target") != 0 && wcscmp(argv[4], L"pristine") != 0))
       )
    {
        std::fwprintf(stderr,
                      L"usage: OpenJocDirectShowNegotiationSmoke.exe --write-manifest "
                      L"<runtime-dir> <runtime-dir\\OpenJocRuntimeIdentity.tsv>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --self-test <runtime-dir> "
                      L"<runtime-dir\\OpenJocRuntimeIdentity.tsv> <target|pristine>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --controlled-sink "
                      L"<runtime-dir> <runtime-dir\\OpenJocRuntimeIdentity.tsv> <fixture-dir>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe "
                      L"--stock-eac3-worker|--eac3-passthrough-worker <runtime-dir> "
                      L"<manifest> <target|pristine> <fixture> <evidence> <policy>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --openjoc-lifecycle ...\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --allocator-performance "
                      L"<runtime-dir> <manifest> <joc.multi.ec3>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --legacy-core "
                      L"<runtime-dir> <manifest> <fixture.ac3> <policy>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --ac3-bitstream "
                      L"<runtime-dir> <manifest> <fixture.ac3>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --native-renderer-probe "
                      L"<runtime-dir> <manifest> <fixture> <renderer-moniker> <policy> "
                      L"<new-evidence-file>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --list-audio-renderers "
                      L"<new-evidence-file>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --inspect-audio-endpoint "
                      L"<endpoint-id> <new-evidence-file>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --compare-task3-evidence "
                      L"<target-evidence> <pristine-evidence> <policy> <stock|passthrough>\n");
        return 64;
    }
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_status))
    {
        std::fwprintf(stderr, L"self-test COM initialization failed: 0x%08lx\n",
                      static_cast<unsigned long>(com_status));
        return 1;
    }
    const GUID &audio_class_id = !controlled_sink && !lifecycle && !allocator_performance &&
                                         !ac3_bitstream && !legacy_core &&
                                         wcscmp(argv[4], L"pristine") == 0
                                     ? kPristineLavAudio
                                     : kTargetLavAudio;
    wchar_t *legacy_policy_end = nullptr;
    const unsigned long legacy_policy_value = legacy_core ? wcstoul(argv[5], &legacy_policy_end, 10) : 0;
    const bool legacy_policy_valid = !legacy_core ||
                                     (legacy_policy_end && *legacy_policy_end == L'\0' &&
                                      legacy_policy_value < LAV_OPENJOC_OUTPUT_CONTRACT_COUNT);
    const HRESULT status = !legacy_policy_valid
                               ? E_INVALIDARG
                           : ac3_bitstream
                               ? openjoc_harness_shell::RunAc3BitstreamGraphCase(
                                     argv[2], argv[3], argv[4])
                           : legacy_core
                               ? openjoc_harness_shell::RunLegacyCoreGraphCase(
                                     argv[2], argv[3], argv[4],
                                     static_cast<LAVOpenJocOutputPolicy>(legacy_policy_value))
                           : allocator_performance
                               ? openjoc_harness_shell::RunTask4AllocatorPerformance(
                                     argv[2], argv[3], argv[4])
                           : lifecycle
                               ? openjoc_harness_shell::RunOpenJocLifecycleMatrix(argv[2], argv[3],
                                                                                  argv[4])
                           : controlled_sink
                               ? openjoc_harness_shell::RunControlledSinkMatrix(argv[2], argv[3],
                                                                                argv[4])
                               : openjoc_harness_shell::RunSelfTest(argv[2], argv[3],
                                                                    audio_class_id);
    CoUninitialize();
    if (FAILED(status))
    {
        if (controlled_sink)
            std::fwprintf(stderr, L"UNVERIFIED: controlled-sink matrix failed: 0x%08lx\n",
                          static_cast<unsigned long>(status));
        else if (lifecycle)
            std::fwprintf(stderr, L"TASK3_UNVERIFIED: lifecycle matrix failed: 0x%08lx\n",
                          static_cast<unsigned long>(status));
        else if (ac3_bitstream)
            std::fwprintf(stderr, L"AC-3 bitstream graph failed: 0x%08lx\n",
                          static_cast<unsigned long>(status));
        else if (legacy_core)
            std::fwprintf(stderr, L"legacy-core graph failed: 0x%08lx\n",
                          static_cast<unsigned long>(status));
        else if (allocator_performance)
            std::fwprintf(stderr, L"TASK4_UNVERIFIED: allocator/performance failed: 0x%08lx\n",
                          static_cast<unsigned long>(status));
        else
            std::fwprintf(stderr, L"self-test failed: 0x%08lx\n",
                          static_cast<unsigned long>(status));
        return 1;
    }
    if (controlled_sink)
    {
        std::wprintf(L"CONTROLLED_SINK_COMPLETE: exact private graph matrix passed; "
                     L"renderer state remains UNVERIFIED\n");
        return 0;
    }
    if (lifecycle)
    {
        std::wprintf(L"TASK3_CONTROL_COMPLETE: lifecycle matrix passed; renderer state remains "
                     L"UNVERIFIED\n");
        return 0;
    }
    if (ac3_bitstream)
    {
        std::wprintf(L"AC3_BITSTREAM_INTEGRATION_COMPLETE\n");
        return 0;
    }
    if (legacy_core)
    {
        std::wprintf(L"LEGACY_CORE_INTEGRATION_COMPLETE\n");
        return 0;
    }
    if (allocator_performance)
        return 0;
    std::wprintf(L"UNVERIFIED: lane-private Audio/Splitter activation and staged identity passed\n");
    return 0;
}
