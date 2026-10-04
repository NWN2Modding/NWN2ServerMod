#pragma once
// Hard-links the module into the server's working copy instead of letting the server copy it.
//
// The 64-bit server calls CopyFileW, resolved through api-ms-win-core-file-l2-1-2.dll. NWNX4's
// xp_fastboot hooks CopyFileA in KERNEL32.dll, which the 64-bit server does not import.
//
// Two differences from xp_fastboot:
//   - a failed CreateHardLink falls back to the real copy instead of failing the module load
//   - the destination filter matches a 'currentgame' path component with an optional numeric
//     suffix, rather than the fixed '\nwn2\currentgame' substring

#include <Logger.h>

#include <atomic>
#include <memory>
#include <string>

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace nwn2mod
{
    /// Replaces the server's module copy with hard links. Install once, before the module loads.
    class FastModuleLoad
    {
    public:
        /// Attaches the hook. False if CopyFileW could not be resolved or Detours refused; the
        /// server then boots normally, just slowly. Calling it twice does nothing.
        static bool Install(std::shared_ptr<Logger> logger);

        /// Detaches the hook, and logs what it did. Safe to call when not installed.
        static void Uninstall();

        /// Whether the hook is currently attached.
        static bool Installed() noexcept { return _installed; }

    private:
        /// Links when the destination is in the working copy; defers to the real CopyFileW otherwise
        /// and for any link that fails.
        static BOOL WINAPI HookCopyFileW(LPCWSTR existing, LPCWSTR destination, BOOL failIfExists);

        /// True if path has a "currentgame" component, optionally suffixed ".<digits>".
        /// Case-insensitive.
        static bool IsWorkingCopy(const std::wstring& path);

        /// Warns once if copies are happening and none is being linked, which means the filter does
        /// not match this installation.
        static void WarnIfNeverMatching(LPCWSTR destination);

        using CopyFileWFunc = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);

        static CopyFileWFunc _realCopyFileW;
        static std::shared_ptr<Logger> _logger;
        static bool _installed;

        // Counters for the shutdown summary.
        static std::atomic<uint64_t> _linked;
        static std::atomic<uint64_t> _copied;
        static std::atomic<uint64_t> _linkFailures;
        static std::atomic<uint64_t> _bytesAvoided;
        static std::atomic<bool> _warned;
    };
}
