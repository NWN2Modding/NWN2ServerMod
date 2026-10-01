// xp_profiler, ported from NWNX4 to the NWN2ServerMod C ABI.
//
// Original: Copyright (C) 2003 Ingmar Stieger (Papillon), (C) 2007 virusman. GPL v2 or later.
//
// Times every script the server runs and periodically logs per-script totals, so you can see which
// scripts are costing a persistent world its frame time.
//
// What changed from the NWNX4 version:
//   Plugin subclass              -> nwn2::PluginBase
//   xp_profiler.ini              -> xp_profiler.yml, matching nwn2mod.config
//   hand-rolled hash table       -> std::unordered_map (the original's hash.cpp was 328 lines)
//   __declspec(naked) asm        -> an ordinary C++ function
//   scan 0x400000-0x800000       -> a byte pattern via the loader's IAddressService
//
// The assembly is the interesting one. NWNX4 needed a naked trampoline because 32-bit thiscall put
// `this` in ECX with arguments on the stack, which C could not express. On x64 __fastcall *is* the
// calling convention, so the whole thing is:
//
//     int __fastcall Hook(void* pVM, int nScriptPart)
//     {
//         start timing
//         int result = original(pVM, nScriptPart);
//         stop timing
//         return result;
//     }
//
// preserving the return value for StartingConditional scripts without touching a register by hand.
//
// On hooking: this plugin attaches its own detour rather than asking the loader for help. Two
// plugins detouring the same function was measured to chain correctly - each attach wraps the
// previous one, in reverse load order, with return values intact - so no loader service is needed.
// The one rule is that a plugin must not detach while another may have chained on top of it, which
// is why nothing here ever detaches.
#include <Plugin.hpp>
#include <Logger.h>
#include <Data.h>
#include <Yaml.h>

#include "../common/LogConfig.h"

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// windows.h defines min and max as macros. NOMINMAX cannot help here because the SDK headers
// arrive first through Data.h, so every use below is parenthesised - (std::max)(a, b) - which
// stops the macro expanding.

#include <detours.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // CVirtualMachine::m_sLastScriptRun, a CExoString, laid out as:
    //
    //   data +0x4f0 [sizeof=16] CExoString m_sLastScriptRun
    //     data +0x4f0 [sizeof=8]  char* m_sString
    //     data +0x4f8 [sizeof=4]  unsigned long m_nBufferLength
    //
    // This is only known to hold for the builds it was developed against. Everything that reads it
    // checks the result first and the plugin disables itself rather than guess - see
    // ReadScriptName.
    constexpr size_t kLastScriptRunString = 0x4F0;
    constexpr size_t kLastScriptRunLength = 0x4F8;

    // CVirtualMachine::RunScriptFile(int nScriptPart), the single point every script execution
    // passes through. RunScript and RunScriptSituation both call it, and nothing else does except
    // a test helper.
    //
    // The function prologue, which carries no addresses and so needs no wildcards. Verified unique
    // in two different Patch 3 server builds, at different addresses in each.
    constexpr const char* kRunScriptFilePattern =
        "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 "
        "48 83 EC 40 48 8B F1 44 8B A9 40 02 00 00";

    constexpr size_t kMaxCallDepth = 128;

    HMODULE GetOwnModule()
    {
        HMODULE hModule = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetOwnModule),
            &hModule);
        return hModule;
    }

    std::filesystem::path OwnPath()
    {
        return std::filesystem::path(GetFullModulePath(GetOwnModule()));
    }

    Logger& GetLogger()
    {
        static Logger logger(OwnPath().replace_extension(L".log"), true, true);
        return logger;
    }

    /// True if size bytes at address are committed and readable. Used before dereferencing an
    /// offset into an engine structure, since a wrong offset would otherwise fault the server.
    bool IsReadable(const void* address, size_t size)
    {
        if (!address)
        {
            return false;
        }

        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
        {
            return false;
        }

        constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                                 | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(info.Protect & readable) || (info.Protect & PAGE_GUARD))
        {
            return false;
        }

        // The region has to actually cover the whole span, not just its first byte.
        auto start = static_cast<const unsigned char*>(info.BaseAddress);
        auto end   = start + info.RegionSize;
        auto want  = static_cast<const unsigned char*>(address) + size;
        return want <= end;
    }

    enum class LogLevel { Nothing = 0, Stats = 1, Callstack = 2 };

    struct XpProfilerConfig
    {
        /// How much detail the profiler itself reports: none, stats or callstack.
        ///
        /// Named apart from log_level deliberately. NWNX4 had both `LogLevel` for this and
        /// `log_level` for logger verbosity, differing only in case, which is a trap.
        std::optional<std::string> report_detail;

        /// Whether resumed script parts (DelayCommand, the action queue) are timed separately.
        std::optional<bool> scriptparts;

        /// How often statistics are written. NWNX4 hardcoded 10 seconds.
        std::optional<int> flush_seconds;

        /// Log verbosity: none, error, warning, info, debug or trace. NWNX4's key and values.
        std::optional<std::string> log_level;

        /// Rotate the log past this size. Accepts a K, M or G suffix; 0 disables rotation.
        std::optional<std::string> log_max_file_size;
    };
}

/// <summary>Lets <c>YAML::Node</c> decode <see cref="XpProfilerConfig"/>.</summary>
template <>
struct YAML::convert<XpProfilerConfig>
{
    static bool decode(const Node& node, XpProfilerConfig& c)
    {
        if (!node.IsMap())
        {
            return false;
        }

        c.report_detail = node["report_detail"].as<std::optional<std::string>>(std::nullopt);
        c.scriptparts   = node["scriptparts"].as<std::optional<bool>>(std::nullopt);
        c.flush_seconds = node["flush_seconds"].as<std::optional<int>>(std::nullopt);
        c.log_level         = node["log_level"].as<std::optional<std::string>>(std::nullopt);
        c.log_max_file_size = node["log_max_file_size"].as<std::optional<std::string>>(std::nullopt);
        return true;
    }
};

namespace
{
    XpProfilerConfig LoadConfig()
    {
        auto path = OwnPath().replace_extension(L".yml");
        if (!std::filesystem::exists(path))
        {
            GetLogger()("no config at {}; using defaults.", path.string());
            return {};
        }

        auto config = Yaml::FromFile<XpProfilerConfig>(path);
        if (!config)
        {
            GetLogger()("could not read {}: {}. Using defaults.", path.string(), config.error());
            return {};
        }

        return *config;
    }

    /// Per-script totals: what this script has cost since the server started, and what it cost
    /// in the window since the last report.
    struct ScriptStats
    {
        uint64_t calls = 0;
        uint64_t microseconds = 0;
        uint64_t windowCalls = 0;
        uint64_t windowMicroseconds = 0;
    };

    /// One entry on the script call stack. Scripts run scripts, so this nests.
    struct Frame
    {
        std::string name;
        LARGE_INTEGER started{};
        bool counted = false;
    };

    class Profiler
    {
    public:
        void Configure(const XpProfilerConfig& config)
        {
            nwn2ports::ApplyLogConfig(GetLogger(), config);

            std::string detail = config.report_detail.value_or("stats");
            if (detail == "none")           { _logLevel = LogLevel::Nothing; }
            else if (detail == "stats")     { _logLevel = LogLevel::Stats; }
            else if (detail == "callstack") { _logLevel = LogLevel::Callstack; }
            else
            {
                GetLogger()(Logger::Level::Warning,
                            "report_detail '{}' is not none, stats or callstack; using stats.",
                            detail);
                _logLevel = LogLevel::Stats;
            }
            _timeScriptParts = config.scriptparts.value_or(true);
            _flushMicroseconds =
                static_cast<uint64_t>((std::max)(1, config.flush_seconds.value_or(10))) * 1000000ull;

            ::QueryPerformanceFrequency(&_frequency);
            ::QueryPerformanceCounter(&_lastFlush);
        }

        bool Enabled() const { return _logLevel != LogLevel::Nothing && !_disabled; }

        /// Called on the way into a script.
        void Enter(void* pVM, int scriptPart)
        {
            if (!Enabled())
            {
                return;
            }

            // A resumed part is a continuation of a script already counted, so timing it as its own
            // call double-counts unless the admin asked for it.
            if (scriptPart != 0 && !_timeScriptParts)
            {
                PushUncounted();
                return;
            }

            std::string name;
            if (!ReadScriptName(pVM, name) || name.empty())
            {
                PushUncounted();
                return;
            }

            if (_stack.size() >= kMaxCallDepth)
            {
                // Runaway recursion. Stop accounting rather than grow without bound.
                PushUncounted();
                return;
            }

            Frame frame;
            frame.name = std::move(name);
            frame.counted = true;
            ::QueryPerformanceCounter(&frame.started);

            if (_logLevel == LogLevel::Callstack)
            {
                GetLogger()("{:>{}}{} (depth {}{})", "", _stack.size() * 2, frame.name,
                            _stack.size(), scriptPart ? ", resumed part" : "");
            }

            _stack.push_back(std::move(frame));
        }

        /// Called on the way back out.
        void Leave()
        {
            if (_stack.empty())
            {
                return;
            }

            Frame frame = std::move(_stack.back());
            _stack.pop_back();

            if (!frame.counted)
            {
                return;
            }

            LARGE_INTEGER now;
            ::QueryPerformanceCounter(&now);

            // Scale before dividing so a sub-microsecond script does not truncate to nothing.
            uint64_t elapsed = static_cast<uint64_t>(
                ((now.QuadPart - frame.started.QuadPart) * 1000000) / _frequency.QuadPart);

            ScriptStats& stats = _stats[frame.name];
            stats.calls++;
            stats.microseconds += elapsed;
            stats.windowCalls++;
            stats.windowMicroseconds += elapsed;
            _totalCalls++;
            _totalMicroseconds += elapsed;

            // Only flush at the outermost level, so the log is not written from inside a nested
            // script execution.
            if (_stack.empty())
            {
                MaybeFlush(now);
            }
        }

    private:
        void PushUncounted()
        {
            if (_stack.size() < kMaxCallDepth)
            {
                _stack.push_back(Frame{});
            }
            else
            {
                // Still needs a frame or Leave() would unbalance the stack.
                _stack.push_back(Frame{});
            }
        }

        /// Reads CVirtualMachine::m_sLastScriptRun, refusing anything that does not look like a
        /// script name. A wrong offset on an unverified build would otherwise hand us garbage, or
        /// fault the server.
        bool ReadScriptName(void* pVM, std::string& out)
        {
            auto base = static_cast<const unsigned char*>(pVM);

            if (!IsReadable(base + kLastScriptRunString, sizeof(void*)) ||
                !IsReadable(base + kLastScriptRunLength, sizeof(uint32_t)))
            {
                Disable("the virtual machine does not have a readable CExoString at +0x4F0");
                return false;
            }

            const char* text = *reinterpret_cast<const char* const*>(base + kLastScriptRunString);
            uint32_t length  = *reinterpret_cast<const uint32_t*>(base + kLastScriptRunLength);

            if (!text)
            {
                return false;   // no script name yet; not an error
            }

            // A resref is 16 characters; the buffer around it is small. Anything wild here means
            // the offset is wrong for this build rather than that the script has a long name.
            if (length == 0 || length > 1024 || !IsReadable(text, length))
            {
                Disable("the script name at +0x4F0 has an implausible length");
                return false;
            }

            size_t used = ::strnlen(text, length);
            if (used == 0)
            {
                return false;   // empty script name, which the original also skipped
            }

            // Script names are resrefs: printable, no spaces or control characters.
            for (size_t i = 0; i < used; ++i)
            {
                unsigned char c = static_cast<unsigned char>(text[i]);
                if (c < 0x21 || c > 0x7E)
                {
                    Disable("the script name at +0x4F0 is not printable text");
                    return false;
                }
            }

            out.assign(text, used);
            return true;
        }

        /// Turns profiling off for the rest of the run. Something about this build does not match
        /// what the offset assumes, and guessing further risks the server.
        void Disable(const char* why)
        {
            if (_disabled)
            {
                return;
            }

            _disabled = true;
            GetLogger()("DISABLED: {}. The hook stays attached and harmless, but nothing is being "
                        "timed. This offset is only known to hold on the builds it was developed "
                        "against.", why);
        }

        void MaybeFlush(const LARGE_INTEGER& now)
        {
            uint64_t since = static_cast<uint64_t>(
                ((now.QuadPart - _lastFlush.QuadPart) * 1000000) / _frequency.QuadPart);

            if (since < _flushMicroseconds)
            {
                return;
            }

            _lastFlush = now;
            Flush(since);
        }

        void Flush(uint64_t sinceMicroseconds)
        {
            if (_stats.empty())
            {
                return;
            }

            // Every script seen since the server started, not just the ones that ran in this
            // window. Cumulative cost is the point of a profiler - a script that is cheap per call
            // but runs constantly only shows up in the running total. The window columns and the
            // * marker say what is active right now.
            std::vector<const std::pair<const std::string, ScriptStats>*> rows;
            rows.reserve(_stats.size());
            for (const auto& entry : _stats)
            {
                rows.push_back(&entry);
            }

            // Most expensive overall first.
            std::sort(rows.begin(), rows.end(), [](auto* a, auto* b) {
                return a->second.microseconds > b->second.microseconds;
            });

            size_t width = 7;   // at least as wide as the "script" heading
            for (const auto* row : rows)
            {
                width = (std::max)(width, row->first.size());
            }

            uint64_t windowMicroseconds = 0;
            uint64_t windowCalls = 0;
            size_t windowScripts = 0;
            for (const auto* row : rows)
            {
                windowMicroseconds += row->second.windowMicroseconds;
                windowCalls += row->second.windowCalls;
                if (row->second.windowCalls > 0)
                {
                    windowScripts++;
                }
            }

            GetLogger()("");
            GetLogger()("script statistics, last {} ms", sinceMicroseconds / 1000);
            GetLogger()("{:<{}}  {:>12} {:>8} {:>10}   {:>12} {:>8}",
                        "script", width, "total us", "calls", "us/call", "window us", "calls");
            GetLogger()("{:-<{}}", "", width + 56);

            for (auto* row : rows)
            {
                const ScriptStats& s = row->second;

                GetLogger()("{:<{}}  {:>12} {:>8} {:>10}   {:>12} {:>8} {}",
                            row->first, width,
                            s.microseconds, s.calls,
                            s.calls ? s.microseconds / s.calls : 0,
                            s.windowMicroseconds, s.windowCalls,
                            s.windowCalls > 0 ? "*" : "");
            }

            GetLogger()("{:-<{}}", "", width + 56);
            GetLogger()("since start: {} scripts, {} calls, {} ms  |  this window: {} scripts, "
                        "{} calls, {} ms",
                        rows.size(), _totalCalls, _totalMicroseconds / 1000,
                        windowScripts, windowCalls, windowMicroseconds / 1000);
            GetLogger()("");

            for (auto& entry : _stats)
            {
                entry.second.windowCalls = 0;
                entry.second.windowMicroseconds = 0;
            }
        }

        LogLevel _logLevel = LogLevel::Stats;
        bool _timeScriptParts = true;
        bool _disabled = false;
        uint64_t _flushMicroseconds = 10000000;

        LARGE_INTEGER _frequency{};
        LARGE_INTEGER _lastFlush{};

        std::vector<Frame> _stack;
        std::unordered_map<std::string, ScriptStats> _stats;
        uint64_t _totalCalls = 0;
        uint64_t _totalMicroseconds = 0;
    };

    Profiler& Instance()
    {
        static Profiler profiler;
        return profiler;
    }

    int(__fastcall* g_originalRunScriptFile)(void*, int) = nullptr;

    /// What the 32-bit version needed a naked assembly trampoline for.
    int __fastcall RunScriptFileHook(void* pVM, int nScriptPart)
    {
        Instance().Enter(pVM, nScriptPart);
        int result = g_originalRunScriptFile(pVM, nScriptPart);
        Instance().Leave();
        return result;
    }

    class XpProfiler : public nwn2::PluginBase
    {
    public:
        const char* GetPluginId() const override { return "PROFILER"; }

        void OnInitialize(nwn2::PluginHost host) override
        {
            Instance().Configure(LoadConfig());

            if (!Instance().Enabled())
            {
                GetLogger()("report_detail is none, so nothing will be hooked or timed.");
                return;
            }

            auto* addresses = host.QueryService<NWN2AddressService>();
            if (!addresses || !addresses->FindUnique)
            {
                GetLogger()("this loader does not provide IAddressService, so the script "
                            "chokepoint cannot be found.");
                return;
            }

            NWN2Result error{};
            void* target = addresses->FindUnique(addresses->self, kRunScriptFilePattern, &error);
            if (!target)
            {
                GetLogger()("could not find CVirtualMachine::RunScriptFile: {}",
                            error.message ? error.message : "no match");
                return;
            }

            GetLogger()("CVirtualMachine::RunScriptFile at 0x{:016X}",
                        reinterpret_cast<uintptr_t>(target));

            g_originalRunScriptFile = reinterpret_cast<int(__fastcall*)(void*, int)>(target);

            ::DetourTransactionBegin();
            ::DetourUpdateThread(::GetCurrentThread());
            ::DetourAttach(&(PVOID&)g_originalRunScriptFile, RunScriptFileHook);
            LONG attached = ::DetourTransactionCommit();

            if (attached != NO_ERROR)
            {
                GetLogger()("Detours failed to attach: {}", attached);
                g_originalRunScriptFile = nullptr;
                return;
            }

            GetLogger()("hooked. Statistics will be written here periodically.");
        }

        void OnNWNXGetString(const char* function, const char* param1, int32_t param2,
                             nwn2::StringResult& result) override
        {
            std::string_view fn(function ? function : "");

            if (fn == "GET SUBCLASS")         { result.Set("PROFILER"); }
            else if (fn == "GET VERSION")     { result.Set("1.0.0"); }
            else if (fn == "GET DESCRIPTION") { result.Set("This plugin times script execution."); }
        }

        void OnUnhandledException(const char* callback, const char* what) noexcept override
        {
            GetLogger()("exception escaping {}: {}", callback ? callback : "?", what ? what : "?");
        }
    };
}

NWN2_EXPORT_PLUGIN(XpProfiler)
