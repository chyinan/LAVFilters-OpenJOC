/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#include "stdafx.h"
#include "OpenJocDecoder.h"
#include "OpenJocDialnorm.h"

#include <array>
#include <deque>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(LAV_ENABLE_OPENJOC)
#include "openjoc.h"
#endif

namespace
{
constexpr std::int64_t kNoPts = std::numeric_limits<std::int64_t>::lowest();

#if defined(LAV_ENABLE_OPENJOC)
template <typename Function>
bool LoadOpenJocSymbol(HMODULE module, const char *name, Function &function)
{
    function = reinterpret_cast<Function>(GetProcAddress(module, name));
    return function != nullptr;
}

HMODULE LoadOpenJocModule()
{
    HMODULE filter_module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LoadOpenJocModule), &filter_module) != 0)
    {
        wchar_t module_path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(filter_module, module_path, ARRAYSIZE(module_path));
        if (length > 0 && length < ARRAYSIZE(module_path))
        {
            std::wstring path(module_path, length);
            const std::size_t separator = path.find_last_of(L"\\/");
            if (separator != std::wstring::npos)
            {
                path.resize(separator + 1);
                path += L"openjoc_capi.dll";
                if (HMODULE module = LoadLibraryW(path.c_str()))
                    return module;
            }
        }
    }

    return LoadLibraryW(L"openjoc_capi.dll");
}
#endif
} // namespace

struct LAVOpenJocDecoder::Impl
{
    LAVOpenJocAdmission admission;
    std::int64_t admission_pts_samples = kNoPts;
    std::size_t classifier_input_bytes = 0;
    std::size_t stream_input_bytes = 0;
    std::string last_error;
    LAVOpenJocDiagnosticSnapshot diagnostic;
    bool available = false;
    const LAVOpenJocOutputContract *output_contract =
        FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Stereo);
    LAVOpenJocDialnormPolicy dialnorm_policy = LAVOpenJocDialnormPolicy::Calibrated;

#if defined(LAV_OPENJOC_TESTING)
    bool fail_next_classifier_create = false;
    bool fail_next_decoder_create = false;
    bool fail_next_classifier_reset = false;
#endif

#if defined(LAV_ENABLE_OPENJOC)
    struct Api
    {
        HMODULE module = nullptr;

        std::uint32_t (*get_abi_version)() = nullptr;
        openjoc_status (*decoder_config_init_v1_4)(openjoc_decoder_config *) = nullptr;
        openjoc_status (*stream_decoder_create)(const openjoc_decoder_config *, openjoc_stream_decoder **) = nullptr;
        void (*stream_decoder_destroy)(openjoc_stream_decoder *) = nullptr;
        openjoc_status (*stream_decoder_send_chunk)(openjoc_stream_decoder *, const std::uint8_t *, std::size_t,
                                                    std::int64_t, std::uint32_t) = nullptr;
        openjoc_status (*stream_decoder_receive_frame)(openjoc_stream_decoder *, openjoc_pcm_frame *) = nullptr;
        openjoc_status (*stream_decoder_drain)(openjoc_stream_decoder *) = nullptr;
        openjoc_status (*stream_decoder_reset)(openjoc_stream_decoder *) = nullptr;
        const char *(*stream_decoder_last_error)(const openjoc_stream_decoder *) = nullptr;
        const char *(*stream_decoder_get_channel_label)(const openjoc_stream_decoder *, std::size_t) = nullptr;
#if defined(LAV_OPENJOC_TESTING)
        const char *(*stream_decoder_get_config_descriptor)(const openjoc_stream_decoder *) = nullptr;
#endif
        openjoc_status (*pcm_frame_init)(openjoc_pcm_frame *) = nullptr;
        openjoc_status (*classifier_create)(openjoc_classifier **) = nullptr;
        void (*classifier_destroy)(openjoc_classifier *) = nullptr;
        openjoc_status (*classifier_send_chunk)(openjoc_classifier *, const std::uint8_t *, std::size_t,
                                                 openjoc_classification *) = nullptr;
        openjoc_status (*classifier_finish)(openjoc_classifier *, openjoc_classification *) = nullptr;
        openjoc_status (*classifier_reset)(openjoc_classifier *) = nullptr;
        const char *(*classifier_last_error)(const openjoc_classifier *) = nullptr;

        bool Load()
        {
            module = LoadOpenJocModule();
            if (!module)
                return false;

            if (!LoadOpenJocSymbol(module, "openjoc_get_abi_version", get_abi_version) ||
                !LoadOpenJocSymbol(module, "openjoc_decoder_config_init_v1_4", decoder_config_init_v1_4) ||
                !LoadOpenJocSymbol(module, "openjoc_stream_decoder_create", stream_decoder_create) ||
                !LoadOpenJocSymbol(module, "openjoc_stream_decoder_destroy", stream_decoder_destroy) ||
                !LoadOpenJocSymbol(module, "openjoc_stream_decoder_send_chunk", stream_decoder_send_chunk) ||
                !LoadOpenJocSymbol(module, "openjoc_stream_decoder_receive_frame", stream_decoder_receive_frame) ||
                !LoadOpenJocSymbol(module, "openjoc_stream_decoder_drain", stream_decoder_drain) ||
                !LoadOpenJocSymbol(module, "openjoc_stream_decoder_reset", stream_decoder_reset) ||
                !LoadOpenJocSymbol(module, "openjoc_stream_decoder_last_error", stream_decoder_last_error) ||
                !LoadOpenJocSymbol(module, "openjoc_stream_decoder_get_channel_label",
                                   stream_decoder_get_channel_label) ||
#if defined(LAV_OPENJOC_TESTING)
                !LoadOpenJocSymbol(module, "openjoc_stream_decoder_get_config_descriptor",
                                   stream_decoder_get_config_descriptor) ||
#endif
                !LoadOpenJocSymbol(module, "openjoc_pcm_frame_init", pcm_frame_init) ||
                !LoadOpenJocSymbol(module, "openjoc_classifier_create", classifier_create) ||
                !LoadOpenJocSymbol(module, "openjoc_classifier_destroy", classifier_destroy) ||
                !LoadOpenJocSymbol(module, "openjoc_classifier_send_chunk", classifier_send_chunk) ||
                !LoadOpenJocSymbol(module, "openjoc_classifier_finish", classifier_finish) ||
                !LoadOpenJocSymbol(module, "openjoc_classifier_reset", classifier_reset) ||
                !LoadOpenJocSymbol(module, "openjoc_classifier_last_error", classifier_last_error))
            {
                FreeLibrary(module);
                module = nullptr;
                return false;
            }

            const std::uint32_t abi_version = get_abi_version();
            if ((abi_version >> 16) != OPENJOC_ABI_VERSION_MAJOR ||
                (abi_version & 0xffffu) < OPENJOC_ABI_VERSION_MINOR)
            {
                FreeLibrary(module);
                module = nullptr;
                return false;
            }

            return true;
        }

        void Unload()
        {
            if (module)
            {
                FreeLibrary(module);
                module = nullptr;
            }
        }
    } api;

    openjoc_classifier *classifier = nullptr;
    openjoc_stream_decoder *decoder = nullptr;
    std::deque<LAVOpenJocFrame> pending_frames;
#endif

    ~Impl()
    {
#if defined(LAV_ENABLE_OPENJOC)
        if (decoder)
            api.stream_decoder_destroy(decoder);
        if (classifier)
            api.classifier_destroy(classifier);
        api.Unload();
#endif
    }

    void SetError(const char *message)
    {
        last_error = message ? message : "OpenJOC returned an unknown error";
    }

    void ClearDiagnostic()
    {
        diagnostic = {};
    }

    void RecordProbeFailure(const LAVOpenJocClassification classification,
                            const bool classifier_call_failed)
    {
        const LAVOpenJocFailureReason reason =
            ClassifyLAVOpenJocProbeFailure(classification, classifier_call_failed,
                                            last_error.c_str());
        diagnostic = MakeLAVOpenJocFallbackDiagnostic(reason, last_error.c_str(), true, 0);
    }

    void RecordRuntimeDiagnostic(const LAVOpenJocFailureReason reason, const char *detail)
    {
        diagnostic = MakeLAVOpenJocRuntimeDiagnostic(reason, detail);
    }

#if defined(LAV_ENABLE_OPENJOC)
    void SetApiError(const char *message)
    {
        SetError(message);
    }

    void SetClassifierError()
    {
        SetError(api.classifier_last_error && classifier ? api.classifier_last_error(classifier) : nullptr);
    }

    void SetDecoderError()
    {
        SetError(api.stream_decoder_last_error && decoder ? api.stream_decoder_last_error(decoder) : nullptr);
    }

    bool CreateClassifier(openjoc_classifier **output_classifier)
    {
        if (!output_classifier)
        {
            SetApiError("invalid OpenJOC classifier output");
            return false;
        }
        *output_classifier = nullptr;
#if defined(LAV_OPENJOC_TESTING)
        if (fail_next_classifier_create)
        {
            fail_next_classifier_create = false;
            SetApiError("failed to create OpenJOC classifier (injected)");
            return false;
        }
#endif

        openjoc_classifier *candidate = nullptr;
        if (!api.classifier_create || api.classifier_create(&candidate) != OPENJOC_STATUS_OK || !candidate)
        {
            if (candidate && api.classifier_destroy)
                api.classifier_destroy(candidate);
            SetApiError("failed to create OpenJOC classifier");
            return false;
        }
        *output_classifier = candidate;
        return true;
    }

    bool CreateDecoderForContract(const LAVOpenJocOutputContract *contract,
                                  const LAVOpenJocDialnormPolicy dialnorm_policy,
                                  openjoc_stream_decoder **output_decoder)
    {
        if (!output_decoder)
        {
            SetApiError("invalid OpenJOC decoder output");
            return false;
        }
        *output_decoder = nullptr;
#if defined(LAV_OPENJOC_TESTING)
        if (fail_next_decoder_create)
        {
            fail_next_decoder_create = false;
            SetApiError("failed to create OpenJOC decoder (injected)");
            return false;
        }
#endif

        openjoc_decoder_config config{};
        if (!api.decoder_config_init_v1_4 || api.decoder_config_init_v1_4(&config) != OPENJOC_STATUS_OK)
        {
            SetApiError("failed to initialize OpenJOC decoder configuration");
            return false;
        }
        if (!contract)
        {
            SetApiError("invalid OpenJOC output contract");
            return false;
        }
        if (contract->policy == LAVOpenJocOutputPolicy::Stereo)
        {
            config.render_mode = OPENJOC_RENDER_STEREO;
            config.speaker_layout = nullptr;
        }
        else if (contract->policy == LAVOpenJocOutputPolicy::Binaural)
        {
            config.render_mode = OPENJOC_RENDER_BINAURAL;
            config.speaker_layout = contract->abi_preset_name;
            config.virtual_layout = nullptr;
            config.sofa_data = nullptr;
            config.sofa_size = 0;
            config.lfe_policy = OPENJOC_LFE_EXCLUDE;
        }
        else
        {
            if (!contract->abi_preset_name)
            {
                SetApiError("missing OpenJOC speaker preset name");
                return false;
            }
            config.render_mode = OPENJOC_RENDER_SPEAKER;
            config.speaker_layout = contract->abi_preset_name;
        }
        std::uint32_t dialnorm_mode = 0;
        if (!TryMapLAVOpenJocDialnormPolicy(dialnorm_policy, &dialnorm_mode))
        {
            SetApiError("invalid OpenJOC dialnorm policy");
            return false;
        }
        config.dialnorm_mode = dialnorm_mode;

        openjoc_stream_decoder *candidate = nullptr;
        if (!api.stream_decoder_create)
        {
            SetApiError("missing OpenJOC decoder factory");
            return false;
        }
        const openjoc_status status = api.stream_decoder_create(&config, &candidate);
        if (status != OPENJOC_STATUS_OK || !candidate)
        {
            if (candidate && api.stream_decoder_destroy)
                api.stream_decoder_destroy(candidate);
            SetApiError("failed to create OpenJOC decoder");
            return false;
        }
        *output_decoder = candidate;
        return true;
    }

    bool CreateDecoder()
    {
        return decoder || CreateDecoderForContract(output_contract, dialnorm_policy, &decoder);
    }

    bool ValidateAndCopyFrame(const openjoc_pcm_frame &pcm, LAVOpenJocFrame &output)
    {
        if (!decoder || !output_contract || !pcm.data || !api.stream_decoder_get_channel_label ||
            pcm.channel_label_count != output_contract->channel_count)
        {
            SetApiError("invalid OpenJOC PCM frame metadata");
            return false;
        }

        std::array<const char *, 12> labels{};
        for (std::uint32_t index = 0; index < output_contract->channel_count; ++index)
            labels[index] = api.stream_decoder_get_channel_label(decoder, index);

        std::size_t element_count = 0;
        std::size_t byte_count = 0;
        if (!ValidateLAVOpenJocFrameMetadata(
                *output_contract, pcm.sample_format, pcm.sample_rate, pcm.channel_count, pcm.sample_count,
                pcm.data_len, pcm.layout_name, labels.data(), pcm.channel_label_count, &element_count, &byte_count))
        {
            SetApiError("invalid OpenJOC PCM frame contract");
            return false;
        }
        (void)byte_count;

        LAVOpenJocFrame validated;
        try
        {
            validated.samples.assign(pcm.data, pcm.data + element_count);
        }
        catch (const std::bad_alloc &)
        {
            SetApiError("failed to allocate OpenJOC PCM frame");
            return false;
        }
        catch (const std::length_error &)
        {
            SetApiError("OpenJOC PCM frame exceeds vector capacity");
            return false;
        }
        validated.sample_rate = pcm.sample_rate;
        validated.channel_count = pcm.channel_count;
        validated.sample_count = pcm.sample_count;
        validated.pts_samples = pcm.pts_samples;
        validated.output_contract = output_contract;
        output = std::move(validated);
        return true;
    }

    static LAVOpenJocClassification ToClassification(const openjoc_classification classification)
    {
        switch (classification)
        {
        case OPENJOC_CLASSIFICATION_CONFIRMED_JOC:
            return LAVOpenJocClassification::ConfirmedJoc;
        case OPENJOC_CLASSIFICATION_CONFIRMED_NON_JOC:
            return LAVOpenJocClassification::ConfirmedNonJoc;
        case OPENJOC_CLASSIFICATION_INVALID_OR_UNSUPPORTED:
            return LAVOpenJocClassification::InvalidOrUnsupported;
        default:
            return LAVOpenJocClassification::Unknown;
        }
    }

    LAVOpenJocClassification Classify(const unsigned char *data, const std::size_t data_size,
                                      const bool end_of_stream, bool *call_failed)
    {
        if (call_failed)
            *call_failed = false;
        openjoc_classification classification = OPENJOC_CLASSIFICATION_UNKNOWN;
        const openjoc_status status = end_of_stream
                                          ? api.classifier_finish(classifier, &classification)
                                          : api.classifier_send_chunk(classifier, data, data_size, &classification);
        if (status != OPENJOC_STATUS_OK)
        {
            if (call_failed)
                *call_failed = true;
            SetClassifierError();
            return LAVOpenJocClassification::InvalidOrUnsupported;
        }

        if (!end_of_stream)
            classifier_input_bytes += data_size;
        return ToClassification(classification);
    }

    bool FeedDecoder(const unsigned char *data, const std::size_t data_size, const std::int64_t pts_samples)
    {
        if (data_size == 0)
            return true;
        if (!data || !CreateDecoder())
            return false;

        const unsigned char *next = data;
        std::size_t remaining = data_size;
        bool first_chunk = true;
        const std::int64_t stream_anchor_pts = stream_input_bytes == 0 ? pts_samples : kNoPts;
        constexpr std::size_t kDecoderInputChunkBytes = 6144;
        while (remaining > 0)
        {
            const std::size_t chunk_size = remaining > kDecoderInputChunkBytes ? kDecoderInputChunkBytes : remaining;
            const openjoc_status status = api.stream_decoder_send_chunk(
                decoder, next, chunk_size, first_chunk ? stream_anchor_pts : kNoPts, 0);
            if (status == OPENJOC_STATUS_OUTPUT_PENDING)
            {
                if (!CollectFrames())
                    return false;
                continue;
            }
            if (status != OPENJOC_STATUS_OK && status != OPENJOC_STATUS_NEED_MORE_INPUT &&
                status != OPENJOC_STATUS_FRAME_AVAILABLE)
            {
                SetDecoderError();
                return false;
            }
            stream_input_bytes += chunk_size;
            next += chunk_size;
            remaining -= chunk_size;
            first_chunk = false;
            if (status == OPENJOC_STATUS_FRAME_AVAILABLE && !CollectFrames())
                return false;
        }
        return true;
    }

    bool CollectFrames()
    {
        for (;;)
        {
            openjoc_pcm_frame pcm{};
            if (api.pcm_frame_init(&pcm) != OPENJOC_STATUS_OK)
            {
                SetDecoderError();
                return false;
            }

            const openjoc_status status = api.stream_decoder_receive_frame(decoder, &pcm);
            if (status == OPENJOC_STATUS_NEED_MORE_INPUT || status == OPENJOC_STATUS_END_OF_STREAM)
                return true;
            if (status != OPENJOC_STATUS_FRAME_AVAILABLE)
            {
                SetDecoderError();
                return false;
            }

            LAVOpenJocFrame frame;
            if (!ValidateAndCopyFrame(pcm, frame))
                return false;
            pending_frames.push_back(std::move(frame));
        }
    }

    bool FinishDecoder()
    {
        if (!CreateDecoder())
            return false;

        for (;;)
        {
            const openjoc_status status = api.stream_decoder_drain(decoder);
            if (status == OPENJOC_STATUS_OUTPUT_PENDING || status == OPENJOC_STATUS_FRAME_AVAILABLE)
            {
                if (!CollectFrames())
                    return false;
                continue;
            }
            if (status != OPENJOC_STATUS_OK && status != OPENJOC_STATUS_NEED_MORE_INPUT &&
                status != OPENJOC_STATUS_END_OF_STREAM)
            {
                SetDecoderError();
                return false;
            }
            return CollectFrames();
        }
    }
#endif
};

LAVOpenJocDecoder::LAVOpenJocDecoder() : m_impl(std::make_unique<Impl>())
{
#if defined(LAV_ENABLE_OPENJOC)
    if (m_impl->api.Load() && m_impl->CreateClassifier(&m_impl->classifier))
        m_impl->available = true;
#endif
}

LAVOpenJocDecoder::~LAVOpenJocDecoder() = default;

bool LAVOpenJocDecoder::IsAvailable() const
{
    return m_impl->available;
}

LAVOpenJocState LAVOpenJocDecoder::State() const
{
    return m_impl->admission.state();
}

bool LAVOpenJocDecoder::SetOutputPolicy(const LAVOpenJocOutputPolicy policy)
{
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    if (!contract)
        return false;
    return SetConfiguration(contract, m_impl->dialnorm_policy);
}

bool LAVOpenJocDecoder::SetDialnormPolicy(const LAVOpenJocDialnormPolicy policy)
{
    if (!IsLAVOpenJocDialnormPolicy(policy))
        return false;
    return SetConfiguration(m_impl->output_contract, policy);
}

bool LAVOpenJocDecoder::SetConfiguration(const LAVOpenJocOutputContract *const contract,
                                         const LAVOpenJocDialnormPolicy dialnorm_policy)
{
    if (!contract || !IsLAVOpenJocDialnormPolicy(dialnorm_policy))
        return false;
    if (contract == m_impl->output_contract && dialnorm_policy == m_impl->dialnorm_policy)
        return true;

#if defined(LAV_ENABLE_OPENJOC)
    m_impl->last_error.clear();
    openjoc_classifier *next_classifier = nullptr;
    if (!m_impl->CreateClassifier(&next_classifier))
        return false;

    openjoc_stream_decoder *next_decoder = nullptr;
    if (!m_impl->CreateDecoderForContract(contract, dialnorm_policy, &next_decoder))
    {
        m_impl->api.classifier_destroy(next_classifier);
        return false;
    }

    openjoc_classifier *old_classifier = m_impl->classifier;
    openjoc_stream_decoder *old_decoder = m_impl->decoder;
    m_impl->classifier = next_classifier;
    m_impl->decoder = next_decoder;
    m_impl->output_contract = contract;
    m_impl->dialnorm_policy = dialnorm_policy;
    m_impl->available = true;
    if (old_decoder)
        m_impl->api.stream_decoder_destroy(old_decoder);
    if (old_classifier)
        m_impl->api.classifier_destroy(old_classifier);
    m_impl->pending_frames.clear();
#else
    m_impl->output_contract = contract;
    m_impl->dialnorm_policy = dialnorm_policy;
#endif
    m_impl->admission.reset();
    m_impl->admission_pts_samples = kNoPts;
    m_impl->classifier_input_bytes = 0;
    m_impl->stream_input_bytes = 0;
    m_impl->last_error.clear();
    m_impl->ClearDiagnostic();
    return true;
}

const LAVOpenJocOutputContract *LAVOpenJocDecoder::OutputContract() const
{
    return m_impl->output_contract;
}

LAVOpenJocDialnormPolicy LAVOpenJocDecoder::DialnormPolicy() const
{
    return m_impl->dialnorm_policy;
}

LAVOpenJocProcessResult LAVOpenJocDecoder::Process(const unsigned char *data, const std::size_t data_size,
                                                   const std::int64_t pts_samples, const bool end_of_stream)
{
    if (!m_impl->available)
    {
        m_impl->admission.resolve(LAVOpenJocClassification::ConfirmedNonJoc, 0);
        return LAVOpenJocProcessResult::UseStockDecoder;
    }

    if (data_size > 0 && !data)
    {
        m_impl->SetError("invalid null OpenJOC input buffer");
        m_impl->admission.resolve(LAVOpenJocClassification::InvalidOrUnsupported, data_size);
        m_impl->RecordProbeFailure(LAVOpenJocClassification::InvalidOrUnsupported, true);
        return LAVOpenJocProcessResult::UseStockDecoder;
    }

    if (m_impl->admission.state() == LAVOpenJocState::Undecided)
    {
#if defined(LAV_ENABLE_OPENJOC)
        if (m_impl->admission.classified_bytes() == 0 && data_size > 0)
            m_impl->admission_pts_samples = pts_samples;

        LAVOpenJocClassification classification = LAVOpenJocClassification::Unknown;
        bool classifier_call_failed = false;
        const std::size_t classified_bytes = m_impl->admission.classified_bytes();
        const std::size_t classification_offset =
            m_impl->admission.classification_offset(data_size);
        const std::size_t classification_budget =
            classified_bytes < LAVOpenJocAdmission::MaxRetainedBytes
                ? LAVOpenJocAdmission::MaxRetainedBytes - classified_bytes
                : 0;
        const std::size_t classification_input_size =
            data_size > classification_offset
                ? ((data_size - classification_offset) < classification_budget
                       ? data_size - classification_offset
                       : classification_budget)
                : 0;

        if (classification_input_size > 0)
        {
            classification = m_impl->Classify(data + classification_offset, classification_input_size, false,
                                               &classifier_call_failed);
            m_impl->admission.note_classified(classification_offset + classification_input_size);
        }
        if (end_of_stream)
        {
            bool finish_call_failed = false;
            classification = m_impl->Classify(nullptr, 0, true, &finish_call_failed);
            classifier_call_failed = classifier_call_failed || finish_call_failed;
        }

        if (classification == LAVOpenJocClassification::ConfirmedJoc ||
            classification == LAVOpenJocClassification::ConfirmedNonJoc)
            m_impl->ClearDiagnostic();
        else if (classification == LAVOpenJocClassification::InvalidOrUnsupported)
            m_impl->RecordProbeFailure(classification, classifier_call_failed);

        LAVOpenJocAdmissionAction action = m_impl->admission.resolve(
            classification, m_impl->admission.classified_bytes());
        if (action.kind == LAVOpenJocActionKind::UseStockDecoder)
            return LAVOpenJocProcessResult::UseStockDecoder;
        if (action.kind == LAVOpenJocActionKind::NoAction)
        {
            if (end_of_stream)
            {
                m_impl->RecordProbeFailure(LAVOpenJocClassification::InvalidOrUnsupported, false);
                m_impl->admission.resolve(LAVOpenJocClassification::InvalidOrUnsupported,
                                           m_impl->admission.classified_bytes());
                return LAVOpenJocProcessResult::UseStockDecoder;
            }
            return LAVOpenJocProcessResult::Waiting;
        }

        if (!m_impl->FeedDecoder(data, data_size, m_impl->admission_pts_samples))
        {
            m_impl->RecordRuntimeDiagnostic(LAVOpenJocFailureReason::OpenJocDecodeError,
                                             m_impl->last_error.c_str());
            return LAVOpenJocProcessResult::Error;
        }
#else
        return LAVOpenJocProcessResult::UseStockDecoder;
#endif
    }
    else if (m_impl->admission.state() == LAVOpenJocState::StockCodec ||
             m_impl->admission.state() == LAVOpenJocState::StockAfterOpenJocFailure)
    {
        return LAVOpenJocProcessResult::UseStockDecoder;
    }
    else
    {
#if defined(LAV_ENABLE_OPENJOC)
        if (!m_impl->FeedDecoder(data, data_size, pts_samples))
        {
            m_impl->RecordRuntimeDiagnostic(LAVOpenJocFailureReason::OpenJocDecodeError,
                                             m_impl->last_error.c_str());
            return LAVOpenJocProcessResult::Error;
        }
#endif
    }

#if defined(LAV_ENABLE_OPENJOC)
    if (end_of_stream && !m_impl->FinishDecoder())
    {
        m_impl->RecordRuntimeDiagnostic(LAVOpenJocFailureReason::OpenJocDecodeError,
                                         m_impl->last_error.c_str());
        return LAVOpenJocProcessResult::Error;
    }
#else
    (void)end_of_stream;
#endif

    return LAVOpenJocProcessResult::OpenJoc;
}

bool LAVOpenJocDecoder::ReceiveFrame(LAVOpenJocFrame &frame)
{
#if defined(LAV_ENABLE_OPENJOC)
    if (!m_impl->pending_frames.empty())
    {
        frame = std::move(m_impl->pending_frames.front());
        m_impl->pending_frames.pop_front();
        return true;
    }

    if (!m_impl->decoder)
        return false;

    openjoc_pcm_frame pcm{};
    if (m_impl->api.pcm_frame_init(&pcm) != OPENJOC_STATUS_OK)
    {
        m_impl->SetDecoderError();
        return false;
    }

    const openjoc_status status = m_impl->api.stream_decoder_receive_frame(m_impl->decoder, &pcm);
    if (status == OPENJOC_STATUS_NEED_MORE_INPUT || status == OPENJOC_STATUS_END_OF_STREAM)
        return false;
    if (status != OPENJOC_STATUS_FRAME_AVAILABLE)
    {
        m_impl->SetDecoderError();
        return false;
    }

    return m_impl->ValidateAndCopyFrame(pcm, frame);
#else
    (void)frame;
    return false;
#endif
}

bool LAVOpenJocDecoder::Drain()
{
#if defined(LAV_ENABLE_OPENJOC)
    return m_impl->available && m_impl->FinishDecoder();
#else
    return false;
#endif
}

void LAVOpenJocDecoder::Reset()
{
    m_impl->last_error.clear();
#if defined(LAV_ENABLE_OPENJOC)
    m_impl->pending_frames.clear();
    if (m_impl->decoder)
    {
        if (m_impl->api.stream_decoder_reset(m_impl->decoder) != OPENJOC_STATUS_OK)
        {
            m_impl->api.stream_decoder_destroy(m_impl->decoder);
            m_impl->decoder = nullptr;
        }
    }
    if (m_impl->classifier)
    {
        bool classifier_reset_failed = false;
#if defined(LAV_OPENJOC_TESTING)
        if (m_impl->fail_next_classifier_reset)
        {
            m_impl->fail_next_classifier_reset = false;
            m_impl->SetApiError("failed to reset OpenJOC classifier (injected)");
            classifier_reset_failed = true;
        }
        else
#endif
        if (m_impl->api.classifier_reset(m_impl->classifier) != OPENJOC_STATUS_OK)
        {
            m_impl->SetClassifierError();
            classifier_reset_failed = true;
        }

        if (classifier_reset_failed)
        {
            m_impl->api.classifier_destroy(m_impl->classifier);
            m_impl->classifier = nullptr;
            if (m_impl->CreateClassifier(&m_impl->classifier))
            {
                m_impl->available = true;
                m_impl->last_error.clear();
            }
            else
            {
                m_impl->available = false;
            }
        }
    }
#endif
    m_impl->admission.reset();
    m_impl->admission_pts_samples = kNoPts;
    m_impl->classifier_input_bytes = 0;
    m_impl->stream_input_bytes = 0;
}

void LAVOpenJocDecoder::ResetForNewStream()
{
    Reset();
    m_impl->ClearDiagnostic();
}

bool LAVOpenJocDecoder::HasError() const
{
    return !m_impl->last_error.empty();
}

const char *LAVOpenJocDecoder::LastError() const
{
    return m_impl->last_error.c_str();
}

LAVOpenJocDiagnosticSnapshot LAVOpenJocDecoder::DiagnosticSnapshot() const
{
    return m_impl->diagnostic;
}

void LAVOpenJocDecoder::RecordRuntimeDiagnostic(const LAVOpenJocFailureReason reason,
                                                const char *detail)
{
    m_impl->RecordRuntimeDiagnostic(reason, detail);
}

std::size_t LAVOpenJocDecoder::ClassifierInputBytes() const
{
    return m_impl->classifier_input_bytes;
}

std::size_t LAVOpenJocDecoder::StreamInputBytes() const
{
    return m_impl->stream_input_bytes;
}

#if defined(LAV_OPENJOC_TESTING)
void LAVOpenJocDecoder::FailNextClassifierCreateForTesting()
{
    m_impl->fail_next_classifier_create = true;
}

void LAVOpenJocDecoder::FailNextDecoderCreateForTesting()
{
    m_impl->fail_next_decoder_create = true;
}

void LAVOpenJocDecoder::FailNextClassifierResetForTesting()
{
    m_impl->fail_next_classifier_reset = true;
}

const char *LAVOpenJocDecoder::ConfigDescriptorForTesting() const
{
#if defined(LAV_ENABLE_OPENJOC)
    return m_impl->decoder && m_impl->api.stream_decoder_get_config_descriptor
               ? m_impl->api.stream_decoder_get_config_descriptor(m_impl->decoder)
               : nullptr;
#else
    return nullptr;
#endif
}
#endif
