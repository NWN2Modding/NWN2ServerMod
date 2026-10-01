// xp_time, ported from NWNX4 to the NWN2ServerMod C ABI.
//
// Original: Copyright (C) 2007 Ingmar Stieger (Papillon), GPL v2 or later. This port keeps the
// behaviour and the script-facing contract identical; only the plugin interface changed.
//
// High-resolution named timers, driven entirely from NWScript:
//
//     NWNXSetString("TIME", "START", sName, 0, "");   start or restart a timer
//     NWNXGetString("TIME", "STOP",  sName, 0);       elapsed microseconds, and forget the timer
//     NWNXGetString("TIME", "QUERY", sName, 0);       elapsed microseconds, timer keeps running
//
// nwnx_time.nss appends ObjectToString(oObject) to the name, so timers are per object without the
// plugin needing to know anything about objects.
//
// What changed from the NWNX4 version:
//   - Plugin/DBPlugin subclass            -> nwn2::PluginBase
//   - GetFunctionClass("TIME")            -> GetPluginId()
//   - char* returnBuffer, MAX_BUFFER      -> nwn2::StringResult, which owns the storage
//   - LogNWNX                             -> Logger, writing next to this DLL
//   - std::string copies on every call    -> std::string_view, since none of them are stored
#include <Plugin.hpp>
#include <Logger.h>
#include <Data.h>

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace
{
    /// This DLL's own module handle, so the log lands next to the DLL rather than next to whatever
    /// process loaded it.
    HMODULE GetOwnModule()
    {
        HMODULE hModule = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetOwnModule),
            &hModule);
        return hModule;
    }

    Logger& GetLogger()
    {
        static Logger logger(
            std::filesystem::path(GetFullModulePath(GetOwnModule())).replace_extension(L".log"),
            true,
            true);
        return logger;
    }

    class XpTime : public nwn2::PluginBase
    {
    public:
        XpTime()
        {
            QueryPerformanceFrequency(&_frequency);
            GetLogger()("xp_time loaded. Timer resolution {} ticks/sec.", _frequency.QuadPart);
        }

        const char* GetPluginId() const override
        {
            return "TIME";
        }

        void OnNWNXSetString(const char* function, const char* param1, int param2, const char* value) override
        {
            std::string_view fn(function ? function : "");
            std::string_view name(param1 ? param1 : "");

            if (fn.empty() || name.empty())
            {
                GetLogger()("SetString: a function and a timer name are both required (got '{}' / '{}').", fn, name);
                return;
            }

            if (fn == "START")
            {
                Start(name);
            }
            else
            {
                GetLogger()("SetString: unknown function '{}'.", fn);
            }
        }

        void OnNWNXGetString(const char* function, const char* param1, int param2, nwn2::StringResult& result) override
        {
            std::string_view fn(function ? function : "");
            std::string_view name(param1 ? param1 : "");

            // The generic queries NWNX4's Plugin base class answered for every plugin. Kept so
            // existing scripts that ask do not start getting an empty string back.
            if (fn == "GET SUBCLASS")    { result.Set("TIME"); return; }
            if (fn == "GET VERSION")     { result.Set("0.0.2"); return; }
            if (fn == "GET DESCRIPTION") { result.Set("This plugin provides highly accurate timers."); return; }

            if (fn.empty() || name.empty())
            {
                GetLogger()("GetString: a function and a timer name are both required (got '{}' / '{}').", fn, name);
                return;
            }

            // Leaving result untouched is "no value", which reaches the script as "".
            if (fn == "STOP")
            {
                result.Set(std::to_string(Stop(name)).c_str());
            }
            else if (fn == "QUERY")
            {
                result.Set(std::to_string(Peek(name)).c_str());
            }
            else
            {
                GetLogger()("GetString: unknown function '{}'.", fn);
            }
        }

    private:
        void Start(std::string_view name)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            _timers[std::string(name)] = now;

            GetLogger()("START {}", name);
        }

        /// Elapsed microseconds, then forget the timer.
        long long Stop(std::string_view name)
        {
            long long elapsed = Peek(name);
            _timers.erase(std::string(name));

            GetLogger()("STOP  {} -> {} us", name, elapsed);
            return elapsed;
        }

        /// Elapsed microseconds, leaving the timer running. Zero for a timer that was never
        /// started, which is what the original returned too.
        long long Peek(std::string_view name)
        {
            auto it = _timers.find(std::string(name));
            if (it == _timers.end())
            {
                return 0;
            }

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);

            // Scale before dividing, so a sub-microsecond interval does not truncate to zero.
            return ((now.QuadPart - it->second.QuadPart) * 1000000) / _frequency.QuadPart;
        }

        LARGE_INTEGER _frequency{};
        std::unordered_map<std::string, LARGE_INTEGER> _timers;
    };
}

NWN2_EXPORT_PLUGIN(XpTime)
