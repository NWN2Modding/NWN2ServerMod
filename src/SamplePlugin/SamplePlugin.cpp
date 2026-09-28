// Sample nwn2::PluginBase implementation, showing the minimal shape of a NWN2ServerMod plugin:
// storage keyed by whatever the engine/script passed in, "not found" reported as
// false/untouched-result rather than thrown, and the NWN2_EXPORT_PLUGIN line the host requires.
#include <Plugin.hpp>
#include <Logger.h>
#include <Data.h>
#include <Yaml.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    /// Builds the map key for the NWNX* callbacks out of everything the script passed.
    std::string MakeKey(const char* function, const char* param1, int param2)
    {
        return std::string(function ? function : "") + "|" + std::string(param1 ? param1 : "") + "|" + std::to_string(param2);
    }

    /// This DLL's own module handle. Resolving an address inside this DLL, rather than calling
    /// GetModuleHandle(nullptr), is what identifies our own module instead of the host process's
    /// exe, whichever process loaded us.
    HMODULE GetOwnModule()
    {
        HMODULE hModule = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetOwnModule),
            &hModule);
        return hModule;
    }

    /// This plugin's shared logger. It writes next to this DLL, same path with a .log extension, so
    /// the log is easy to find whatever loaded the plugin and whatever its working directory is.
    Logger& GetLogger()
    {
        static Logger logger(
            std::filesystem::path(GetFullModulePath(GetOwnModule())).replace_extension(L".log"),
            true,
            true);
        return logger;
    }

    /// A minimal plugin: every callback is backed by an in-memory map and logged, which is the shape
    /// a real plugin follows.
    class SamplePlugin : public nwn2::PluginBase
    {
    public:
        /// Keeps the host API for later use. PluginHost is a small non-owning view, so storing it by
        /// value is the intended thing to do.
        explicit SamplePlugin(nwn2::PluginHost host) : _host(host)
        {
            GetLogger()("SamplePlugin created.");

            // Demonstrates Yaml.h: a plugin can load its own YAML config (here, an optional
            // string-to-string map from SamplePlugin.yaml next to this DLL) the same way
            // NWN2ServerMod loads nwn2mod.config, without linking against NWN2Shared at all.
            auto configPath = std::filesystem::path(GetFullModulePath(GetOwnModule())).replace_extension(L".yaml");
            if (std::filesystem::exists(configPath))
            {
                auto config = Yaml::FromFile<std::unordered_map<std::string, std::string>>(configPath);
                if (config)
                {
                    GetLogger()("Loaded {} entr{} from {}.", config->size(), config->size() == 1 ? "y" : "ies", ToString(configPath.wstring()));
                }
                else
                {
                    GetLogger()("Failed to load {}: {}", ToString(configPath.wstring()), config.error());
                }
            }
        }

        const char* GetPluginId() const override
        {
            return "Sample";
        }

        void OnInitialize(nwn2::PluginHost host) override
        {
            // Every plugin has finished loading by now, so looking a plugin up here (even this
            // one, just to prove the round trip works) is safe regardless of load order.
            NWN2Plugin* self = host.GetPlugin(GetPluginId());
            GetLogger()("OnInitialize() - host.GetPlugin(\"{}\") returned {}.",
                GetPluginId(), self == &Abi() ? "this plugin itself, as expected" : "something unexpected");

            // Return value would be the previous hook, but since we're just a sample, it'll be
            // ignored for this call. It can allow multiple plugins to hook chat though by chaining calls to previous hook.
            host.RegisterChatHook(&SamplePlugin::OnChat);
        }

        bool OnSetBinaryData(const char* varName, const char* player,
            const uint8_t* data, size_t size) override
        {
            GetLogger()("OnSetBinaryData(varName='{}', player='{}', size={})",
                varName ? varName : "", player ? player : "", size);

            _setBinaryCalls++;
            _binaryData[varName ? varName : ""] = std::vector<uint8_t>(data, data + size);
            return true;
        }

        void OnGetBinaryData(const char* varName, const char* player, nwn2::BinaryDataResult& result) override
        {
            GetLogger()("OnGetBinaryData(varName='{}', player='{}')",
                varName ? varName : "", player ? player : "");

            _getBinaryCalls++;
            auto it = _binaryData.find(varName ? varName : "");
            if (it == _binaryData.end())
            {
                return;
            }

            uint8_t* buffer = result.Allocate(it->second.size());
            if (!it->second.empty())
            {
                std::memcpy(buffer, it->second.data(), it->second.size());
            }
        }

        void OnNWNXSetString(const char* function, const char* param1, int param2, const char* value) override
        {
            GetLogger()("OnNWNXSetString(function='{}', param1='{}', param2={}, value='{}')",
                function ? function : "", param1 ? param1 : "", param2, value ? value : "");

            // One function name does something instead of just storing, to show what the stored host
            // API is for: NWNXSetString("Sample", "RunScript", "", 0, "myscript") runs myscript.
            if (function && std::strcmp(function, "RunScript") == 0)
            {
                NWN2Result ran = _host.RunScript(value, NWN2_OBJECT_INVALID);
                GetLogger()("RunScript('{}') -> {}", value ? value : "",
                    nwn2::Succeeded(ran) ? "ok" : (ran.message ? ran.message : "failed"));
                return;
            }

            _strings[MakeKey(function, param1, param2)] = value ? value : "";
        }

        void OnNWNXSetInt(const char* function, const char* param1, int param2, int value) override
        {
            GetLogger()("OnNWNXSetInt(function='{}', param1='{}', param2={}, value={})",
                function ? function : "", param1 ? param1 : "", param2, value);

            _ints[MakeKey(function, param1, param2)] = value;
        }

        void OnNWNXSetFloat(const char* function, const char* param1, int param2, float value) override
        {
            GetLogger()("OnNWNXSetFloat(function='{}', param1='{}', param2={}, value={})",
                function ? function : "", param1 ? param1 : "", param2, value);

            _floats[MakeKey(function, param1, param2)] = value;
        }

        void OnNWNXGetString(const char* function, const char* param1, int param2, nwn2::StringResult& result) override
        {
            GetLogger()("OnNWNXGetString(function='{}', param1='{}', param2={})",
                function ? function : "", param1 ? param1 : "", param2);

            auto it = _strings.find(MakeKey(function, param1, param2));
            if (it != _strings.end())
            {
                result.Set(it->second.c_str());
            }
        }

        bool OnNWNXGetInt(const char* function, const char* param1, int param2, int& outValue) override
        {
            GetLogger()("OnNWNXGetInt(function='{}', param1='{}', param2={}) caller=0x{:08X}",
                function ? function : "", param1 ? param1 : "", param2, _host.GetCallingObject());

            // Two reserved names report this plugin's own callback counters. A script can read one
            // either side of a call to prove the callback actually reached the plugin - which
            // StoreCampaignObject's return value cannot show, since the engine answers it the same
            // way when no loader is attached.
            if (function && std::strcmp(function, "__stat_setbinary") == 0)
            {
                outValue = _setBinaryCalls;
                return true;
            }

            if (function && std::strcmp(function, "__stat_getbinary") == 0)
            {
                outValue = _getBinaryCalls;
                return true;
            }

            auto it = _ints.find(MakeKey(function, param1, param2));
            if (it == _ints.end())
            {
                return false;
            }
            outValue = it->second;
            return true;
        }

        bool OnNWNXGetFloat(const char* function, const char* param1, int param2, float& outValue) override
        {
            GetLogger()("OnNWNXGetFloat(function='{}', param1='{}', param2={})",
                function ? function : "", param1 ? param1 : "", param2);

            auto it = _floats.find(MakeKey(function, param1, param2));
            if (it == _floats.end())
            {
                return false;
            }

            outValue = it->second;
            return true;
        }

        /// The chat hook has to be static, since the C ABI passes no context pointer with it.
        /// Returning true here would swallow the message; a sample should never do that.
        static bool OnChat(uint8_t mode, uint32_t speakerId, const char* message, uint32_t tellPlayerId)
        {
            GetLogger()("OnChat(mode={}, speakerId={:#x}, tellPlayerId={:#x}): '{}'",
                (uint32_t)mode, speakerId, tellPlayerId, message ? message : "");
            return false;
        }
    private:
        nwn2::PluginHost _host;
        int _setBinaryCalls = 0;
        int _getBinaryCalls = 0;
        std::unordered_map<std::string, std::vector<uint8_t>> _binaryData;
        std::unordered_map<std::string, std::string> _strings;
        std::unordered_map<std::string, int> _ints;
        std::unordered_map<std::string, float> _floats;
    };
}

NWN2_EXPORT_PLUGIN(SamplePlugin)
