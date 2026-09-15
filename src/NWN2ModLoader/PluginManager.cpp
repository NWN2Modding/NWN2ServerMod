#include "PluginManager.h"
#include "NWN2Mod.h"

PluginManager::~PluginManager()
{
    UnloadAll();
}

void PluginManager::LoadPlugins(NWN2Mod* host, const std::vector<std::string>& pluginPaths)
{
    for (const auto& path : pluginPaths)
    {
        HMODULE module = ::LoadLibraryW(ToWString(path).c_str());
        if (!module)
        {
            NWN2Mod::Log("Failed to load plugin '{}': {}", path, GetErrorMessage(::GetLastError()));
            continue;
        }

        auto createFunc = (CreatePluginFunc)::GetProcAddress(module, "CreatePlugin");
        auto destroyFunc = (DestroyPluginFunc)::GetProcAddress(module, "DestroyPlugin");
        if (!createFunc || !destroyFunc)
        {
            NWN2Mod::Log("Plugin '{}' is missing the CreatePlugin/DestroyPlugin exports.", path);
            ::FreeLibrary(module);
            continue;
        }

        IPlugin* instance = createFunc(host);
        if (!instance)
        {
            NWN2Mod::Log("Plugin '{}' CreatePlugin returned null.", path);
            ::FreeLibrary(module);
            continue;
        }

        std::string id = instance->GetPluginId();
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

void PluginManager::InitializeAll(NWN2Mod* host)
{
    for (auto& plugin : _loaded)
    {
        plugin.instance->OnInitialize(host);
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

IPlugin* PluginManager::FindById(const std::string& id) const
{
    auto it = _byId.find(id);
    return it != _byId.end() ? it->second : nullptr;
}
