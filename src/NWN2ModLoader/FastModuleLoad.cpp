// See FastModuleLoad.h.
#include "FastModuleLoad.h"

#include <Data.h>

#include <detours.h>

#include <algorithm>
#include <cwctype>

namespace nwn2mod
{
    FastModuleLoad::CopyFileWFunc FastModuleLoad::_realCopyFileW = nullptr;
    std::shared_ptr<Logger> FastModuleLoad::_logger;
    bool FastModuleLoad::_installed = false;

    std::atomic<uint64_t> FastModuleLoad::_linked{0};
    std::atomic<uint64_t> FastModuleLoad::_copied{0};
    std::atomic<uint64_t> FastModuleLoad::_linkFailures{0};
    std::atomic<uint64_t> FastModuleLoad::_bytesAvoided{0};
    std::atomic<bool> FastModuleLoad::_warned{false};

    namespace
    {
        /// Size, or 0 if unreadable. Only feeds the "bytes avoided" figure.
        uint64_t FileSize(LPCWSTR path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!::GetFileAttributesExW(path, GetFileExInfoStandard, &data))
            {
                return 0;
            }

            return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }
    }

    bool FastModuleLoad::IsWorkingCopy(const std::wstring& path)
    {
        // EE names the working copy CURRENTGAME.0, so the numeric suffix has to be accepted:
        // currentgame, currentgame.0, currentgame.12. Matching a path component rather than a
        // substring - "\currentgame\" misses "\currentgame.0\", and xp_fastboot's
        // "\nwn2\currentgame" is tied to one layout. No allocation: this runs per copied file.
        constexpr std::wstring_view kName = L"currentgame";

        size_t start = 0;
        while (start <= path.size())
        {
            size_t end = path.find_first_of(L"\\/", start);
            if (end == std::wstring::npos)
            {
                end = path.size();
            }

            const std::wstring_view component(path.data() + start, end - start);

            if (component.size() >= kName.size())
            {
                bool namesMatch = true;
                for (size_t i = 0; i < kName.size(); ++i)
                {
                    if (std::towlower(component[i]) != kName[i])
                    {
                        namesMatch = false;
                        break;
                    }
                }

                if (namesMatch)
                {
                    // Either exactly "currentgame", or the remainder is ".<digits>".
                    const std::wstring_view rest = component.substr(kName.size());

                    if (rest.empty())
                    {
                        return true;
                    }

                    if (rest.size() >= 2 && rest[0] == L'.' &&
                        std::all_of(rest.begin() + 1, rest.end(),
                                    [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                    {
                        return true;
                    }
                }
            }

            if (end == path.size())
            {
                break;
            }

            start = end + 1;
        }

        return false;
    }

    void FastModuleLoad::WarnIfNeverMatching(LPCWSTR destination)
    {
        // 64 copies with nothing linked means the filter does not match this installation.
        constexpr uint64_t kThreshold = 64;

        if (_linked.load() != 0 || _copied.load() != kThreshold || _warned.exchange(true))
        {
            return;
        }

        if (_logger)
        {
            (*_logger)(Logger::Level::Warning,
                       "fast_module_load is enabled but none of the first {} copied files looked like "
                       "a working copy, so nothing is being linked and boot time is unchanged. The "
                       "filter accepts a path component of 'currentgame', optionally followed by a "
                       "numeric suffix such as 'CURRENTGAME.0'; a sample destination was '{}'. If the "
                       "server's working copy is named differently again, send that path - the filter "
                       "is a one-line change.",
                       kThreshold, ::ToString(destination));
        }
    }

    BOOL WINAPI FastModuleLoad::HookCopyFileW(LPCWSTR existing, LPCWSTR destination, BOOL failIfExists)
    {
        if (existing == nullptr || destination == nullptr || !IsWorkingCopy(destination))
        {
            _copied.fetch_add(1);
            WarnIfNeverMatching(destination);
            return _realCopyFileW(existing, destination, failIfExists);
        }

        // A link cannot overwrite, so without failIfExists the destination goes first. With it,
        // CreateHardLinkW fails on an existing destination just as CopyFileW would.
        if (!failIfExists)
        {
            ::DeleteFileW(destination);
        }

        const uint64_t size = FileSize(existing);

        if (::CreateHardLinkW(destination, existing, nullptr))
        {
            _linked.fetch_add(1);
            _bytesAvoided.fetch_add(size);
            return TRUE;
        }

        const DWORD error = ::GetLastError();

        // Neither of these says anything about whether linking works here, so they are passed on
        // without counting or warning: ALREADY_EXISTS with failIfExists is what the caller asked
        // to be told, and a missing source has nothing to link.
        const bool expected = (error == ERROR_ALREADY_EXISTS && failIfExists) ||
                              error == ERROR_FILE_NOT_FOUND ||
                              error == ERROR_PATH_NOT_FOUND;

        if (expected)
        {
            return _realCopyFileW(existing, destination, failIfExists);
        }

        // A genuine inability to link - cross-volume, non-NTFS, or an unopenable source.
        // xp_fastboot returned FALSE here, which fails the module load; copying is slow but works.
        if (_linkFailures.fetch_add(1) == 0 && _logger)
        {
            (*_logger)(Logger::Level::Warning,
                       "could not hard-link '{}' to '{}' (error {}); falling back to copying. Hard "
                       "links need both paths on one NTFS volume. Boot will be as slow as it was "
                       "before this option existed.",
                       ::ToString(existing), ::ToString(destination), error);
        }

        _copied.fetch_add(1);
        return _realCopyFileW(existing, destination, failIfExists);
    }

    bool FastModuleLoad::Install(std::shared_ptr<Logger> logger)
    {
        if (_installed)
        {
            return true;
        }

        _logger = std::move(logger);

        // Resolved by name so the log can say where from. The api-set stub and kernel32 both route
        // to KernelBase, and Detours patches the implementation, so every caller is covered.
        const wchar_t* sources[] = {L"kernelbase.dll", L"kernel32.dll"};
        const wchar_t* resolvedFrom = nullptr;

        for (const wchar_t* module : sources)
        {
            if (HMODULE handle = ::GetModuleHandleW(module))
            {
                if (auto* proc = ::GetProcAddress(handle, "CopyFileW"))
                {
                    _realCopyFileW = reinterpret_cast<CopyFileWFunc>(proc);
                    resolvedFrom = module;
                    break;
                }
            }
        }

        if (_realCopyFileW == nullptr)
        {
            if (_logger)
            {
                (*_logger)(Logger::Level::Error,
                           "fast_module_load: could not resolve CopyFileW, so the module copy cannot "
                           "be intercepted. The server will boot normally, just slowly.");
            }
            return false;
        }

        ::DetourTransactionBegin();
        ::DetourUpdateThread(::GetCurrentThread());
        ::DetourAttach(&(PVOID&)_realCopyFileW, &HookCopyFileW);

        const LONG error = ::DetourTransactionCommit();
        if (error != NO_ERROR)
        {
            if (_logger)
            {
                (*_logger)(Logger::Level::Error,
                           "fast_module_load: Detours refused to attach to CopyFileW (error {}). The "
                           "server will boot normally, just slowly.", error);
            }
            _realCopyFileW = nullptr;
            return false;
        }

        _installed = true;

        if (_logger)
        {
            (*_logger)("fast_module_load: hard-linking the module into the working copy instead of "
                       "copying it. CopyFileW resolved from {}.", ::ToString(resolvedFrom));
        }

        return true;
    }

    void FastModuleLoad::Uninstall()
    {
        if (!_installed)
        {
            return;
        }

        ::DetourTransactionBegin();
        ::DetourUpdateThread(::GetCurrentThread());
        ::DetourDetach(&(PVOID&)_realCopyFileW, &HookCopyFileW);
        ::DetourTransactionCommit();

        _installed = false;

        if (_logger)
        {
            (*_logger)("fast_module_load: {} files linked, {} copied, {} links failed, {:.1f} MB not "
                       "copied.",
                       _linked.load(), _copied.load(), _linkFailures.load(),
                       static_cast<double>(_bytesAvoided.load()) / (1024.0 * 1024.0));
        }

        _logger.reset();
    }
}
