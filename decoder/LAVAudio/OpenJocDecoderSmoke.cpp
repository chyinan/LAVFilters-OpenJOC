/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

// OpenJOC C-ABI bridge smoke test.

#include "OpenJocDecoder.h"
#include "OpenJocDialnorm.h"
#include "OpenJocOutput.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

static_assert(std::is_same_v<decltype(LAVOpenJocFrame{}.output_contract),
                             const LAVOpenJocOutputContract *>);

static constexpr std::array<LAVOpenJocOutputPolicy, LAV_OPENJOC_OUTPUT_CONTRACT_COUNT> kPolicies = {
    LAVOpenJocOutputPolicy::Stereo,   LAVOpenJocOutputPolicy::Layout51,
    LAVOpenJocOutputPolicy::Layout71, LAVOpenJocOutputPolicy::Layout512,
    LAVOpenJocOutputPolicy::Layout514, LAVOpenJocOutputPolicy::Layout712,
    LAVOpenJocOutputPolicy::Layout714,
};

static std::vector<unsigned char> read_file(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

static void classify_as_stock(const std::vector<unsigned char> &bytes)
{
    LAVOpenJocDecoder decoder;
    assert(decoder.IsAvailable());
    assert(decoder.OutputContract() == FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Stereo));

    assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) ==
           LAVOpenJocProcessResult::UseStockDecoder);
    assert(decoder.State() == LAVOpenJocState::StockCodec);
    assert(decoder.StreamInputBytes() == 0);
    assert(decoder.ClassifierInputBytes() > 0);
    assert(decoder.ClassifierInputBytes() <= LAVOpenJocAdmission::MaxRetainedBytes);
}

static std::size_t assert_frames_match_contract(LAVOpenJocDecoder &decoder,
                                                const LAVOpenJocOutputContract *contract)
{
    LAVOpenJocFrame frame;
    std::size_t frame_count = 0;
    while (decoder.ReceiveFrame(frame))
    {
        ++frame_count;
        assert(frame.output_contract == contract);
        assert(frame.sample_rate == 48000);
        assert(frame.channel_count == contract->channel_count);
        assert(frame.sample_count > 0);
        assert(frame.samples.size() == frame.sample_count * frame.channel_count);

        AVChannelLayout prepared_layout{};
        std::uint32_t prepared_samples = 0;
        std::uint32_t prepared_bytes = 0;
        assert(PrepareLAVOpenJocFrameHandoff(
            decoder.OutputContract(), frame.output_contract, frame.sample_rate, frame.channel_count,
            frame.sample_count, frame.samples.size(), &prepared_layout, &prepared_samples, &prepared_bytes));
        assert(prepared_samples == frame.sample_count);
        assert(prepared_bytes == frame.samples.size() * sizeof(float));
        assert(prepared_layout.order == AV_CHANNEL_ORDER_NATIVE);
        assert(prepared_layout.nb_channels == static_cast<int>(contract->channel_count));
        assert(prepared_layout.u.mask == contract->ffmpeg_channel_mask);
        for (std::uint32_t index = 0; index < contract->channel_count; ++index)
            assert(av_channel_layout_channel_from_index(&prepared_layout, index) ==
                   contract->ordered_channels[index]);
        av_channel_layout_uninit(&prepared_layout);
    }
    assert(frame_count > 0);
    assert(!decoder.HasError());
    return frame_count;
}

static std::size_t decode_policy(const std::vector<unsigned char> &bytes,
                                 const LAVOpenJocOutputPolicy policy)
{
    LAVOpenJocDecoder decoder;
    assert(decoder.IsAvailable());
    assert(decoder.SetOutputPolicy(policy));
    const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(policy);
    assert(contract != nullptr);
    assert(decoder.OutputContract() == contract);

    assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::OpenJoc);
    assert(decoder.State() == LAVOpenJocState::OpenJoc);
    assert(decoder.StreamInputBytes() == bytes.size());
    assert(decoder.ClassifierInputBytes() > 0);
    assert(decoder.ClassifierInputBytes() <= LAVOpenJocAdmission::MaxRetainedBytes);
    const std::size_t frame_count = assert_frames_match_contract(decoder, contract);
    std::printf("policy=%s openjoc_layout=%s api_layout=%s channels=%u mask=0x%08x frames=%zu\n",
                contract->property_page_label, contract->openjoc_layout_name,
                contract->ffmpeg_standard_layout_name, contract->channel_count,
                contract->windows_channel_mask, frame_count);

    decoder.Reset();
    assert(decoder.OutputContract() == contract);
    assert(decoder.State() == LAVOpenJocState::Undecided);
    assert(decoder.ClassifierInputBytes() == 0);
    assert(decoder.StreamInputBytes() == 0);
    return frame_count;
}

static void classify_and_feed_joc_for_all_policies(const std::vector<unsigned char> &bytes)
{
    std::size_t layout51_frames = 0;
    for (const LAVOpenJocOutputPolicy policy : kPolicies)
    {
        const std::size_t frame_count = decode_policy(bytes, policy);
        if (policy == LAVOpenJocOutputPolicy::Layout51)
            layout51_frames = frame_count;
    }
    assert(layout51_frames > 1);
}

static void policy_assignment_and_switch_are_safe(const std::vector<unsigned char> &bytes)
{
    const LAVOpenJocOutputContract *layout51 =
        FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout51);
    const LAVOpenJocOutputContract *layout714 =
        FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout714);

    LAVOpenJocDecoder decoder;
    assert(decoder.IsAvailable());
    assert(decoder.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout51));
    assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::OpenJoc);
    const std::size_t classified_before_noop = decoder.ClassifierInputBytes();
    const std::size_t streamed_before_noop = decoder.StreamInputBytes();
    assert(decoder.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout51));
    assert(decoder.State() == LAVOpenJocState::OpenJoc);
    assert(decoder.ClassifierInputBytes() == classified_before_noop);
    assert(decoder.StreamInputBytes() == streamed_before_noop);

    LAVOpenJocFrame first;
    assert(decoder.ReceiveFrame(first));
    assert(first.output_contract == layout51);
    assert(!decoder.SetOutputPolicy(static_cast<LAVOpenJocOutputPolicy>(
        (std::numeric_limits<std::uint32_t>::max)())));
    assert(decoder.OutputContract() == layout51);
    assert(decoder.State() == LAVOpenJocState::OpenJoc);
    assert(decoder.ClassifierInputBytes() == classified_before_noop);
    assert(decoder.StreamInputBytes() == streamed_before_noop);

    assert(decoder.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout714));
    assert(decoder.OutputContract() == layout714);
    assert(decoder.State() == LAVOpenJocState::Undecided);
    assert(decoder.ClassifierInputBytes() == 0);
    assert(decoder.StreamInputBytes() == 0);
    LAVOpenJocFrame stale;
    assert(!decoder.ReceiveFrame(stale));

    assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::OpenJoc);
    assert_frames_match_contract(decoder, layout714);

    decoder.Reset();
    assert(decoder.OutputContract() == layout714);
    assert(decoder.State() == LAVOpenJocState::Undecided);
    assert(decoder.ClassifierInputBytes() == 0);
    assert(decoder.StreamInputBytes() == 0);
}

static void dialnorm_assignment_and_switch_are_safe(const std::vector<unsigned char> &bytes)
{
    LAVOpenJocDecoder decoder;
    assert(decoder.IsAvailable());
    assert(decoder.DialnormPolicy() == LAVOpenJocDialnormPolicy::Calibrated);
    assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::OpenJoc);
#if defined(LAV_OPENJOC_TESTING)
    assert(decoder.ConfigDescriptorForTesting() != nullptr);
    assert(std::strstr(decoder.ConfigDescriptorForTesting(), "dialnorm=default") != nullptr);
#endif

    const std::size_t classified_before_noop = decoder.ClassifierInputBytes();
    const std::size_t streamed_before_noop = decoder.StreamInputBytes();
    assert(decoder.SetDialnormPolicy(LAVOpenJocDialnormPolicy::Calibrated));
    assert(decoder.State() == LAVOpenJocState::OpenJoc);
    assert(decoder.ClassifierInputBytes() == classified_before_noop);
    assert(decoder.StreamInputBytes() == streamed_before_noop);

    assert(!decoder.SetDialnormPolicy(static_cast<LAVOpenJocDialnormPolicy>(0xffffffffu)));
    assert(decoder.DialnormPolicy() == LAVOpenJocDialnormPolicy::Calibrated);
    assert(decoder.State() == LAVOpenJocState::OpenJoc);
    assert(decoder.ClassifierInputBytes() == classified_before_noop);
    assert(decoder.StreamInputBytes() == streamed_before_noop);

    assert(decoder.SetDialnormPolicy(LAVOpenJocDialnormPolicy::UnityCompatibility));
    assert(decoder.DialnormPolicy() == LAVOpenJocDialnormPolicy::UnityCompatibility);
    assert(decoder.State() == LAVOpenJocState::Undecided);
    assert(decoder.ClassifierInputBytes() == 0);
    assert(decoder.StreamInputBytes() == 0);
    LAVOpenJocFrame stale;
    assert(!decoder.ReceiveFrame(stale));

    assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::OpenJoc);
#if defined(LAV_OPENJOC_TESTING)
    assert(decoder.ConfigDescriptorForTesting() != nullptr);
    assert(std::strstr(decoder.ConfigDescriptorForTesting(), "dialnorm=analog") != nullptr);
#endif
    assert_frames_match_contract(decoder, decoder.OutputContract());
    assert(decoder.SetDialnormPolicy(LAVOpenJocDialnormPolicy::Calibrated));
    assert(decoder.DialnormPolicy() == LAVOpenJocDialnormPolicy::Calibrated);
    assert(decoder.State() == LAVOpenJocState::Undecided);
}

static void dialnorm_config_is_applied_before_decoder_creation()
{
    const std::filesystem::path source_path = std::filesystem::path(__FILE__).parent_path() / "OpenJocDecoder.cpp";
    std::ifstream source_file(source_path, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(source_file)), std::istreambuf_iterator<char>());
    const auto initializer_symbol = source.find("openjoc_decoder_config_init_v1_4");
    const auto config_init = source.find("api.decoder_config_init_v1_4(&config)");
    const auto map = source.find("TryMapLAVOpenJocDialnormPolicy", config_init);
    const auto assignment = source.find("config.dialnorm_mode = dialnorm_mode", map);
    const auto create = source.find("api.stream_decoder_create(&config, &candidate)", assignment);
    assert(initializer_symbol != std::string::npos && config_init != std::string::npos &&
           map != std::string::npos && assignment != std::string::npos && create != std::string::npos);
    assert(config_init < map && map < assignment && assignment < create);
}

#if defined(LAV_OPENJOC_TESTING)
static void failed_policy_change_is_atomic(const std::vector<unsigned char> &bytes,
                                           const bool fail_classifier_create)
{
    const LAVOpenJocOutputContract *layout51 =
        FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout51);
    LAVOpenJocDecoder decoder;
    assert(decoder.IsAvailable());
    assert(decoder.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout51));
    assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::OpenJoc);

    const LAVOpenJocState old_state = decoder.State();
    const std::size_t old_classifier_bytes = decoder.ClassifierInputBytes();
    const std::size_t old_stream_bytes = decoder.StreamInputBytes();
    const bool old_available = decoder.IsAvailable();
    if (fail_classifier_create)
        decoder.FailNextClassifierCreateForTesting();
    else
        decoder.FailNextDecoderCreateForTesting();

    assert(!decoder.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout714));
    assert(decoder.OutputContract() == layout51);
    assert(decoder.State() == old_state);
    assert(decoder.ClassifierInputBytes() == old_classifier_bytes);
    assert(decoder.StreamInputBytes() == old_stream_bytes);
    assert(decoder.IsAvailable() == old_available);
    assert(decoder.HasError());

    LAVOpenJocFrame pending;
    assert(decoder.ReceiveFrame(pending));
    assert(pending.output_contract == layout51);
    assert(pending.channel_count == layout51->channel_count);

    decoder.Reset();
    assert(decoder.OutputContract() == layout51);
    assert(decoder.IsAvailable());
    assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::OpenJoc);
    assert_frames_match_contract(decoder, layout51);
}

static void reset_preserves_classifier_rebuild_failure_diagnostic()
{
    LAVOpenJocDecoder decoder;
    assert(decoder.IsAvailable());
    assert(decoder.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout51));
    const LAVOpenJocOutputContract *contract = decoder.OutputContract();
    decoder.FailNextClassifierResetForTesting();
    decoder.FailNextClassifierCreateForTesting();

    decoder.Reset();

    assert(decoder.OutputContract() == contract);
    assert(!decoder.IsAvailable());
    assert(decoder.HasError());
    assert(std::strstr(decoder.LastError(), "classifier") != nullptr);
    assert(decoder.State() == LAVOpenJocState::Undecided);
    assert(decoder.ClassifierInputBytes() == 0);
    assert(decoder.StreamInputBytes() == 0);
}

static void failed_dialnorm_change_is_atomic(const std::vector<unsigned char> &bytes,
                                             const bool fail_classifier_create)
{
    LAVOpenJocDecoder decoder;
    assert(decoder.IsAvailable());
    assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::OpenJoc);

    const LAVOpenJocState old_state = decoder.State();
    const std::size_t old_classifier_bytes = decoder.ClassifierInputBytes();
    const std::size_t old_stream_bytes = decoder.StreamInputBytes();
    if (fail_classifier_create)
        decoder.FailNextClassifierCreateForTesting();
    else
        decoder.FailNextDecoderCreateForTesting();

    assert(!decoder.SetDialnormPolicy(LAVOpenJocDialnormPolicy::UnityCompatibility));
    assert(decoder.DialnormPolicy() == LAVOpenJocDialnormPolicy::Calibrated);
    assert(decoder.State() == old_state);
    assert(decoder.ClassifierInputBytes() == old_classifier_bytes);
    assert(decoder.StreamInputBytes() == old_stream_bytes);
    assert(decoder.HasError());

    LAVOpenJocFrame pending;
    assert(decoder.ReceiveFrame(pending));
    assert(pending.output_contract == decoder.OutputContract());
}
#endif

int main(int argc, char **argv)
{
    assert(argc == 3);
    classify_as_stock(read_file(argv[1]));
    const std::vector<unsigned char> joc = read_file(argv[2]);
    classify_and_feed_joc_for_all_policies(joc);
    policy_assignment_and_switch_are_safe(joc);
    dialnorm_assignment_and_switch_are_safe(joc);
    dialnorm_config_is_applied_before_decoder_creation();
#if defined(LAV_OPENJOC_TESTING)
    failed_policy_change_is_atomic(joc, true);
    failed_policy_change_is_atomic(joc, false);
    failed_dialnorm_change_is_atomic(joc, true);
    failed_dialnorm_change_is_atomic(joc, false);
    reset_preserves_classifier_rebuild_failure_diagnostic();
#endif
    return 0;
}
