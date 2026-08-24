/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// RT_DIALOG identity control for guarded target-only property-page resources.

#include <windows.h>

#include <cstdio>
#include <initializer_list>
#include <vector>

namespace
{
class ResourceModule final
{
  public:
    explicit ResourceModule(const wchar_t *path)
        : module_(LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))
    {
    }
    ~ResourceModule()
    {
        if (module_)
            FreeLibrary(module_);
    }
    bool loaded() const { return module_ != nullptr; }
    std::vector<unsigned char> Dialog(const WORD id) const
    {
        HRSRC resource = FindResourceW(module_, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(5));
        if (!resource)
            return {};
        const DWORD size = SizeofResource(module_, resource);
        HGLOBAL loaded = LoadResource(module_, resource);
        const auto *data = static_cast<const unsigned char *>(LockResource(loaded));
        if (!data || size == 0)
            return {};
        return std::vector<unsigned char>(data, data + size);
    }

  private:
    HMODULE module_ = nullptr;
};
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc != 4)
    {
        std::fwprintf(stderr, L"usage: LAVAudioResourceIdentitySmoke.exe <target.ax> <stock.ax> <pre-phase4.ax>\n");
        return 2;
    }

    ResourceModule target(argv[1]);
    ResourceModule stock(argv[2]);
    ResourceModule baseline(argv[3]);
    if (!target.loaded() || !stock.loaded() || !baseline.loaded())
    {
        std::fwprintf(stderr, L"failed to load one or more resource modules\n");
        return 1;
    }

    for (const WORD id : {WORD(9), WORD(10), WORD(11), WORD(12)})
    {
        const auto stock_dialog = stock.Dialog(id);
        const auto baseline_dialog = baseline.Dialog(id);
        if (stock_dialog.empty() || stock_dialog != baseline_dialog)
        {
            std::fwprintf(stderr, L"stock RT_DIALOG %u changed\n", static_cast<unsigned>(id));
            return 1;
        }
    }

    if (target.Dialog(10) != stock.Dialog(10) || target.Dialog(11) != stock.Dialog(11))
    {
        std::fwprintf(stderr, L"unguarded target RT_DIALOG changed\n");
        return 1;
    }
    if (target.Dialog(9) == stock.Dialog(9) || target.Dialog(12) == stock.Dialog(12))
    {
        std::fwprintf(stderr, L"target-only settings/status RT_DIALOG was not emitted\n");
        return 1;
    }

    std::wprintf(L"LAV Audio guarded RT_DIALOG identity passed\n");
    return 0;
}
