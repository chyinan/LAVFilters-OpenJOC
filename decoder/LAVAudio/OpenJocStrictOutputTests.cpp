/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "streams.h"
#include "OpenJocStrictOutput.h"
#include "OpenJocStrictNegotiation.h"
#include "growarray.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>

namespace
{
LAVOpenJocStrictMediaType ExpectedType(const LAVOpenJocOutputContract &contract)
{
    LAVOpenJocStrictMediaType expected{};
    expected.major_type = MEDIATYPE_Audio;
    expected.subtype = MEDIASUBTYPE_IEEE_FLOAT;
    expected.fixed_size_samples = TRUE;
    expected.temporal_compression = FALSE;
    expected.format_type = FORMAT_WaveFormatEx;
    expected.format_size = sizeof(WAVEFORMATEXTENSIBLE);
    expected.wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    expected.wave.Format.nChannels = static_cast<WORD>(contract.channel_count);
    expected.wave.Format.nSamplesPerSec = 48000;
    expected.wave.Format.nBlockAlign = static_cast<WORD>(contract.channel_count * sizeof(float));
    expected.wave.Format.nAvgBytesPerSec = 48000 * expected.wave.Format.nBlockAlign;
    expected.wave.Format.wBitsPerSample = 32;
    expected.wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    expected.wave.Samples.wValidBitsPerSample = 32;
    expected.wave.dwChannelMask = contract.windows_channel_mask;
    expected.wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    expected.sample_size = expected.wave.Format.nBlockAlign;
    return expected;
}

AM_MEDIA_TYPE AsMediaType(LAVOpenJocStrictMediaType &strict)
{
    AM_MEDIA_TYPE media{};
    media.majortype = strict.major_type;
    media.subtype = strict.subtype;
    media.bFixedSizeSamples = strict.fixed_size_samples;
    media.bTemporalCompression = strict.temporal_compression;
    media.lSampleSize = strict.sample_size;
    media.formattype = strict.format_type;
    media.pUnk = nullptr;
    media.cbFormat = strict.format_size;
    media.pbFormat = reinterpret_cast<BYTE *>(&strict.wave);
    return media;
}

void TestExactMediaTypes()
{
    for (std::uint32_t value = 0; value < LAV_OPENJOC_OUTPUT_CONTRACT_COUNT; ++value)
    {
        const auto *contract = FindLAVOpenJocOutputContract(static_cast<LAVOpenJocOutputPolicy>(value));
        assert(contract != nullptr);

        LAVOpenJocStrictMediaType actual{};
        assert(BuildLAVOpenJocStrictMediaType(*contract, &actual));
        const auto expected = ExpectedType(*contract);
        assert(std::memcmp(&actual, &expected, sizeof(actual)) == 0);
        assert(IsExactLAVOpenJocStrictMediaType(*contract, actual));
    }

    const auto *stereo = FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Stereo);
    LAVOpenJocStrictMediaType stereo_type{};
    assert(BuildLAVOpenJocStrictMediaType(*stereo, &stereo_type));
    assert(stereo_type.wave.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    assert(stereo_type.wave.Format.cbSize == 22);
}

void TestMediaTypeRejections()
{
    const auto *canonical = FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout514);
    assert(canonical != nullptr);

    LAVOpenJocOutputContract invalid = *canonical;
    invalid.windows_channel_mask = 0;
    LAVOpenJocStrictMediaType output{};
    assert(!BuildLAVOpenJocStrictMediaType(invalid, &output));

    invalid = *canonical;
    invalid.channel_count = 9;
    assert(!BuildLAVOpenJocStrictMediaType(invalid, &output));

    assert(BuildLAVOpenJocStrictMediaType(*canonical, &output));
    output.wave.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    assert(!IsExactLAVOpenJocStrictMediaType(*canonical, output));
    assert(BuildLAVOpenJocStrictMediaType(*canonical, &output));
    output.wave.Format.nSamplesPerSec = 44100;
    assert(!IsExactLAVOpenJocStrictMediaType(*canonical, output));
    assert(BuildLAVOpenJocStrictMediaType(*canonical, &output));
    output.wave.dwChannelMask ^= SPEAKER_TOP_BACK_RIGHT;
    assert(!IsExactLAVOpenJocStrictMediaType(*canonical, output));
}

void TestCompleteDirectShowMediaTypeComparison()
{
    const auto *contract = FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout714);
    LAVOpenJocStrictMediaType strict{};
    assert(contract && BuildLAVOpenJocStrictMediaType(*contract, &strict));
    AM_MEDIA_TYPE media = AsMediaType(strict);
    assert(IsExactLAVOpenJocStrictMediaType(*contract, media));

    const auto pristine = media;
    media.majortype = MEDIATYPE_Video;
    assert(!IsExactLAVOpenJocStrictMediaType(*contract, media));
    media = pristine;
    media.subtype = MEDIASUBTYPE_PCM;
    assert(!IsExactLAVOpenJocStrictMediaType(*contract, media));
    media = pristine;
    media.formattype = FORMAT_None;
    assert(!IsExactLAVOpenJocStrictMediaType(*contract, media));
    media = pristine;
    media.bFixedSizeSamples = FALSE;
    assert(!IsExactLAVOpenJocStrictMediaType(*contract, media));
    media = pristine;
    media.bTemporalCompression = TRUE;
    assert(!IsExactLAVOpenJocStrictMediaType(*contract, media));
    media = pristine;
    media.lSampleSize++;
    assert(!IsExactLAVOpenJocStrictMediaType(*contract, media));
    media = pristine;
    media.cbFormat--;
    assert(!IsExactLAVOpenJocStrictMediaType(*contract, media));
    media = pristine;
    media.pUnk = reinterpret_cast<IUnknown *>(1);
    assert(!IsExactLAVOpenJocStrictMediaType(*contract, media));
    media = pristine;
    strict.wave.Format.nAvgBytesPerSec++;
    assert(!IsExactLAVOpenJocStrictMediaType(*contract, media));
}

void TestStrictQueryAndSampleValidation()
{
    const auto *contract = FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout51);
    LAVOpenJocStrictMediaType strict{};
    assert(contract && BuildLAVOpenJocStrictMediaType(*contract, &strict));
    AM_MEDIA_TYPE attached = AsMediaType(strict);

    assert(NormalizeLAVOpenJocQueryAcceptResult(S_OK) == S_OK);
    assert(NormalizeLAVOpenJocQueryAcceptResult(S_FALSE) == VFW_E_TYPE_NOT_ACCEPTED);
    assert(NormalizeLAVOpenJocQueryAcceptResult(E_ACCESSDENIED) == E_ACCESSDENIED);
    assert(NormalizeLAVOpenJocEndOfStreamStep(true, E_ACCESSDENIED) == E_ACCESSDENIED);
    assert(NormalizeLAVOpenJocEndOfStreamStep(false, E_ACCESSDENIED) == S_OK);
    assert(ValidateLAVOpenJocDeliverySample(*contract, nullptr, 4096, 2048) == S_OK);
    assert(ValidateLAVOpenJocDeliverySample(*contract, &attached, 4096, 2048) == S_OK);

    attached.lSampleSize++;
    assert(ValidateLAVOpenJocDeliverySample(*contract, &attached, 4096, 2048) == VFW_E_TYPE_NOT_ACCEPTED);
    attached = AsMediaType(strict);
    assert(ValidateLAVOpenJocDeliverySample(*contract, &attached, 1024, 2048) == VFW_E_BUFFER_UNDERFLOW);
    LAVOpenJocOutputContract copied_contract = *contract;
    assert(ValidateLAVOpenJocDeliverySample(copied_contract, nullptr, 4096, 2048) == E_INVALIDARG);
}

struct FakeStrictDownstream
{
    int query_count = 0;
    int reconnect_count = 0;
    int acquire_count = 0;
    int attached_release_count = 0;
    int sample_release_count = 0;
    int sample_type_count = 0;
    int output_type_count = 0;
    int deliver_count = 0;
    HRESULT query_result = S_OK;
    HRESULT acquire_result = S_OK;
    long capacity = 4096;
    AM_MEDIA_TYPE *attached = nullptr;
    BYTE storage[4096]{};
};

LAVOpenJocStrictDeliveryOperations MakeStrictOperations(FakeStrictDownstream &fake)
{
    LAVOpenJocStrictDeliveryOperations operations;
    operations.query_accept = [&](const AM_MEDIA_TYPE &) {
        ++fake.query_count;
        return fake.query_result;
    };
    operations.reconnect = [&](long, const AM_MEDIA_TYPE &) {
        ++fake.reconnect_count;
        return S_OK;
    };
    operations.acquire_sample = [&](LAVOpenJocStrictAcquiredSample *sample) {
        ++fake.acquire_count;
        sample->handle = &fake;
        sample->data = fake.storage;
        sample->attached_type = fake.attached;
        sample->capacity = fake.capacity;
        return fake.acquire_result;
    };
    operations.release_attached_type = [&](AM_MEDIA_TYPE *) { ++fake.attached_release_count; };
    operations.release_sample = [&](void *) { ++fake.sample_release_count; };
    operations.set_sample_media_type = [&](void *, const AM_MEDIA_TYPE &) {
        ++fake.sample_type_count;
        return S_OK;
    };
    operations.set_output_media_type = [&](const AM_MEDIA_TYPE &) {
        ++fake.output_type_count;
        return S_OK;
    };
    operations.deliver = [&](void *, BYTE *, long) {
        ++fake.deliver_count;
        return S_OK;
    };
    return operations;
}

void TestStrictDeliveryOrchestration()
{
    const auto *contract = FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout714);
    LAVOpenJocStrictMediaType strict{};
    assert(contract && BuildLAVOpenJocStrictMediaType(*contract, &strict));
    AM_MEDIA_TYPE candidate = AsMediaType(strict);

    FakeStrictDownstream rejected;
    rejected.query_result = S_FALSE;
    auto operations = MakeStrictOperations(rejected);
    assert(DeliverLAVOpenJocStrictMediaType(contract, candidate, true, 2048, operations) ==
           VFW_E_TYPE_NOT_ACCEPTED);
    assert(rejected.query_count == 1);
    assert(rejected.reconnect_count == 0 && rejected.acquire_count == 0);
    assert(rejected.sample_type_count == 0 && rejected.output_type_count == 0 && rejected.deliver_count == 0);

    FakeStrictDownstream mismatched;
    AM_MEDIA_TYPE mismatched_attached = candidate;
    mismatched_attached.lSampleSize++;
    mismatched.attached = &mismatched_attached;
    operations = MakeStrictOperations(mismatched);
    assert(DeliverLAVOpenJocStrictMediaType(contract, candidate, true, 2048, operations) ==
           VFW_E_TYPE_NOT_ACCEPTED);
    assert(mismatched.attached_release_count == 1 && mismatched.sample_release_count == 1);
    assert(mismatched.sample_type_count == 0 && mismatched.output_type_count == 0 && mismatched.deliver_count == 0);

    FakeStrictDownstream undersized;
    undersized.capacity = 1024;
    operations = MakeStrictOperations(undersized);
    assert(DeliverLAVOpenJocStrictMediaType(contract, candidate, true, 2048, operations) ==
           VFW_E_BUFFER_UNDERFLOW);
    assert(undersized.sample_release_count == 1 && undersized.deliver_count == 0);

    FakeStrictDownstream acquire_failed_with_attached;
    acquire_failed_with_attached.acquire_result = E_ACCESSDENIED;
    acquire_failed_with_attached.attached = &candidate;
    operations = MakeStrictOperations(acquire_failed_with_attached);
    assert(DeliverLAVOpenJocStrictMediaType(contract, candidate, true, 2048, operations) == E_ACCESSDENIED);
    assert(acquire_failed_with_attached.attached_release_count == 1);
    assert(acquire_failed_with_attached.sample_release_count == 1);

    FakeStrictDownstream accepted;
    accepted.attached = &candidate;
    operations = MakeStrictOperations(accepted);
    assert(DeliverLAVOpenJocStrictMediaType(contract, candidate, true, 2048, operations) == S_OK);
    assert(accepted.query_count == 1 && accepted.reconnect_count == 1 && accepted.acquire_count == 1);
    assert(accepted.attached_release_count == 1);
    assert(accepted.sample_type_count == 1 && accepted.output_type_count == 1 && accepted.deliver_count == 1);
    assert(accepted.sample_release_count == 1);

    FakeStrictDownstream stock;
    operations = MakeStrictOperations(stock);
    assert(DeliverLAVOpenJocStrictMediaType(nullptr, candidate, true, 2048, operations) == S_FALSE);
    assert(stock.query_count == 0 && stock.reconnect_count == 0 && stock.acquire_count == 0 &&
           stock.deliver_count == 0);
}

void TestQueueTransactionOrchestration()
{
    struct FakeQueue
    {
        int flush = 0;
        int prepare = 0;
        int swap = 0;
        int append = 0;
        HRESULT flush_result = S_OK;
        HRESULT append_result = S_OK;
    };
    auto operations_for = [](FakeQueue &fake) {
        LAVOpenJocQueueTransactionOperations operations;
        operations.flush = [&]() { ++fake.flush; return fake.flush_result; };
        operations.prepare_metadata = [&]() { ++fake.prepare; return S_OK; };
        operations.swap_buffer = [&]() { ++fake.swap; };
        operations.append_buffer = [&]() { ++fake.append; return fake.append_result; };
        return operations;
    };

    LAVOpenJocQueueTransactionInput input{true, 10, 5, 100, 200, 48000};
    LAVOpenJocQueueTransactionResult result{77, 88};
    FakeQueue same;
    auto operations = operations_for(same);
    assert(ExecuteLAVOpenJocQueueTransaction(input, operations, &result) == S_OK);
    assert(same.flush == 0 && same.prepare == 0 && same.append == 1 && same.swap == 0);
    assert(result.sample_count == 15 && result.start_time == 100);

    FakeQueue transition;
    input.compatible = false;
    result = {77, 88};
    operations = operations_for(transition);
    assert(ExecuteLAVOpenJocQueueTransaction(input, operations, &result) == S_OK);
    assert(transition.flush == 1 && transition.prepare == 1 && transition.swap == 1 && transition.append == 0);
    assert(result.sample_count == 5 && result.start_time == 200);

    FakeQueue flush_failed;
    flush_failed.flush_result = E_ACCESSDENIED;
    result = {77, 88};
    operations = operations_for(flush_failed);
    assert(ExecuteLAVOpenJocQueueTransaction(input, operations, &result) == E_ACCESSDENIED);
    assert(flush_failed.prepare == 0 && flush_failed.swap == 0 && flush_failed.append == 0);
    assert(result.sample_count == 77 && result.start_time == 88);

    FakeQueue append_failed;
    append_failed.append_result = E_OUTOFMEMORY;
    input.compatible = true;
    result = {77, 88};
    operations = operations_for(append_failed);
    assert(ExecuteLAVOpenJocQueueTransaction(input, operations, &result) == E_OUTOFMEMORY);
    assert(append_failed.append == 1 && append_failed.flush == 0 && append_failed.prepare == 0);
    assert(result.sample_count == 77 && result.start_time == 88);
}

void TestCheckedArithmetic()
{
    std::uint32_t dword = 0;
    std::size_t bytes = 0;
    long narrowed = 0;
    long grown = 0;

    assert(CheckedLAVOpenJocSampleAdd(10, 20, &dword) && dword == 30);
    assert(!CheckedLAVOpenJocSampleAdd((std::numeric_limits<std::uint32_t>::max)(), 1, &dword));
    assert(CheckedLAVOpenJocPcmByteCount(256, 48, &bytes) && bytes == 12288);
    assert(!CheckedLAVOpenJocPcmByteCount((std::numeric_limits<std::size_t>::max)(), 48, &bytes));
    assert(CheckedLAVOpenJocLongNarrow(12288, &narrowed) && narrowed == 12288);
    assert(!CheckedLAVOpenJocLongNarrow(static_cast<std::size_t>((std::numeric_limits<long>::max)()) + 1u,
                                       &narrowed));
    assert(CheckedLAVOpenJocAllocatorGrowth(100, &grown) && grown == 150);
    assert(!CheckedLAVOpenJocAllocatorGrowth((std::numeric_limits<long>::max)(), &grown));
}

class FailingGrowableArray final : public GrowableArray<BYTE>
{
  public:
    void FailAllocations() noexcept { fail_allocations_ = true; }
    void ForceCountForOverflow(DWORD value) noexcept
    {
        m_count = value;
        m_allocated = value;
    }

  protected:
    void *ReallocateMemory(void *memory, std::size_t bytes) override
    {
        return fail_allocations_ ? nullptr : std::realloc(memory, bytes);
    }

  private:
    bool fail_allocations_ = false;
};

void TestGrowableArrayFailuresPreserveState()
{
    FailingGrowableArray bytes;
    const BYTE original[] = {1, 2, 3, 4};
    assert(bytes.Append(original, sizeof(original)) == S_OK);
    BYTE *const old_pointer = bytes.Ptr();
    bytes.FailAllocations();

    const BYTE addition[] = {5, 6, 7, 8, 9};
    assert(bytes.Append(addition, sizeof(addition)) == E_OUTOFMEMORY);
    assert(bytes.Ptr() == old_pointer);
    assert(bytes.GetCount() == sizeof(original));
    assert(std::memcmp(bytes.Ptr(), original, sizeof(original)) == 0);
    assert(bytes.AppendZero(5) == E_OUTOFMEMORY);
    assert(bytes.Ptr() == old_pointer && bytes.GetCount() == sizeof(original));
    assert(std::memcmp(bytes.Ptr(), original, sizeof(original)) == 0);

    FailingGrowableArray overflow;
    overflow.ForceCountForOverflow((std::numeric_limits<DWORD>::max)());
    assert(overflow.Append(original, 1) == HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));
    assert(overflow.AppendZero(1) == HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));
    assert(overflow.GetCount() == (std::numeric_limits<DWORD>::max)());

    FailingGrowableArray alias;
    assert(alias.Append(original, sizeof(original)) == S_OK);
    const BYTE alias_snapshot[] = {1, 2, 3, 4};
    assert(alias.Append(&alias) == E_INVALIDARG);
    assert(alias.Append(alias.Ptr() + 1, 2) == E_INVALIDARG);
    assert(alias.GetCount() == sizeof(alias_snapshot));
    assert(std::memcmp(alias.Ptr(), alias_snapshot, sizeof(alias_snapshot)) == 0);

    GrowableArray<std::uint32_t> words;
    const std::uint32_t word_values[] = {0x11223344u, 0xaabbccddu};
    assert(words.Append(word_values, 2) == S_OK);
    words.Consume(1);
    assert(words.GetCount() == 1 && words[0] == word_values[1]);
    GrowableArray<std::uint32_t> *null_words = nullptr;
    assert(words.Append(null_words) == E_POINTER);
}

void TestStrictBufferIdentityAndValidation()
{
    const auto *layout71 = FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout71);
    const auto *layout512 = FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout512);
    assert(layout71 && layout512 && layout71->channel_count == layout512->channel_count);
    assert(AreLAVOpenJocBufferContractsCompatible(layout71, layout71));
    assert(!AreLAVOpenJocBufferContractsCompatible(layout71, layout512));
    assert(!AreLAVOpenJocBufferContractsCompatible(layout71, nullptr));
    assert(!AreLAVOpenJocBufferContractsCompatible(nullptr, layout512));
    assert(AreLAVOpenJocBufferContractsCompatible(nullptr, nullptr));

    AVChannelLayout layout{};
    assert(BuildOpenJocAvChannelLayout(*layout512, &layout));
    const std::size_t bytes = 256u * layout512->channel_count * sizeof(float);
    assert(ValidateLAVOpenJocStrictBuffer(layout512, true, 48000, false, layout, 256, bytes));
    assert(!ValidateLAVOpenJocStrictBuffer(layout71, true, 48000, false, layout, 256, bytes));
    assert(!ValidateLAVOpenJocStrictBuffer(layout512, false, 48000, false, layout, 256, bytes));
    assert(!ValidateLAVOpenJocStrictBuffer(layout512, true, 44100, false, layout, 256, bytes));
    assert(!ValidateLAVOpenJocStrictBuffer(layout512, true, 48000, true, layout, 256, bytes));
    assert(!ValidateLAVOpenJocStrictBuffer(layout512, true, 48000, false, layout, 256, bytes - 1));
    av_channel_layout_uninit(&layout);
}

std::string ReadSource(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    assert(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void TestStrictIdentityIsIntegratedBeforeStockPostprocessing()
{
    const std::filesystem::path source_dir = std::filesystem::path(__FILE__).parent_path();
    const std::string header = ReadSource(source_dir / "LAVAudio.h");
    const std::string lav_audio = ReadSource(source_dir / "LAVAudio.cpp");
    const std::string postprocessor = ReadSource(source_dir / "PostProcessor.cpp");

    assert(header.find("const LAVOpenJocOutputContract *openjoc_contract") != std::string::npos);
    assert(lav_audio.find("out.openjoc_contract = contract") != std::string::npos);
    assert(lav_audio.find("AreLAVOpenJocBufferContractsCompatible") != std::string::npos);
    assert(lav_audio.find("m_OutputQueue.openjoc_contract = nullptr") != std::string::npos);

    const auto decode = lav_audio.find("HRESULT CLAVAudio::DecodeOpenJoc(HRESULT *hrDeliver)");
    const auto decode_layout_copy = lav_audio.find("av_channel_layout_copy(&m_DecodeLayout, &out.layout)", decode);
    const auto marker_commit = lav_audio.find("out.openjoc_contract = contract", decode);
    const auto postprocess = lav_audio.find("PostProcess(&out)", decode);
    assert(decode_layout_copy < marker_commit && marker_commit < postprocess);

    const auto strict_check = postprocessor.find("ValidateLAVOpenJocStrictBuffer");
    const auto stock_validation = postprocessor.find("// Validate channel mask");
    assert(strict_check != std::string::npos && strict_check < stock_validation);
    assert(postprocessor.find("return ValidateLAVOpenJocStrictBuffer", strict_check - 16) != std::string::npos);
}

void TestStrictDeliveryPrecedesSampleAndStockFallbacks()
{
    const std::filesystem::path source_dir = std::filesystem::path(__FILE__).parent_path();
    const std::string source = ReadSource(source_dir / "LAVAudio.cpp");
    const auto deliver = source.find("HRESULT CLAVAudio::Deliver(BufferDetails &buffer)");
    const auto strict_marker = source.find("const bool strict_openjoc = buffer.openjoc_contract != nullptr", deliver);
    const auto strict_branch = source.find("if (strict_openjoc)", strict_marker);
    const auto orchestration = source.find("return DeliverLAVOpenJocStrictMediaType", strict_branch);
    const auto stock_fallback = source.find("retry_qa:", strict_branch);
    assert(deliver != std::string::npos && strict_marker != std::string::npos && strict_branch != std::string::npos);
    assert(orchestration != std::string::npos && stock_fallback != std::string::npos && orchestration < stock_fallback);

    const auto eos = source.find("HRESULT CLAVAudio::EndOfStream()");
    const auto receive = source.find("HRESULT CLAVAudio::Receive(IMediaSample *pIn)");
    assert(source.find("strict_eos", eos) < receive);
    assert(source.find("first_process_hr", eos) < receive);
    assert(source.find("eof_process_hr", eos) < receive);
    assert(source.find("strict_resync", receive) != std::string::npos);

    const auto queue = source.find("HRESULT CLAVAudio::QueueOutput(BufferDetails &buffer)");
    const auto transaction = source.find("ExecuteLAVOpenJocQueueTransaction", queue);
    const auto timestamp_commit = source.find("m_OutputQueue.rtStart = result.start_time", queue);
    assert(transaction != std::string::npos && timestamp_commit != std::string::npos && transaction < timestamp_commit);
}
} // namespace

int main()
{
    TestExactMediaTypes();
    TestMediaTypeRejections();
    TestCompleteDirectShowMediaTypeComparison();
    TestStrictQueryAndSampleValidation();
    TestStrictDeliveryOrchestration();
    TestQueueTransactionOrchestration();
    TestCheckedArithmetic();
    TestGrowableArrayFailuresPreserveState();
    TestStrictBufferIdentityAndValidation();
    TestStrictIdentityIsIntegratedBeforeStockPostprocessing();
    TestStrictDeliveryPrecedesSampleAndStockFallbacks();
    return 0;
}
