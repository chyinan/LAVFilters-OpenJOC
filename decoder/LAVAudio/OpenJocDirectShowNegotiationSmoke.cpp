/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Controlled-sink evidence is deliberately distinct from renderer support.
// CONTROLLED_SINK_COMPLETE proves this exact private graph and capture sink;
// only a later named-renderer run may classify renderer support.

#include <windows.h>

#include "streams.h"

#include <bcrypt.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <psapi.h>

#include "OpenJocDecoder.h"
#include "OpenJocStrictOutput.h"
#include "LAVAudioSettings.h"
#include "LAVSplitterSettings.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
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
                      std::vector<CMediaType> accepted_fallbacks, HRESULT *status)
        : CBaseFilter(L"OpenJOC Strict Capture Sink", nullptr, &filter_lock_, CLSID_NULL, status),
          expected_(expected), rejection_trap_(rejection_trap),
          accepted_fallbacks_(std::move(accepted_fallbacks)), input_(this, &filter_lock_, status)
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
        return exact ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
    }

    HRESULT RecordQuery(const AM_MEDIA_TYPE &media_type)
    {
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
        receive_connections_.emplace_back(media_type);
        RecordMutationLocked();
    }
    void RecordSetMediaType(const AM_MEDIA_TYPE &media_type)
    {
        CAutoLock lock(&filter_lock_);
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
        REFERENCE_TIME start = 0;
        REFERENCE_TIME stop = 0;
        const bool has_time = sample && sample->GetTime(&start, &stop) == S_OK;
        const auto *wave = expected_.formattype == FORMAT_WaveFormatEx && expected_.pbFormat &&
                                   expected_.cbFormat >= sizeof(WAVEFORMATEX)
                               ? reinterpret_cast<const WAVEFORMATEX *>(expected_.pbFormat)
                               : nullptr;
        const bool buffer_valid = sample && wave && wave->nBlockAlign > 0 && capacity > 0 &&
                                  length > 0 && length <= capacity &&
                                  length % wave->nBlockAlign == 0 &&
                                  SUCCEEDED(sample->GetPointer(&data)) && data;
        {
            CAutoLock lock(&filter_lock_);
            ++sample_observation_count_;
            RecordMutationLocked();
            if (attached_status == S_OK && attached)
                sample_attached_types_.emplace_back(*attached);
            sample_capacities_.push_back(capacity);
            sample_lengths_.push_back(length);
            const bool timestamp_valid = has_time && start < stop &&
                                         (timestamps_.empty() ||
                                          timestamps_.back().second == start);
            sample_buffers_valid_ = sample_buffers_valid_ && buffer_valid;
            timestamps_valid_ = timestamps_valid_ && timestamp_valid;
            if (!buffer_valid || !timestamp_valid ||
                bytes_.size() > (std::numeric_limits<std::size_t>::max)() -
                                    static_cast<std::size_t>(length))
            {
                if (attached)
                    DeleteMediaType(attached);
                return !buffer_valid || !timestamp_valid
                           ? E_INVALIDARG
                           : HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            }
            bytes_.insert(bytes_.end(), data, data + length);
            ++sample_count_;
            if (m_State == State_Running)
                ++running_sample_count_;
            if (has_time)
                timestamps_.emplace_back(start, stop);
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
        SetEvent(end_of_stream_event_);
    }
    void RecordBeginFlush()
    {
        CAutoLock lock(&filter_lock_);
        ++begin_flush_count_;
    }
    void RecordEndFlush()
    {
        CAutoLock lock(&filter_lock_);
        ++end_flush_count_;
    }
    void RecordNewSegment()
    {
        CAutoLock lock(&filter_lock_);
        ++new_segment_count_;
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
    bool sample_contracts_valid() const
    {
        CAutoLock lock(&filter_lock_);
        return sample_buffers_valid_ && timestamps_valid_ &&
               sample_capacities_.size() == sample_count_ &&
               sample_lengths_.size() == sample_count_ && timestamps_.size() == sample_count_;
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
        owner_->RecordNewSegment();
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

bool IsExactEac3MediaType(const AM_MEDIA_TYPE &media_type)
{
    return media_type.majortype == MEDIATYPE_Audio &&
           media_type.subtype == kDolbyDdPlus &&
           media_type.formattype != GUID_NULL && media_type.cbFormat >= sizeof(WAVEFORMATEX) &&
           media_type.pbFormat != nullptr;
}

HRESULT FindSingleEac3SourcePin(IBaseFilter *source, IPin **result,
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
                if (IsExactEac3MediaType(*candidate))
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

HRESULT ConfigureTargetAudio(IBaseFilter *audio_filter)
{
    ComOwner<ILAVAudioSettings> settings;
    HRESULT status = audio_filter
                         ? audio_filter->QueryInterface(
                               __uuidof(ILAVAudioSettings),
                               reinterpret_cast<void **>(settings.put()))
                         : E_POINTER;
    if (FAILED(status) || FAILED(status = settings->SetRuntimeConfig(TRUE)) ||
        FAILED(status = settings->SetFormatConfiguration(Codec_EAC3, TRUE)) ||
        FAILED(status = settings->SetBitstreamConfig(Bitstream_EAC3, FALSE)) ||
        FAILED(status = settings->SetSampleFormat(SampleFormat_FP32, TRUE)) ||
        FAILED(status = settings->SetMixingEnabled(FALSE)) ||
        FAILED(status = settings->SetOutputStandardLayout(FALSE)) ||
        FAILED(status = settings->SetOutput51LegacyLayout(FALSE)) ||
        FAILED(status = settings->SetSuppressFormatChanges(FALSE)) ||
        FAILED(status = settings->SetBitstreamingFallback(FALSE)))
        return status;
    return settings->GetFormatConfiguration(Codec_EAC3) &&
                   !settings->GetBitstreamConfig(Bitstream_EAC3) &&
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
                              IPin **audio_output, CMediaType *exact_eac3_type)
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
        FAILED(status = ConfigureTargetAudio(*audio_filter)) ||
        (configure_policy && FAILED(status = SetOpenJocPolicy(*audio_filter, policy))))
        return status;

    ComOwner<IFileSourceFilter> file_source;
    status = (*source_filter)->QueryInterface(IID_IFileSourceFilter,
                                               reinterpret_cast<void **>(file_source.put()));
    if (FAILED(status) || FAILED(status = file_source->Load(fixture.c_str(), nullptr)))
        return status;
    if (!CurrentFileMatches(file_source.get(), fixture))
        return E_UNEXPECTED;

    if (FAILED(status = FindSingleEac3SourcePin(*source_filter, source_output_pin,
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
                 L"oracle_sha256=%hs policy=%hs channels=%u mask=0x%08x samples=%llu bytes=%llu\n",
                 fixture.final_path.c_str(), fixture_sha.c_str(), oracle_sha.c_str(),
                 contract->property_page_label,
                 contract->channel_count, contract->windows_channel_mask,
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
        LAVOpenJocOutputPolicy::Stereo,   LAVOpenJocOutputPolicy::Layout51,
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
    if (!FingerprintsArePairwiseDistinct(
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
    return ClassifyControlledEvidence(inputs) == ControlledEvidenceState::ControlledSinkComplete;
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
    const bool controlled_sink = argc == 5 && wcscmp(argv[1], L"--controlled-sink") == 0;
    if (!controlled_sink &&
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
                      L"<runtime-dir> <runtime-dir\\OpenJocRuntimeIdentity.tsv> <fixture-dir>\n");
        return 64;
    }
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_status))
    {
        std::fwprintf(stderr, L"self-test COM initialization failed: 0x%08lx\n",
                      static_cast<unsigned long>(com_status));
        return 1;
    }
    const GUID &audio_class_id = !controlled_sink && wcscmp(argv[4], L"pristine") == 0
                                     ? kPristineLavAudio
                                     : kTargetLavAudio;
    const HRESULT status = controlled_sink
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
    std::wprintf(L"UNVERIFIED: lane-private Audio/Splitter activation and staged identity passed\n");
    return 0;
}
