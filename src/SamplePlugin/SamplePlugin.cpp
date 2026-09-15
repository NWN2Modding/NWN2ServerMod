// Sample IPlugin implementation, showing the minimal shape of a NWN2ServerMod plugin:
// storage keyed by whatever the engine/script passed in, "not found" reported as
// false/untouched-result rather than thrown, and the two extern "C" exports the host requires.
#include <Plugin.h>
#include <Logger.h>
#include <Data.h>
#include <Yaml.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    /// <summary>Builds the map key used for the <c>NWNX*</c> callbacks.</summary>
    /// <param name="function">The function name the script passed.</param>
    /// <param name="param1">The first parameter the script passed.</param>
    /// <param name="param2">The second parameter the script passed.</param>
    std::string MakeKey(const char* function, const char* param1, int param2)
    {
        return std::string(function ? function : "") + "|" + std::string(param1 ? param1 : "") + "|" + std::to_string(param2);
    }

    /// <summary>Gets this DLL's own module handle.</summary>
    /// <remarks>
    /// Resolving an address inside this DLL (rather than <c>GetModuleHandle(nullptr)</c>, which
    /// would give the *host process's* exe) is what identifies "our own module" regardless of
    /// which process loads us.
    /// </remarks>
    HMODULE GetOwnModule()
    {
        HMODULE hModule = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetOwnModule),
            &hModule);
        return hModule;
    }

    /// <summary>
    /// Gets this plugin's shared logger, writing to a file next to this DLL (same path, <c>.log</c>
    /// extension) so the log is easy to find regardless of what process loaded the plugin or what
    /// its working directory is.
    /// </summary>
    Logger& GetLogger()
    {
        static Logger logger(
            std::filesystem::path(GetFullModulePath(GetOwnModule())).replace_extension(L".log"),
            true,
            true);
        return logger;
    }

    /// <summary>
    /// Minimal <see cref="IPlugin"/> implementation: every callback is backed by an in-memory map
    /// and logged, demonstrating the shape a real plugin follows.
    /// </summary>
    class SamplePlugin : public IPlugin
    {
    public:
        /// <summary>Constructs the plugin, keeping <paramref name="host"/> for later use.</summary>
        /// <param name="host">The host API passed to this DLL's exported <c>CreatePlugin</c>.</param>
        explicit SamplePlugin(IPluginHost* host) : _host(host)
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

        void OnInitialize(IPluginHost* host) override
        {
            // Every plugin has finished loading by now, so looking a plugin up here (even this
            // one, just to prove the round trip works) is safe regardless of load order.
            IPlugin* self = host->GetPlugin(GetPluginId());
            GetLogger()("OnInitialize() - host->GetPlugin(\"{}\") returned {}.",
                GetPluginId(), self == this ? "this plugin itself, as expected" : "something unexpected");

            // Return value would be the previous hook, but since we're just a sample, it'll be
            // ignored for this call. It can allow multiple plugins to hook chat though by chaining calls to previous hook.
            host->RegisterChatHook(&SamplePlugin::OnChat);
        }

        bool OnSetBinaryData(const char* varName, const char* player,
            const uint8_t* data, size_t size) override
        {
            GetLogger()("OnSetBinaryData(varName='{}', player='{}', size={})",
                varName ? varName : "", player ? player : "", size);

            _binaryData[varName] = std::vector<uint8_t>(data, data + size);
            return true;
        }

        void OnGetBinaryData(const char* varName, const char* player, IBinaryDataResult& result) override
        {
            GetLogger()("OnGetBinaryData(varName='{}', player='{}')",
                varName ? varName : "", player ? player : "");

            auto it = _binaryData.find(varName);
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

            _strings[MakeKey(function, param1, param2)] = value;
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

        void OnNWNXGetString(const char* function, const char* param1, int param2, IStringResult& result) override
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
            GetLogger()("OnNWNXGetInt(function='{}', param1='{}', param2={})",
                function ? function : "", param1 ? param1 : "", param2);

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

        static bool OnChat(uint8_t mode, uint32_t senderId, const char* message, uint32_t targetId)
        {
            GetLogger()("OnChat({}): '{}'", (uint32_t)mode, message);
            return false;
        }
    private:
        IPluginHost* _host;
        std::unordered_map<std::string, std::vector<uint8_t>> _binaryData;
        std::unordered_map<std::string, std::string> _strings;
        std::unordered_map<std::string, int> _ints;
        std::unordered_map<std::string, float> _floats;
    };
}

/// <summary>See <see cref="CreatePluginFunc"/>.</summary>
extern "C" __declspec(dllexport) IPlugin* CreatePlugin(IPluginHost* host)
{
    return new SamplePlugin(host);
}

/// <summary>See <see cref="DestroyPluginFunc"/>.</summary>
extern "C" __declspec(dllexport) void DestroyPlugin(IPlugin* plugin)
{
    delete plugin;
}
