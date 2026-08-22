/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// OpenJOC C-ABI bridge smoke test.

#include "OpenJocDecoder.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<unsigned char> read_file(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

static void classify_as_stock(const std::vector<unsigned char> &bytes)
{
    LAVOpenJocDecoder decoder;
    assert(decoder.IsAvailable());

    const LAVOpenJocProcessResult first = decoder.Process(bytes.data(), bytes.size(), INT64_MIN, false);
    if (first == LAVOpenJocProcessResult::Waiting)
        assert(decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::UseStockEac3);
    else
        assert(first == LAVOpenJocProcessResult::UseStockEac3);
    assert(decoder.State() == LAVOpenJocState::StockEac3);
    assert(decoder.StreamInputBytes() == 0);
}

static void classify_and_feed_joc(const std::vector<unsigned char> &bytes)
{
    LAVOpenJocDecoder decoder;
    assert(decoder.IsAvailable());

    LAVOpenJocProcessResult result = decoder.Process(bytes.data(), bytes.size(), INT64_MIN, false);
    if (result == LAVOpenJocProcessResult::Waiting)
        result = decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true);
    assert(result == LAVOpenJocProcessResult::OpenJoc);
    assert(decoder.State() == LAVOpenJocState::OpenJoc);
    assert(decoder.StreamInputBytes() == bytes.size());

    assert(decoder.Drain());
    LAVOpenJocFrame frame;
    while (decoder.ReceiveFrame(frame))
    {
        assert(frame.sample_rate == 48000);
        assert(frame.channel_count == 2);
        assert(frame.sample_count > 0);
        assert(frame.samples.size() == frame.sample_count * frame.channel_count);
    }
    assert(!decoder.HasError());

    LAVOpenJocDecoder eos_decoder;
    assert(eos_decoder.IsAvailable());
    assert(eos_decoder.Process(bytes.data(), bytes.size(), INT64_MIN, true) == LAVOpenJocProcessResult::OpenJoc);
    assert(eos_decoder.State() == LAVOpenJocState::OpenJoc);
    assert(eos_decoder.ClassifierInputBytes() == bytes.size());
    assert(eos_decoder.StreamInputBytes() == bytes.size());
    LAVOpenJocFrame eos_frame;
    bool received_eos_frame = false;
    while (eos_decoder.ReceiveFrame(eos_frame))
        received_eos_frame = true;
    assert(received_eos_frame);
    assert(!eos_decoder.HasError());

    decoder.Reset();
    assert(decoder.State() == LAVOpenJocState::Undecided);
    assert(decoder.ClassifierInputBytes() == 0);
    assert(decoder.StreamInputBytes() == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    classify_as_stock(read_file(argv[1]));
    classify_and_feed_joc(read_file(argv[2]));
    return 0;
}
