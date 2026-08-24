/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Task 1 infrastructure only. A passing self-test proves lane-private filter
// activation and staged runtime identity; it does not prove renderer support.

#include <windows.h>

#include <bcrypt.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <limits>
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

struct EvidenceInputs
{
    bool exact_connection = false;
    bool paused = false;
    bool running = false;
    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
    bool end_of_stream = false;
    bool graph_error = false;
};

enum class ControlledEvidenceState
{
    Incomplete,
    ControlledSinkComplete,
};

ControlledEvidenceState ClassifyControlledEvidence(const EvidenceInputs &inputs)
{
    if (!inputs.exact_connection || !inputs.paused || !inputs.running || inputs.samples == 0 ||
        inputs.bytes == 0 || !inputs.end_of_stream || inputs.graph_error)
        return ControlledEvidenceState::Incomplete;
    return ControlledEvidenceState::ControlledSinkComplete;
}
} // namespace openjoc_harness_core

// pattern: Imperative Shell
namespace openjoc_harness_shell
{
using Digest = std::array<std::uint8_t, 32>;
using DllGetClassObjectProc = HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *);

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
    right.pUnk = reinterpret_cast<IUnknown *>(&right);
    if (openjoc_harness_core::ExactMediaTypeEqual(left, right))
        return false;
    right.pUnk = nullptr;
    ++second_format.Format.nSamplesPerSec;
    return !openjoc_harness_core::ExactMediaTypeEqual(left, right);
}

bool TestPureHelpers()
{
    using namespace openjoc_harness_core;
    if (!FingerprintsArePairwiseDistinct(
            {{0.0f, 1.0f, 2.0f}, {1.0f, 2.0f, 3.0f}, {2.0f, 3.0f, 4.0f}}) ||
        FingerprintsArePairwiseDistinct({{0.0f, 1.0f}, {0.0f, 1.0f}}))
        return false;
    EvidenceInputs inputs;
    if (ClassifyControlledEvidence(inputs) != ControlledEvidenceState::Incomplete)
        return false;
    inputs.exact_connection = inputs.paused = inputs.running = inputs.end_of_stream = true;
    inputs.samples = 1;
    inputs.bytes = 8;
    return ClassifyControlledEvidence(inputs) == ControlledEvidenceState::ControlledSinkComplete;
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
        (!TestExactMediaTypeComparison() || !TestPureHelpers() || !RuntimeIdentityMatches(records)))
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
    if (argc != 5 || wcscmp(argv[1], L"--self-test") != 0 ||
        (wcscmp(argv[4], L"target") != 0 && wcscmp(argv[4], L"pristine") != 0))
    {
        std::fwprintf(stderr,
                      L"usage: OpenJocDirectShowNegotiationSmoke.exe --write-manifest "
                      L"<runtime-dir> <runtime-dir\\OpenJocRuntimeIdentity.tsv>\n"
                      L"   or: OpenJocDirectShowNegotiationSmoke.exe --self-test <runtime-dir> "
                      L"<runtime-dir\\OpenJocRuntimeIdentity.tsv> <target|pristine>\n");
        return 64;
    }
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_status))
    {
        std::fwprintf(stderr, L"self-test COM initialization failed: 0x%08lx\n",
                      static_cast<unsigned long>(com_status));
        return 1;
    }
    const GUID &audio_class_id =
        wcscmp(argv[4], L"target") == 0 ? kTargetLavAudio : kPristineLavAudio;
    const HRESULT status = openjoc_harness_shell::RunSelfTest(argv[2], argv[3], audio_class_id);
    CoUninitialize();
    if (FAILED(status))
    {
        std::fwprintf(stderr, L"self-test failed: 0x%08lx\n",
                      static_cast<unsigned long>(status));
        return 1;
    }
    std::wprintf(L"UNVERIFIED: lane-private Audio/Splitter activation and staged identity passed\n");
    return 0;
}
