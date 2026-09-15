# NWN2ServerMod
A Windows mod loader for Neverwinter Nights 2: Enhanced Edition's dedicated server. It launches `NWN2Server64.exe`, injects a loader DLL, and lets you write plugins that hook into campaign-object storage (`StoreCampaignObject`/`RetrieveCampaignObject`) and NWNX-style get/set calls from NWScript, and run scripts on demand.

## Requirements
- Visual Studio 2026
- Neverwinter Nights 2: Enhanced Edition (Legacy Not Supported)
- 64-Bit Only

## Languages / Libraries Used
- Windows API
- [C++23 Standard](https://cppreference.com/cpp/23)

## Building

Open `NWN2ServerMod.slnx` in Visual Studio and build the `x64`/`Release` configuration, or from a Developer Command Prompt:

```
msbuild NWN2ServerMod.slnx /p:Configuration=Release /p:Platform=x64
```

Output binaries land in `bin\x64\Release\`.

## Running

Run `NWN2ServerMod.exe`, pointing it at a config file (see below). It starts `NWN2Server64.exe`, injects the loader DLL and any configured plugins, then lets the server run normally.

## Configuration Path

NWN2ServerMod will load the configuration file `nwn2mod.config` in the executables directory (Not the working directory).

The path to the configuration may be optionally provided as a command-line argument.

```
NWN2ServerMod.exe <config-path>
```

## Configuration Format

The configuration is a YAML file which is described below.

**NOTE:** Only UTF-8 format is supported. Lines starting with `#` are comments.

```yaml
server_exe: C:/SomePath/To/NWN2Server64.exe
server_args: -moduledir <Module>
server_directory: C:\SomePath\To\Working\Directory\
loader_dll: C:/SomePath/To/NWN2ModLoader.dll
loader_log: C:/SomePath/To/LogFile.txt
server_log: C:/SomePath/To/ServerLog.txt
debug_log: true
std_log: false
plugins:
  - C:/SomePath/To/MyPlugin.dll
```

### server_exe (Required)

The full path to the `NWN2Server64.exe` executable.

### server_args (Required)

The arguments passed to `NWN2Server64.exe` when executed.

### server_directory (optional)

The optional working directory of `NWN2Server64.exe`. This defaults to the parent path of `server_exe`.

### loader_dll (optional)

The optional full path to the `NWN2ModLoader.dll`. This defaults to `NWN2ModLoader.dll` in the same parent path as `NWN2ServerMod.exe`.

### loader_log (optional)

The optional full path to a log file written to by `NWN2ModLoader.dll`. This will contain errors and information about the server. If omitted there will be no log file created.

### server_log (optional)

The optional full path to a log file written to by `NWN2ServerMod.exe`. This will contain errors and information about the startup of the NWN2 server modding before hooking occurs and after. If omitted there will be no log file created.

### debug_log (optional)

If `true` then logs will additionally output with `OutputDebugString`. Defaults to `false`.

### std_log (optional)

If `true` then logs will additionally output to stdout and stderr. Defaults to `true`.

### plugins (optional)

A list of full paths to plugin DLLs to load. Each is loaded once all of NWN2ModLoader's own hooks are attached, in the order listed. If omitted, no plugins are loaded.

## Plugins

A plugin is a DLL that exports two `extern "C"` functions:

```cpp
extern "C" __declspec(dllexport) IPlugin* CreatePlugin(IPluginHost* host);
extern "C" __declspec(dllexport) void DestroyPlugin(IPlugin* plugin);
```

`CreatePlugin` is called once at load time with an `IPluginHost*`, so the plugin can call its API (e.g. looking up other plugins) as needed. `DestroyPlugin` is called on shutdown so the plugin's own module frees the object it allocated.

`IPlugin` (`src/NWN2Plugin/Plugin.h`) is the interface a plugin implements. It's the one header a plugin author needs, and every method's parameters and return values are fully documented in place there via XML doc comments — this README only covers what's needed to get oriented:

- `GetPluginId()` — a short, stable string ID for the plugin, used to route calls to it. Must be unique among loaded plugins.
- `OnInitialize(host)` — called once per plugin after every configured plugin has finished loading; the right place to look up other plugins via `IPluginHost::GetPlugin`.
- `OnNWNXSetString` / `OnNWNXSetInt` / `OnNWNXSetFloat` / `OnNWNXGetString` / `OnNWNXGetInt` / `OnNWNXGetFloat` — a script's `NWNXSetString`/etc. call with this plugin's ID as the `plugin` argument.
- `OnSetBinaryData` / `OnGetBinaryData` — a script's `StoreCampaignObject`/`RetrieveCampaignObject` call with this plugin's ID as the campaign name. NWN2ServerMod never touches the engine's own campaign DB for these calls — a plugin owns that storage entirely.

All methods except `GetPluginId` have no-op default implementations, so a plugin only needs to override what it actually uses.

`IPluginHost` (also `Plugin.h`) is passed in as `host`:

- `host->GetPlugin(id)` — looks up another loaded plugin by ID, or `nullptr` if none is loaded with that ID.
- `host->RunScript(script, objectId)` — runs a compiled script (a `.ncs` resref) immediately against `objectId`, like NWScript's own `ExecuteScript`. Bare `void main()` scripts only; returns `false` if the script couldn't run or the engine's VM isn't ready yet. `NWScriptObject::OBJECT_INVALID` is available for `objectId` when no target object is needed.

Build a plugin with the same toolset as `NWN2ModLoader.dll` (currently `v145`) so the `IPlugin`/`IPluginHost` vtables line up between them.

### Loading YAML from a Plugin

`src/NWN2Plugin/Yaml.h` (a copy of `NWN2Shared`'s own header, so a plugin never needs to link against `NWN2Shared`) exposes the same YAML loading NWN2ServerMod uses for `nwn2mod.config`, for a plugin's own config:

```cpp
#include <Yaml.h>

struct MyPluginConfig
{
    std::string someSetting;
    std::optional<int> someOptionalSetting;
};

template <>
struct YAML::convert<MyPluginConfig>
{
    static bool decode(const Node& node, MyPluginConfig& c)
    {
        if (!node.IsMap()) return false;
        c.someSetting = node["someSetting"].as<std::string>();
        c.someOptionalSetting = node["someOptionalSetting"].as<std::optional<int>>();
        return true;
    }
};

std::expected<MyPluginConfig, std::string> config = Yaml::FromFile<MyPluginConfig>(path);
```

Any type with a `YAML::convert<T>` specialization works, including plain `std::map`/`std::vector`/etc. that yaml-cpp already knows how to convert — `src/SamplePlugin/SamplePlugin.cpp` demonstrates loading an optional `std::unordered_map<std::string, std::string>` from `SamplePlugin.yaml` next to the DLL with no custom `convert` needed. A plugin project needs yaml-cpp's headers on its include path and `YAML_CPP_STATIC_DEFINE` defined (see `SamplePlugin.vcxproj`); `Yaml.h` pulls in `yaml-cpp.lib` itself via `#pragma comment(lib, ...)`.

### Calling a Plugin from NWScript

A script reaches a plugin through NWScript's existing `StoreCampaignObject`/`RetrieveCampaignObject` and `NWNX*` functions — NWN2ServerMod adds no new script functions. The plugin is selected by matching `sCampaignName`/`sPlugin` against its `GetPluginId()`:

```nwscript
StoreCampaignObject("Sample", "Sword", oSword, oPC);
object oSwordGotten = RetrieveCampaignObject("Sample", "Sword", GetLocation(oPC));

NWNXSetFloat("Sample", "Float", "", 0, 11.0);
NWNXSetString("Sample", "String", "", 0, "StringValue");
NWNXSetInt("Sample", "Int", "", 0, 33);
float fValue = NWNXGetFloat("Sample", "Float", "", 0);
string sValue = NWNXGetString("Sample", "String", "", 0);
int iValue = NWNXGetInt("Sample", "Int", "", 0);
```

The other arguments (`sVarName`, `sFunction`/`sParam1`/`nParam2`) are opaque, plugin-defined keys forwarded straight through unmodified — see `Plugin.h`'s doc comments on each `On...` method for exactly what they mean and what a "not found" result looks like to the calling script.

`src/SamplePlugin/` is a complete, minimal `IPlugin` reference implementation (ID `"Sample"`) that backs every callback with an in-memory map and logs each call it receives to a `.log` file next to `SamplePlugin.dll`. It builds as part of the solution alongside the other projects.

# Contributing

See CONTRIBUTING.md for information.