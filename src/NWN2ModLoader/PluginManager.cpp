#include "PluginManager.h"
#include "NWN2Mod.h"

PluginManager::~PluginManager()
{
    UnloadAll();
}

void PluginManager::LoadPlugins(const NWN2_PluginHost* host, const std::vector<std::string>& pluginPaths)
{
    for (const auto& path : pluginPaths)
    {
        HMODULE module = ::LoadLibraryW(ToWString(path).c_str());
        if (!module)
        {
            NWN2Mod::Log("Failed to load plugin '{}': {}", path, GetErrorMessage(::GetLastError()));
            continue;
        }

        auto versionFunc = (NWN2_GetPluginAbiVersionFunc)::GetProcAddress(module, "GetPluginAbiVersion");
        auto createFunc = (NWN2_CreatePluginFunc)::GetProcAddress(module, "CreatePlugin");
        auto destroyFunc = (NWN2_DestroyPluginFunc)::GetProcAddress(module, "DestroyPlugin");
        if (!versionFunc || !createFunc || !destroyFunc)
        {
            NWN2Mod::Log("Plugin '{}' is missing the GetPluginAbiVersion/CreatePlugin/DestroyPlugin exports.", path);
            ::FreeLibrary(module);
            continue;
        }

        // The ABI version only moves when an existing field changes meaning, so it's a hard
        // mismatch. A plugin that simply predates a newly appended field still loads, and the
        // structSize checks at each call site cover the fields it doesn't have.
        uint32_t version = versionFunc();
        if (version != NWN2_PLUGIN_ABI_VERSION)
        {
            NWN2Mod::Log("Plugin '{}' was built against plugin ABI version {}, but this loader speaks version {}.",
                path, version, NWN2_PLUGIN_ABI_VERSION);
            ::FreeLibrary(module);
            continue;
        }

        NWN2_Plugin* instance = createFunc(host);
        if (!instance)
        {
            NWN2Mod::Log("Plugin '{}' CreatePlugin returned null.", path);
            ::FreeLibrary(module);
            continue;
        }

        // Everything else routes by plugin ID, so a plugin whose struct doesn't even reach
        // GetPluginId can't be used at all.
        if (!NWN2_HAS_FIELD(instance, NWN2_Plugin, GetPluginId) || !instance->GetPluginId)
        {
            NWN2Mod::Log("Plugin '{}' provides no GetPluginId (struct size {}). Skipping.", path, instance->structSize);
            destroyFunc(instance);
            ::FreeLibrary(module);
            continue;
        }

        const char* pluginId = instance->GetPluginId(instance->self);
        std::string id = pluginId ? pluginId : "";
        if (id.empty() || _byId.contains(id))
        {
            NWN2Mod::Log("Plugin '{}' has a missing or duplicate ID ('{}'). Skipping.", path, id);
            destroyFunc(instance);
            ::FreeLibrary(module);
            continue;
        }

        NWN2Mod::Log("Loaded plugin '{}' from '{}'.", id, path);

        _byId.emplace(id, instance);
        _loaded.push_back({ module, instance, destroyFunc });
    }
}

void PluginManager::InitializeAll(const NWN2_PluginHost* host)
{
    for (auto& plugin : _loaded)
    {
        if (NWN2_HAS_FIELD(plugin.instance, NWN2_Plugin, OnInitialize) && plugin.instance->OnInitialize)
        {
            plugin.instance->OnInitialize(plugin.instance->self, host);
        }
    }
}

void PluginManager::UnloadAll()
{
    for (auto& plugin : _loaded)
    {
        plugin.destroy(plugin.instance);
        ::FreeLibrary(plugin.module);
    }

    _loaded.clear();
    _byId.clear();
}

NWN2_Plugin* PluginManager::FindById(const std::string& id) const
{
    auto it = _byId.find(id);
    return it != _byId.end() ? it->second : nullptr;
}
