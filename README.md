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

Open the repository folder in Visual Studio and build the `clang-cl-release` configuration (or `msvc-release` for MSVC), or from a Developer Command Prompt:

```
cmake --preset clang-cl
cmake --build --preset clang-cl-release
```

Output binaries land in `build\clang-cl\bin\Release\`.

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

A plugin is a DLL that exports three `extern "C"` functions:

```cpp
extern "C" __declspec(dllexport) uint32_t GetPluginAbiVersion(void);
extern "C" __declspec(dllexport) NWN2Plugin* CreatePlugin(const NWN2PluginHost* host);
extern "C" __declspec(dllexport) void DestroyPlugin(NWN2Plugin* plugin);
```

In C++ you never write those by hand — `NWN2_EXPORT_PLUGIN(MyPlugin)` at the end of the file generates all three.

The loader reads `GetPluginAbiVersion` first and skips any plugin built against a different ABI. `CreatePlugin` is then called once at load time with the host API, and `DestroyPlugin` on shutdown, so the plugin's own module frees what it allocated.

`nwn2::PluginBase` (`src/NWN2Plugin/Plugin.hpp`) is the class a plugin derives from. It's the one header a plugin author needs, and every method is documented in place there — this README only covers what's needed to get oriented:

- `GetPluginId()` — a short, stable string ID for the plugin, used to route calls to it. Must be unique among loaded plugins.
- `OnInitialize(host)` — called once per plugin after every configured plugin has finished loading; the right place to look up other plugins via `host.GetPlugin`.
- `OnNWNXSetString` / `OnNWNXSetInt` / `OnNWNXSetFloat` / `OnNWNXGetString` / `OnNWNXGetInt` / `OnNWNXGetFloat` — a script's `NWNXSetString`/etc. call with this plugin's ID as the `plugin` argument.
- `OnSetBinaryData` / `OnGetBinaryData` — a script's `StoreCampaignObject`/`RetrieveCampaignObject` call with this plugin's ID as the campaign name. NWN2ServerMod never touches the engine's own campaign DB for these calls — a plugin owns that storage entirely.

All methods except `GetPluginId` have no-op default implementations, so a plugin only needs to override what it actually uses.

`nwn2::PluginHost` (also `Plugin.hpp`) is passed to the constructor and again to `OnInitialize`. It's a small non-owning view, so keeping a copy of it by value is the intended thing to do:

- `host.GetPlugin(id)` — another loaded plugin by ID, or `nullptr` if none is loaded with that ID. Call it through its own function pointers; its `self` belongs to another DLL and means nothing in yours.
- `host.RunScript(script, objectId)` — runs a compiled script (a `.ncs` resref) immediately against `objectId`, like NWScript's own `ExecuteScript`. Bare `void main()` scripts only. `NWN2_OBJECT_INVALID` is available for `objectId` when no target object is needed. Returns an `NWN2Result`, which `nwn2::Succeeded(result)` tests.
- `host.GetCallingObject()` — the object the running script was called on, like NWScript's `OBJECT_SELF`. Only meaningful inside a callback from the loader; `NWN2_OBJECT_INVALID` otherwise.
- `host.RegisterChatHook(hook)` — intercepts chat on its way to players and returns whatever hook was registered before, so hooks can chain. Returning `true` swallows the message and stops the module's `OnChat` event firing for it. The hook has to be a plain function rather than a member, since the ABI carries no context pointer alongside it.

  It covers player chat (talk, shout, whisper, tell, party) and server tells. It does **not** cover NWScript's `SendMessageToPC`, which the engine delivers as a feedback message rather than chat. Note also that the speaker ID is `0x7FFFFFFF` for player chat and so cannot identify who spoke, and that the fourth argument is a player index that only means anything for a tell — see `NWN2ChatHookFunc` in `PluginAbi.h`.
- `host.QueryService<T>()` — a versioned loader service by name, or `nullptr` if this loader doesn't have it. Nothing offers a service yet; this is how new host APIs will arrive without changing the structs above.

The boundary between a plugin and the loader is plain C (`src/NWN2Plugin/PluginAbi.h`): structs of function pointers, not C++ vtables. So a plugin does **not** have to be built with the same compiler, C++ standard version, or CRT as `NWN2ModLoader.dll` — MSVC, clang, MinGW, or a plugin written in plain C all work. Exceptions never cross the boundary in either direction either: `Plugin.hpp` stops them on the plugin side and hands them to `OnUnhandledException`.

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

Any type with a `YAML::convert<T>` specialization works, including plain `std::map`/`std::vector`/etc. that yaml-cpp already knows how to convert — `src/SamplePlugin/SamplePlugin.cpp` demonstrates loading an optional `std::unordered_map<std::string, std::string>` from `SamplePlugin.yaml` next to the DLL with no custom `convert` needed. A plugin project needs yaml-cpp's headers on its include path and `YAML_CPP_STATIC_DEFINE` defined; linking the `yaml-cpp` CMake target does both, as `src/SamplePlugin/CMakeLists.txt` shows. `Yaml.h` pulls in `yaml-cpp.lib` itself via `#pragma comment(lib, ...)`.

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

The other arguments (`sVarName`, `sFunction`/`sParam1`/`nParam2`) are opaque, plugin-defined keys forwarded straight through unmodified — see `Plugin.hpp`'s comments on each `On...` method for exactly what they mean and what a "not found" result looks like to the calling script.

`SamplePlugin` treats one function name specially rather than just storing it, to show what the host API is for:

```nwscript
NWNXSetString("Sample", "RunScript", "", 0, "myscript");
```

`src/SamplePlugin/` is a complete, minimal reference implementation (ID `"Sample"`) that backs every callback with an in-memory map and logs each call it receives to a `.log` file next to `SamplePlugin.dll`. It builds as part of the CMake project alongside the other targets.

# Contributing

See CONTRIBUTING.md for information.
