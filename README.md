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

## Running as a Windows service

NWN2ServerMod can install itself as a service, as NWNX4 did. The service starts the server, starts it again whenever it exits (a crash, a scheduled restart, a script calling shutdown), and on a service stop shuts it down cleanly before ending it.

Run these from a Command Prompt opened with **Run as administrator**:

```
NWN2ServerMod.exe -installservice -account <user> [-serviceno N] [config-path]
NWN2ServerMod.exe -startservice [-serviceno N]
NWN2ServerMod.exe -stopservice [-serviceno N]
NWN2ServerMod.exe -uninstallservice [-serviceno N]
```

- The service is named `NWN2ServerMod-N` (`-serviceno` defaults to 1). Several servers on one machine means several services, each with its own number and config.
- The config path is stored in the service when it is installed, so install again after moving it.
- `-account` is required: the Windows account the server normally runs under. The server reads its settings from that account's `Documents\Neverwinter Nights 2` and won't run as LocalSystem, even with `-home`. Windows needs the account's password for a service, so you're asked for it in a sign-in box (never on the command line), and the account is granted "Log on as a service".
- The service installs as Manual, like NWNX4, so a new install isn't live at the next reboot. Set it to Automatic in `services.msc` once it's tested.
- If the account's `nwn2player.ini` is missing when the service starts, it logs which account and path, and stops.
- The service's own log is `server_log` if set, otherwise `NWN2ServerMod-N.log` next to `NWN2ServerMod.exe`.

**Stopping.** With the [xp_srvadmin](plugins/README.md) plugin loaded, a service stop runs its whole shutdown sequence: the shutdown script, every player booted (saved, `OnClientLeave` run), then the server closes itself. Without it, the server's window is closed directly, as NWNX4 did. Either way, the server is ended if it is still running after `stop_timeout`. A Windows shutdown or reboot is handled the same way.

**Restarting.** If the server fails to start, or exits within five minutes of starting, five times in a row, the service stops rather than restarting it forever. The log says why.

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
log_player_identities: true
master_server: true
restart_on_exit: true
restart_delay: 5
stop_timeout: 180
temp_dir: C:/SomePath/To/Temp
cpu: 2
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

### fast_module_load (optional)

If `true`, the module is hard-linked into the server's working copy instead of being copied into it.
Defaults to `true`.

On startup the server copies the whole module into its `currentgame` directory - `CURRENTGAME.0` on
EE. For a directory-mode module of any size that dominates boot time. This is the behaviour of NWNX4's
`xp_fastboot` folded into the loader, intercepting `CopyFileW` and making a hard link instead, so no
bytes move.

The destination filter matches a path component of `currentgame` with an optional numeric suffix, so
both `currentgame` and `CURRENTGAME.0` are recognised. If a future server names it differently again,
the loader log says so with a sample path rather than silently copying - which is the failure mode this
went through once during development.

A link that cannot be made - both paths must be on one NTFS volume - falls back to a real copy and is
reported once. The server always boots; the only question is how long it takes.

### plugins (optional)

A list of full paths to plugin DLLs to load. Each is loaded once all of NWN2ModLoader's own hooks are attached, in the order listed. If omitted, no plugins are loaded.

### log_player_identities (optional)

If `true`, the loader writes one line per connecting player recording the platform identity the
client transmitted, and scripts can read the platform user id. Defaults to `true`. It only
observes: vault folders, names and logins are unchanged.

```
player identity: platform_name='Iowanscott' platform=Steam (83) platform_user_id=76561190000000000
modern_user_id='...' unique_id='a1b2c3...'
```

`platform_user_id` is the field worth having. EE replaced the pre-EE login name and CD key with a
`UserInfo` struct, and only four of its fields cross: the connect packet carries one
string, parsed back as `"%s.%s.%llu.%c"` - platform name, modern user id, platform user id,
platform tag. That makes the platform user id the only stable per-account identifier a server
receives, and the successor to `GetPCPublicCDKey()`. Scripts read it with:

```c
string sPlatformId = NWNXGetString("NWN2MOD", "PLATFORM_ID", GetPCPlayerName(oPC), 0);
```

It returns the id as a decimal string, `"0"` for a client that sent none (a legacy client), and `""`
if no player of that name has connected since the server started. `NWN2MOD` is reserved by the
loader for this; any other function name passes through to the plugins.

`GetPCPublicCDKey()` itself no longer returns anything. Nothing in the engine calls
`CNetLayerPlayerInfo::AddCDKey` or `SetCDKey` any more - both are dead code with no callers
anywhere in the image - so `m_lstKeys` is always empty, `GetNumCDKeys()` is always 0, and
`ExecuteCommandGetPCPublicCDKey` takes its empty-string branch for every player. **Any script that
keys accounts, bans or authentication on the public CD key is comparing empty strings on EE.**

The hook is on `UserInfo::EvaluateUniqueNameAndId`, found by a byte pattern that matches exactly
once. If the lookup fails the loader log says so and nothing else is affected.

### master_server (optional)

If `true`, the server checks in with the NWN2 master server (`nwn2.mst.valhallalegends.com`) so
NWNScry and the other server lists show it. Defaults to `true`; set it to `false` to keep the server
off the lists.

1.23 servers got this from xp_bugfix, which sent the server's own master server check-in to
Skywing's master. EE no longer sends that check-in, though it still answers the master's queries.
The loader sends it instead: the game port, every 5 minutes, from the first UDP port the server
binds. The master then queries the server directly, so that port has to be reachable from outside,
which it already is for players.

```
master_server: game port 5121; checking in with nwn2.mst.valhallalegends.com every 5 minutes.
```

### restart_on_exit (optional, service only)

If `true`, the service starts the server again whenever it exits. Defaults to `true`. If `false`, the service stops when the server does.

### restart_delay (optional, service only)

Seconds to wait after the server exits before starting it again. Defaults to `5`.

### stop_timeout (optional, service only)

Seconds a service stop waits for the server to shut down cleanly before ending it. Defaults to `180`, enough for xp_srvadmin's shutdown script, boots and drain. It also sets how long Windows waits for the service when the machine shuts down, so reinstall the service after changing it.

### temp_dir (optional)

Sets `TEMP` and `TMP` for the server, which is where it puts `CURRENTGAME` and `LOGS`. A service doesn't get these from a start script. Same as NWNX4's `nwn2temp`.

### cpu (optional)

The CPU cores the server runs on, numbered from 0: one number (`cpu: 2`) or a list (`cpu: [2, 3]`). Applied after the plugins load, on every start and restart, so it wins over a plugin that sets its own.

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
