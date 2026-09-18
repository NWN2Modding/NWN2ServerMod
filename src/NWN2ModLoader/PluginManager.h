#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include "PluginAbi.h"

/// Owns the loaded plugin DLLs: loading, initializing, looking up and unloading them.
class PluginManager
{
public:
    /// Unloads every plugin still loaded.
    ~PluginManager();

    /// Loads each DLL in pluginPaths, checks it was built against this plugin ABI, calls its
    /// CreatePlugin export with host, and indexes the result by its GetPluginId. A plugin that
    /// fails any of that is logged and skipped; the rest still load.
    void LoadPlugins(const NWN2_PluginHost* host, const std::vector<std::string>& pluginPaths);

    /// Calls OnInitialize on every loaded plugin that provides it. Runs after LoadPlugins has
    /// finished, so a plugin can look up any other plugin here regardless of load order.
    void InitializeAll(const NWN2_PluginHost* host);

    /// Calls DestroyPlugin on and frees every loaded plugin module.
    void UnloadAll();

    /// A loaded plugin by its ID, or null. Routes SetBinaryData/GetBinaryData by campaign name, and
    /// the NWNX* calls by their plugin argument.
    NWN2_Plugin* FindById(const std::string& id) const;

private:
    struct LoadedPlugin
    {
        HMODULE module;
        NWN2_Plugin* instance;
        NWN2_DestroyPluginFunc destroy;
    };

    std::vector<LoadedPlugin> _loaded;
    std::unordered_map<std::string, NWN2_Plugin*> _byId;
};
