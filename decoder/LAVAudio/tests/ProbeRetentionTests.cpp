// Portable byte-transport regression, not a classifier/decoder/Windows test.
// The runner extracts the complete, unchanged production Process method.
#include "OpenJocAdmission.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>
#define LAV_ENABLE_OPENJOC
constexpr std::int64_t kNoPts = INT64_MIN;
enum class LAVOpenJocProcessResult {Error, UseStockDecoder, Waiting, OpenJoc};
enum class LAVOpenJocFailureReason {BinauralHrtfConfiguration, OpenJocDecodeError};
void require(bool ok, const char *detail)
{
    if (!ok) { std::cerr << detail << '\n'; std::abort(); }
}
struct LAVOpenJocDecoder
{
    struct Impl
    {
        bool available = true, configuration_error = false;
        std::string configuration_error_detail, last_error;
        LAVOpenJocAdmission admission;
        std::int64_t admission_pts_samples = kNoPts;
        std::vector<unsigned char> admission_bytes, delivered;
        std::vector<std::int64_t> anchors;
        std::size_t classified = 0;
        int classify_calls = 0, confirm_on_call = 1, finishes = 0;
        void SetError(const char *s) { last_error = s; }
        void ClearDiagnostic() {}
        void RecordProbeFailure(LAVOpenJocClassification, bool) {}
        void RecordRuntimeDiagnostic(LAVOpenJocFailureReason, const char *) {}
        LAVOpenJocClassification Classify(const unsigned char *, std::size_t size, bool eof, bool *failed)
        {
            *failed = false;
            classified += size;
            if (!eof) ++classify_calls;
            return classify_calls >= confirm_on_call ? LAVOpenJocClassification::ConfirmedJoc
                                                      : LAVOpenJocClassification::Unknown;
        }
        bool FeedDecoder(const unsigned char *data, std::size_t size, std::int64_t pts)
        {
            if (size) { delivered.insert(delivered.end(), data, data + size); anchors.push_back(pts); }
            return true;
        }
        bool FinishDecoder() { ++finishes; return true; }
    } impl;
    Impl *m_impl = &impl;
    std::recursive_mutex m_mutex;
    LAVOpenJocProcessResult Process(const unsigned char *, std::size_t, std::int64_t, bool);
};
#include "ProbeProcess.inc"

bool run_case(const std::vector<unsigned char> &input, int mode)
{
    LAVOpenJocDecoder decoder;
    auto &state = decoder.impl;
    constexpr std::int64_t anchor = 48000;
    const bool cumulative = mode == 1;
    const bool incremental = mode == 2;
    const bool eof = mode == 3;
    LAVOpenJocProcessResult result;
    if (cumulative || incremental)
    {
        state.confirm_on_call = 2;
        require(decoder.Process(input.data(), 4096, anchor, false) == LAVOpenJocProcessResult::Waiting,
                "initial probe must wait");
        const std::size_t offset = incremental ? 4096 : 0;
        result = decoder.Process(input.data() + offset, input.size() - offset, anchor + 1536, false);
    }
    else
        result = decoder.Process(input.data(), input.size(), anchor, eof);
    const bool bytes_ok = state.delivered == input;
    bool anchors_ok = !state.anchors.empty() && state.anchors.front() == anchor;
    for (std::size_t i = 1; i < state.anchors.size(); ++i)
        anchors_ok = anchors_ok && state.anchors[i] == kNoPts;
    const bool ok = result == LAVOpenJocProcessResult::OpenJoc && bytes_ok && anchors_ok &&
                    state.admission_bytes.empty() && state.classified <= LAVOpenJocAdmission::MaxRetainedBytes &&
                    state.finishes == (eof ? 1 : 0);
    std::cout << (ok ? "PASS " : "FAIL ") << "mode=" << mode << " supplied=" << input.size()
              << " delivered=" << state.delivered.size() << " classified=" << state.classified
              << " exact_bytes=" << bytes_ok << '\n';
    // Once admitted, subsequent ordinary chunks remain a direct pass-through.
    require(decoder.Process(input.data(), 4096, kNoPts, false) == LAVOpenJocProcessResult::OpenJoc,
            "post-promotion input");
    require(state.delivered.size() >= 4096 &&
            std::equal(input.begin(), input.begin() + 4096, state.delivered.end() - 4096),
            "post-promotion payload");
    return ok;
}
int main(int argc, char **argv)
{
    require(argc == 2, "expected public valid JOC fixture path");
    std::ifstream stream(argv[1], std::ios::binary);
    require(bool(stream), "fixture unavailable");
    std::vector<unsigned char> input((std::istreambuf_iterator<char>(stream)), {});
    require(input.size() == 524288, "expected unchanged 524288-byte public lifecycle fixture");
    require(!std::equal(input.begin(), input.begin() + 4096, input.begin() + 4096),
            "incremental fixture must be distinct from cumulative prefix");
    bool ok = true;
    std::vector<unsigned char> small(input.begin(), input.begin() + 4096);
    ok = run_case(small, 0) && ok;
    for (int mode = 0; mode < 4; ++mode) ok = run_case(input, mode) && ok;
    return ok ? 0 : 1;
}
