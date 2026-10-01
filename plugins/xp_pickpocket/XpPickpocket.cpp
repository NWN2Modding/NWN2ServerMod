// xp_pickpocket, ported from NWNX4 to the NWN2ServerMod C ABI.
//
// Original: by GodBeastX, GPL v2 or later.
//
// Lets a module script decide whether a pickpocket attempt is allowed. The plugin hooks the
// engine's pickpocket action, runs a script named in its config with OBJECT_SELF set to the
// pickpocketer, and either lets the action proceed or cancels it depending on what that script
// asked for.
//
//     #include "nwnx_pickpocket"
//
//     void main()
//     {
//         object oTarget = PickpocketGetTarget();
//         if (GetIsPC(oTarget)) { PickpocketCancel(); }
//     }
//
// What changed from the NWNX4 version:
//   Plugin subclass                  -> nwn2::PluginBase
//   xp_pickpocket.ini                -> xp_pickpocket.yml, matching nwn2mod.config
//   three hardcoded 32-bit addresses -> one byte pattern, and the loader's RunScript
//   a reconstructed struct offset     -> the engine's actual structure layout
//
// The original carried `CNWSCreature::AIActionPickPocket`, `CVirtualMachine::ExecuteScript` and
// `g_pVirtualMachine` as literal addresses, which pinned it to one build of one binary. Only the
// first is still needed here - the loader already exposes RunScript - and it is found by pattern.
//
// The struct offset is the more interesting change. The original read the target from a
// hand-reconstructed struct:
//
//     struct ActionNode { std::byte pad_0004[68]; NWN::OBJECTID targetObjectId; /* 0x044 */ };
//
// On x64 the real layout is not like that at all:
//
//     class CNWSObjectActionNode [sizeof = 168]
//       data +0x14 [sizeof=48] std::array<unsigned long,12> m_nParamType
//       data +0x48 [sizeof=96] std::array<void*,12>         m_pParameter
//
// The target is parameter 0, so this reads m_pParameter[0] rather than a magic offset.
#include <Plugin.hpp>
#include <Logger.h>
#include <Data.h>
#include <Yaml.h>

#include "../common/LogConfig.h"

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <format>
#include <string>
#include <string_view>

namespace
{
    // CNWSObjectActionNode::m_pParameter, an array of 12 void* holding the action's arguments.
    // See the note at the top of this file for where this comes from.
    constexpr size_t kActionNodeParameters = 0x48;

    // CGameObject::m_idSelf, which CNWSCreature inherits at offset 0. The pickpocketer's own id.
    constexpr size_t kGameObjectIdSelf = 0x98;

    // On what this plugin can and cannot do:
    //
    // CNWSCreature::AddPickPocketActions decides whether the pickpocket option is offered at all,
    // and it gates on six things - the asker can use Sleight of Hand, the target resolves to a
    // creature, the target is not a DM, the target is not the asker, the target has no master, and
    // the asker's m_bAbleToModifyActionQueue is set.
    //
    // That function was hooked during development and never fired once, including while
    // pickpocketing worked. The radial is built entirely client-side, so no server plugin - this
    // one or NWNX4's - can see or change what gets offered. It can only filter attempts the client
    // has already decided to make. The server's PvP setting is the usual reason an attempt never
    // reaches here: pickpocket is a hostile action, so a friendly target under Party PvP is
    // refused by the client with "You cannot perform that action on a friendly target".

    // CNWSCreature::AIActionPickPocket(CNWSObjectActionNode*). The function prologue, which
    // carries no addresses and so needs no wildcards. Verified unique in two different Patch 3
    // server builds, at different addresses in each.
    constexpr const char* kPickPocketPattern =
        "48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 10";

    // What AIActionPickPocket returns to abandon the action. Taken from the original, which
    // returned 3 when a script cancelled.
    constexpr uint32_t kActionResultAbort = 3;

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

    /// True if size bytes at address are committed and readable. The engine structures this reads
    /// only known to hold for the builds this was developed against, so a wrong offset must fail
    /// rather than fault the server.
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

        auto start = static_cast<const unsigned char*>(info.BaseAddress);
        auto want  = static_cast<const unsigned char*>(address) + size;
        return want <= start + info.RegionSize;
    }

    struct XpPickpocketConfig
    {
        /// The function class scripts call this plugin by.
        std::optional<std::string> plugin_class;

        /// The module script to run on every pickpocket attempt.
        std::optional<std::string> script;

        std::optional<std::string> log_level;
        std::optional<std::string> log_max_file_size;
    };
}

/// <summary>Lets <c>YAML::Node</c> decode <see cref="XpPickpocketConfig"/>.</summary>
template <>
struct YAML::convert<XpPickpocketConfig>
{
    static bool decode(const Node& node, XpPickpocketConfig& c)
    {
        if (!node.IsMap())
        {
            return false;
        }

        c.plugin_class      = node["class"].as<std::optional<std::string>>(std::nullopt);
        c.script            = node["script"].as<std::optional<std::string>>(std::nullopt);
        c.log_level         = node["log_level"].as<std::optional<std::string>>(std::nullopt);
        c.log_max_file_size = node["log_max_file_size"].as<std::optional<std::string>>(std::nullopt);
        return true;
    }
};

namespace
{
    XpPickpocketConfig LoadConfig()
    {
        auto path = OwnPath().replace_extension(L".yml");
        if (!std::filesystem::exists(path))
        {
            GetLogger()("no config at {}; using defaults.", path.string());
            return {};
        }

        auto config = Yaml::FromFile<XpPickpocketConfig>(path);
        if (!config)
        {
            GetLogger()("could not read {}: {}. Using defaults.", path.string(), config.error());
            return {};
        }

        return *config;
    }

    /// Read once; the base class needs the function class before the constructor body runs.
    const XpPickpocketConfig& Config()
    {
        static XpPickpocketConfig config = LoadConfig();
        return config;
    }

    /// The state a pickpocket attempt makes available to the script, for as long as that script is
    /// running. Script execution is single-threaded, and the hook does not re-enter itself, so a
    /// plain pair of values is enough - the original used the same approach.
    struct Attempt
    {
        uint32_t target = NWN2_OBJECT_INVALID;
        bool cancelled = false;
        bool inProgress = false;
    };

    Attempt g_attempt;
    std::string g_scriptName;
    nwn2::PluginHost g_host;

    uint32_t(__fastcall* g_originalPickPocket)(void*, void*) = nullptr;

    /// Reads the action's first parameter, which for a pickpocket is the target's object id.
    uint32_t ReadTarget(void* actionNode)
    {
        auto parameters = static_cast<const unsigned char*>(actionNode) + kActionNodeParameters;
        if (!IsReadable(parameters, sizeof(void*)))
        {
            return NWN2_OBJECT_INVALID;
        }

        // The parameters are stored as void*, with the id occupying the low half.
        auto raw = *reinterpret_cast<const uintptr_t*>(parameters);
        return static_cast<uint32_t>(raw);
    }

    /// Reads CGameObject::m_idSelf - the creature doing the pickpocketing.
    uint32_t ReadSelf(void* creature)
    {
        auto id = static_cast<const unsigned char*>(creature) + kGameObjectIdSelf;
        if (!IsReadable(id, sizeof(uint32_t)))
        {
            return NWN2_OBJECT_INVALID;
        }

        return *reinterpret_cast<const uint32_t*>(id);
    }

    uint32_t __fastcall PickPocketHook(void* creature, void* actionNode)
    {
        g_attempt.target = ReadTarget(actionNode);
        g_attempt.cancelled = false;
        g_attempt.inProgress = true;

        uint32_t self = ReadSelf(creature);

        // At Info rather than Debug: a pickpocket attempt is the event this plugin exists to
        // report, and a log nobody can see by default is how a broken hook stays hidden.
        GetLogger()("pickpocket: {:08X} -> {:08X}", self, g_attempt.target);

        // OBJECT_SELF inside the script is the pickpocketer, as it was in NWNX4.
        auto result = g_host.RunScript(g_scriptName.c_str(), self);
        if (!nwn2::Succeeded(result))
        {
            GetLogger()(Logger::Level::Warning, "could not run '{}': {}", g_scriptName,
                        result.message ? result.message : "unknown error");
        }

        bool cancelled = g_attempt.cancelled;
        g_attempt.inProgress = false;
        g_attempt.target = NWN2_OBJECT_INVALID;

        if (cancelled)
        {
            GetLogger()(Logger::Level::Debug, "pickpocket cancelled by script.");
            return kActionResultAbort;
        }

        return g_originalPickPocket(creature, actionNode);
    }

    class XpPickpocket : public nwn2::PluginBase
    {
    public:
        XpPickpocket() : _pluginId(Config().plugin_class.value_or("PICK")) {}

        const char* GetPluginId() const override { return _pluginId.c_str(); }

        void OnInitialize(nwn2::PluginHost host) override
        {
            nwn2ports::ApplyLogConfig(GetLogger(), Config());

            g_host = host;
            g_scriptName = Config().script.value_or("");

            // The shipped config ships with a placeholder, and a plugin that hooks an engine
            // function to run a script that does not exist is worse than one that does nothing.
            if (g_scriptName.empty() || g_scriptName == "<changeme>")
            {
                GetLogger()(Logger::Level::Warning,
                            "no script configured, so nothing is hooked. Set 'script' in "
                            "xp_pickpocket.yml to the module script that should decide whether a "
                            "pickpocket attempt is allowed.");
                return;
            }

            auto* addresses = host.QueryService<NWN2AddressService>();
            if (!addresses || !addresses->FindUnique)
            {
                GetLogger()(Logger::Level::Error,
                            "this loader does not provide IAddressService, so the pickpocket "
                            "action cannot be found.");
                return;
            }

            NWN2Result error{};
            void* target = addresses->FindUnique(addresses->self, kPickPocketPattern, &error);
            if (!target)
            {
                GetLogger()(Logger::Level::Error,
                            "could not find CNWSCreature::AIActionPickPocket: {}",
                            error.message ? error.message : "no match");
                return;
            }

            GetLogger()("CNWSCreature::AIActionPickPocket at 0x{:016X}",
                        reinterpret_cast<uintptr_t>(target));

            g_originalPickPocket = reinterpret_cast<uint32_t(__fastcall*)(void*, void*)>(target);

            ::DetourTransactionBegin();
            ::DetourUpdateThread(::GetCurrentThread());
            ::DetourAttach(&(PVOID&)g_originalPickPocket, PickPocketHook);
            LONG attached = ::DetourTransactionCommit();

            if (attached != NO_ERROR)
            {
                GetLogger()(Logger::Level::Error, "Detours failed to attach: {}", attached);
                g_originalPickPocket = nullptr;
                return;
            }

            GetLogger()("hooked. '{}' will run on every pickpocket attempt.", g_scriptName);

        }

        /// The script-facing surface, unchanged from NWNX4 so nwnx_pickpocket.nss still works:
        ///
        ///     NWNXGetInt(PICK, "0", "", 0)   the target's object id
        ///     NWNXGetInt(PICK, "1", "", n)   n != 0 cancels the attempt
        ///
        /// The numeric function names are the original's. Readable aliases are accepted too, since
        /// "1" says nothing about what it does.
        bool OnNWNXGetInt(const char* function, const char* param1, int32_t param2,
                          int32_t& outValue) override
        {
            std::string_view fn(function ? function : "");

            if (fn == "0" || fn == "GETTARGET")
            {
                if (!g_attempt.inProgress)
                {
                    GetLogger()(Logger::Level::Warning,
                                "the target was asked for outside a pickpocket attempt.");
                }

                outValue = static_cast<int32_t>(g_attempt.target);
                return true;
            }

            if (fn == "1" || fn == "SETCANCEL")
            {
                g_attempt.cancelled = (param2 != 0);
                outValue = g_attempt.cancelled ? 1 : 0;
                return true;
            }

            GetLogger()(Logger::Level::Warning, "GetInt: unknown function '{}'.", fn);
            return false;
        }

        void OnNWNXGetString(const char* function, const char* param1, int32_t param2,
                             nwn2::StringResult& result) override
        {
            std::string_view fn(function ? function : "");

            if (fn == "GET SUBCLASS")         { result.Set("PICK"); }
            else if (fn == "GET VERSION")     { result.Set("1.0.0"); }
            else if (fn == "GET DESCRIPTION") { result.Set("This plugin lets a script allow or deny pickpocketing."); }
        }

        void OnUnhandledException(const char* callback, const char* what) noexcept override
        {
            GetLogger()(Logger::Level::Error, "exception escaping {}: {}",
                        callback ? callback : "?", what ? what : "?");
        }

    private:
        std::string _pluginId;
    };
}

NWN2_EXPORT_PLUGIN(XpPickpocket)
