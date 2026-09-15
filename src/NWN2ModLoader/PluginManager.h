#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include "Plugin.h"

class NWN2Mod;

/// <summary>
/// Owns the set of loaded plugin DLLs: loading, initializing, looking up, and unloading them.
/// </summary>
class PluginManager
{
public:
    /// <summary>Unloads every plugin still loaded (see <see cref="UnloadAll"/>).</summary>
    ~PluginManager();

    /// <summary>
    /// Loads each DLL in <paramref name="pluginPaths"/>, calls its <c>CreatePlugin</c> export with
    /// <paramref name="host"/>, and indexes the result by <see cref="IPlugin::GetPluginId"/>.
    /// </summary>
    /// <param name="host">The host API passed to each plugin's <c>CreatePlugin</c>.</param>
    /// <param name="pluginPaths">The full paths of the plugin DLLs to load, in order.</param>
    /// <remarks>Failures for an individual plugin are logged and skipped rather than aborting the whole batch.</remarks>
    void LoadPlugins(NWN2Mod* host, const std::vector<std::string>& pluginPaths);

    /// <summary>Calls <see cref="IPlugin::OnInitialize"/> on every loaded plugin.</summary>
    /// <param name="host">The host API passed to each plugin's <c>OnInitialize</c>.</param>
    /// <remarks>
    /// Called once, after <see cref="LoadPlugins"/> has finished loading all of them, so every
    /// plugin can already see every other plugin via <see cref="FindById"/>/<c>GetPlugin</c>
    /// regardless of load order.
    /// </remarks>
    void InitializeAll(NWN2Mod* host);

    /// <summary>Calls <c>DestroyPlugin</c> on and frees every loaded plugin module.</summary>
    void UnloadAll();

    /// <summary>
    /// Looks up a plugin by its <see cref="IPlugin::GetPluginId"/>.
    /// </summary>
    /// <param name="id">The plugin ID to look up.</param>
    /// <returns>The matching plugin instance, or <see langword="nullptr"/> if none is loaded with that ID.</returns>
    /// <remarks>Used to route <c>SetBinaryData</c>/<c>GetBinaryData</c> calls by campaign name, and <c>NWNX*</c> calls by their explicit plugin argument.</remarks>
    IPlugin* FindById(const std::string& id) const;

private:
    struct LoadedPlugin
    {
        HMODULE module;
        IPlugin* instance;
        DestroyPluginFunc destroy;
    };

    std::vector<LoadedPlugin> _loaded;
    std::unordered_map<std::string, IPlugin*> _byId;
};
