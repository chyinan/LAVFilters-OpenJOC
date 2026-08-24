/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#include "OpenJocStrictNegotiation.h"

#include <limits>

HRESULT DeliverLAVOpenJocStrictMediaType(const LAVOpenJocOutputContract *contract,
                                         const AM_MEDIA_TYPE &candidate, const bool media_type_changed,
                                         const long required_bytes,
                                         const LAVOpenJocStrictDeliveryOperations &operations)
{
    if (!contract)
        return S_FALSE;
    if (required_bytes < 0 || !IsExactLAVOpenJocStrictMediaType(*contract, candidate) || !operations.reconnect ||
        !operations.acquire_sample || !operations.release_attached_type || !operations.release_sample ||
        !operations.deliver)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = S_OK;
    if (media_type_changed)
    {
        if (!operations.query_accept || !operations.set_sample_media_type || !operations.set_output_media_type)
            return E_INVALIDARG;
        hr = NormalizeLAVOpenJocQueryAcceptResult(operations.query_accept(candidate));
        if (FAILED(hr))
            return hr;
    }

    if (FAILED(hr = operations.reconnect(required_bytes, candidate)))
        return hr;

    LAVOpenJocStrictAcquiredSample sample{};
    hr = operations.acquire_sample(&sample);
    if (FAILED(hr))
    {
        if (sample.attached_type)
            operations.release_attached_type(sample.attached_type);
        if (sample.handle)
            operations.release_sample(sample.handle);
        return hr;
    }

    const HRESULT validation_hr = ValidateLAVOpenJocDeliverySample(*contract, sample.attached_type,
                                                                    sample.capacity, required_bytes);
    if (sample.attached_type)
        operations.release_attached_type(sample.attached_type);
    if (FAILED(validation_hr) || !sample.handle || !sample.data)
    {
        if (sample.handle)
            operations.release_sample(sample.handle);
        return FAILED(validation_hr) ? validation_hr : E_POINTER;
    }

    if (media_type_changed)
    {
        if (FAILED(hr = operations.set_sample_media_type(sample.handle, candidate)) ||
            FAILED(hr = operations.set_output_media_type(candidate)))
        {
            operations.release_sample(sample.handle);
            return hr;
        }
    }

    hr = operations.deliver(sample.handle, sample.data, required_bytes);
    operations.release_sample(sample.handle);
    return hr;
}

HRESULT ExecuteLAVOpenJocQueueTransaction(const LAVOpenJocQueueTransactionInput &input,
                                          const LAVOpenJocQueueTransactionOperations &operations,
                                          LAVOpenJocQueueTransactionResult *result)
{
    if (!result || input.sample_rate == 0 || !operations.flush || !operations.prepare_metadata ||
        !operations.swap_buffer || !operations.append_buffer)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = S_OK;
    if (!input.compatible && input.queued_samples > 0 && FAILED(hr = operations.flush()))
        return hr;
    if (!input.compatible && FAILED(hr = operations.prepare_metadata()))
        return hr;

    const std::uint32_t effective_queued_samples = input.compatible ? input.queued_samples : 0;
    std::uint32_t merged_samples = 0;
    if (!CheckedLAVOpenJocSampleAdd(effective_queued_samples, input.incoming_samples, &merged_samples))
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);

    REFERENCE_TIME merged_start = input.compatible ? input.queued_start : input.incoming_start;
    if (effective_queued_samples == 0)
    {
        merged_start = input.incoming_start;
        operations.swap_buffer();
    }
    else
    {
        if (merged_start == (std::numeric_limits<REFERENCE_TIME>::min)() &&
            input.incoming_start != (std::numeric_limits<REFERENCE_TIME>::min)())
        {
            merged_start = input.incoming_start -
                           (REFERENCE_TIME)((double)effective_queued_samples / input.sample_rate * 10000000.0);
        }
        if (FAILED(hr = operations.append_buffer()))
            return hr;
    }

    result->sample_count = merged_samples;
    result->start_time = merged_start;
    return S_OK;
}
