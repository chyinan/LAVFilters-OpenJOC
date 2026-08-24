/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#pragma once

#include "OpenJocStrictOutput.h"

#include <functional>

struct LAVOpenJocStrictAcquiredSample
{
    void *handle = nullptr;
    BYTE *data = nullptr;
    AM_MEDIA_TYPE *attached_type = nullptr;
    long capacity = 0;
};

struct LAVOpenJocStrictDeliveryOperations
{
    std::function<HRESULT(const AM_MEDIA_TYPE &)> query_accept;
    std::function<HRESULT(long, const AM_MEDIA_TYPE &)> reconnect;
    std::function<HRESULT(LAVOpenJocStrictAcquiredSample *)> acquire_sample;
    std::function<void(AM_MEDIA_TYPE *)> release_attached_type;
    std::function<void(void *)> release_sample;
    std::function<HRESULT(void *, const AM_MEDIA_TYPE &)> set_sample_media_type;
    std::function<HRESULT(const AM_MEDIA_TYPE &)> set_output_media_type;
    std::function<HRESULT(void *, BYTE *, long)> deliver;
};

struct LAVOpenJocQueueTransactionInput
{
    bool compatible = false;
    std::uint32_t queued_samples = 0;
    std::uint32_t incoming_samples = 0;
    REFERENCE_TIME queued_start = 0;
    REFERENCE_TIME incoming_start = 0;
    std::uint32_t sample_rate = 0;
};

struct LAVOpenJocQueueTransactionResult
{
    std::uint32_t sample_count = 0;
    REFERENCE_TIME start_time = 0;
};

struct LAVOpenJocQueueTransactionOperations
{
    std::function<HRESULT()> flush;
    std::function<HRESULT()> prepare_metadata;
    std::function<void()> swap_buffer;
    std::function<HRESULT()> append_buffer;
};

[[nodiscard]] HRESULT DeliverLAVOpenJocStrictMediaType(
    const LAVOpenJocOutputContract *contract, const AM_MEDIA_TYPE &candidate, bool media_type_changed,
    long required_bytes, const LAVOpenJocStrictDeliveryOperations &operations);

[[nodiscard]] HRESULT ExecuteLAVOpenJocQueueTransaction(
    const LAVOpenJocQueueTransactionInput &input, const LAVOpenJocQueueTransactionOperations &operations,
    LAVOpenJocQueueTransactionResult *result);
