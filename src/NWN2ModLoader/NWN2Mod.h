#pragma once
#include <expected>
#include <memory>
#include <string>
#include <cinttypes>
#include <NWN2Shared.h>
#include "VirtualMachineCommands.h"
#include "DataBlock.h"
#include "CExoString.h"
#include "PluginManager.h"

/// <summary>The real engine's <c>CServerExoAppInternal::InitializeNetLayer</c>.</summary>
typedef void(__fastcall* InitializeNetLayerFunc)(void* pThis, bool);

/// <summary>The real engine's <c>CNWVirtualMachineCommands</c> command-table initializer.</summary>
typedef void(__fastcall* InitializeCommandsFunc)(void* pThis);

/// <summary>The real engine's <c>CCampaignDB::SetBinaryData</c>.</summary>
typedef bool(__fastcall* SetBinaryDataFunc)(
    void* pThisCampaignDB,
    CExoString* pCampNameExoStr,
    CExoString* pVarNameExoStr,
    CExoString* pPlayerExoStr,
    DataBlock* pDataBlock,
    uint16_t varType);

/// <summary>The real engine's <c>CCampaignDB::GetBinaryData</c>.</summary>
typedef DataBlockPtr*(__fastcall* GetBinaryDataFunc)(
    void* pThisCampaignDB,
    DataBlockPtr* pRetVal,
    CExoString* pCampNameExoStr,
    CExoString* pVarNameExoStr,
    CExoString* pPlayerExoStr);

/// <summary>
/// The real engine's <c>CNWSMessage::SendServerToPlayerChatMessage</c>: the single function every
/// chat message (Talk/Shout/Whisper/Tell/Party and their DM variants) funnels through before NWN2
/// sends it to any client.
/// </summary>
typedef int(__fastcall* SendServerToPlayerChatMessageFunc)(
    void* pThis,
    uint8_t mode,
    uint32_t senderId,
    CExoString* message,
    uint32_t targetId,
    void* clientList,
    CExoString* extraMessage,
    bool runScriptFlag);

/// <summary>
/// The real engine's <c>CVirtualMachine::RunScript(CExoString*, unsigned long, int, PARAMTER_VALIDATION)</c>
/// convenience overload, which always runs against the global <c>g_pVirtualMachine</c>.
/// </summary>
/// <remarks>
/// This is called as a plain function, not through a <c>CVirtualMachine*</c>: the first parameter
/// is a dead "this" slot the function itself overwrites with <c>g_pVirtualMachine</c> before ever
/// reading it, so any value (including <see langword="nullptr"/>) is safe to pass for it.
/// </remarks>
typedef int(__fastcall* RunScriptFunc)(
    void* unusedThis,
    CExoString* pScriptExoStr,
    unsigned long objectId,
    int flag,
    int validation);

/// <summary>
/// Owns the whole lifetime of the injected loader: locating and hooking the server's internal
/// functions, dispatching NWNX/campaign-object calls to plugins, and logging.
/// </summary>
/// <remarks>
/// Publicly inherits <see cref="IPluginHost"/> so a plugin can call <see cref="GetPlugin"/> through
/// that small vtable interface (see <see cref="PluginManager::LoadPlugins"/>/
/// <see cref="PluginManager::InitializeAll"/>), without needing this class's real (much heavier)
/// definition, or to link against NWN2ModLoader.lib at all.
/// </remarks>
class NWN2Mod : public IPluginHost
{
public:
    /// <summary>Constructs the loader with an already-loaded config.</summary>
    /// <param name="config">The parsed <c>nwn2mod.config</c>.</param>
    NWN2Mod(const Config config)
        : _Config(config)
        , _NWVirtualMachineCommands(nullptr)
    {
    }

    /// <summary>Hooks the target process's internal functions and loads/initializes all configured plugins.</summary>
    /// <returns>An unexpected Win32-style error code on failure.</returns>
    std::expected<void, uint32_t> Initialize();

    /// <summary>Loads the config at <paramref name="configPath"/>, constructs <see cref="Current"/>, and initializes it.</summary>
    /// <param name="configPath">The full path to the config file to load.</param>
    /// <returns>An unexpected Win32-style error code on failure.</returns>
    static std::expected<void, uint32_t> Initialize(std::wstring_view configPath);

    /// <summary>The single loader instance for this process, constructed by <see cref="Initialize(std::wstring_view)"/>.</summary>
    static std::unique_ptr<NWN2Mod> Current;

    /// <summary>Logs a message at the given level through <see cref="Current"/>'s logger.</summary>
    /// <param name="level">The severity to log at.</param>
    /// <param name="fmt">A <c>std::format</c> format string.</param>
    /// <param name="args">The format arguments.</param>
    template <typename... Args>
    static void Log(Logger::Level level, std::format_string<Args...> fmt, Args&&... args)
    {
        Current->_Logger->log(level, fmt, std::forward<Args>(args)...);
    }

    /// <summary>Logs an informational message through <see cref="Current"/>'s logger.</summary>
    /// <param name="fmt">A <c>std::format</c> format string.</param>
    /// <param name="args">The format arguments.</param>
    template <typename... Args>
    static void Log(std::format_string<Args...> fmt, Args&&... args)
    {
        Current->_Logger->log(Logger::Level::Info, fmt, std::forward<Args>(args)...);
    }

    /// <summary>See <see cref="IPluginHost::GetPlugin"/>.</summary>
    IPlugin* GetPlugin(const char* id) const override
    {
        return _PluginManager.FindById(id ? id : "");
    }

    /// <summary>See <see cref="IPluginHost::RunScript"/>.</summary>
    bool RunScript(const char* script, uint32_t objectId) const override;

    /// <summary>See <see cref="IPluginHost::RegisterChatHook"/>.</summary>
    ChatHookFunc RegisterChatHook(ChatHookFunc hook) override;

private:
    static InitializeNetLayerFunc _InitializeNetLayer;
    static InitializeCommandsFunc _InitializeCommands;
    static SetBinaryDataFunc _SetBinaryData;
    static GetBinaryDataFunc _GetBinaryData;
    static RunScriptFunc _RunScript;
    static SendServerToPlayerChatMessageFunc _SendServerToPlayerChatMessage;

    /// <summary>The currently registered chat hook, or <see langword="nullptr"/> if none is (see <see cref="RegisterChatHook"/>).</summary>
    static ChatHookFunc _ChatHook;

    /// <summary>Patches the <c>NWNX*</c> command function pointers directly into the game's command table.</summary>
    /// <remarks>NWN2Server calls these through the table by pointer, so no detouring is needed for them.</remarks>
    void MapNWNXFunctions();

    /// <summary>Handler for a script's <c>NWNXSetString</c> call, wired directly into the command table.</summary>
    static void __cdecl NWNXSetString(const char *plugin, const char *function, const char *param1, int param2, const char *value);

    /// <summary>Handler for a script's <c>NWNXSetInt</c> call, wired directly into the command table.</summary>
    static void __cdecl NWNXSetInt(const char* plugin, const char* function, const char* param1, int param2, int value);

    /// <summary>Handler for a script's <c>NWNXSetFloat</c> call, wired directly into the command table.</summary>
    static void __cdecl NWNXSetFloat(const char* plugin, const char* function, const char* param1, int param2, float value);

    /// <summary>Handler for a script's <c>NWNXGetString</c> call, wired directly into the command table.</summary>
    /// <returns>
    /// A pointer valid until the next call to this function - the engine never modifies the
    /// string it's given, so a single reused static buffer is safe here. Empty if no plugin is
    /// registered for <paramref name="plugin"/>, or its <c>OnNWNXGetString</c> left the result untouched.
    /// </returns>
    static const char * __cdecl NWNXGetString(const char* plugin, const char* function, const char* param1, int param2);

    /// <summary>Handler for a script's <c>NWNXGetInt</c> call, wired directly into the command table.</summary>
    /// <returns>
    /// <c>0</c> if no plugin is registered for <paramref name="plugin"/>, or its
    /// <c>OnNWNXGetInt</c> returned <see langword="false"/>. The script has no way to distinguish
    /// this from a genuine value of <c>0</c>.
    /// </returns>
    static int __cdecl NWNXGetInt(const char* plugin, const char* function, const char* param1, int param2);

    /// <summary>Handler for a script's <c>NWNXGetFloat</c> call, wired directly into the command table.</summary>
    /// <returns>
    /// <c>0.0</c> if no plugin is registered for <paramref name="plugin"/>, or its
    /// <c>OnNWNXGetFloat</c> returned <see langword="false"/>. The script has no way to distinguish
    /// this from a genuine value of <c>0.0</c>.
    /// </returns>
    static float __cdecl NWNXGetFloat(const char* plugin, const char* function, const char* param1, int param2);

    /// <summary>Detour target for <c>CServerExoAppInternal::InitializeNetLayer</c>; runs <see cref="FinishInitialization"/> then chains to the real function.</summary>
    static void __fastcall HookInitializeNetLayer(void *pThis, bool param);

    /// <summary>Detour target for the command-table initializer; captures <c>pThis</c> as <see cref="_NWVirtualMachineCommands"/> then chains to the real function.</summary>
    static void __fastcall HookInitializeCommands(void* pThis);

    /// <summary>Detour target for <c>CCampaignDB::SetBinaryData</c>; routes to the plugin whose ID matches the campaign name instead of calling through.</summary>
    static bool __fastcall HookSetBinaryData(void* pThisCampaignDB,
        CExoString* pCampNameExoStr,
        CExoString* pVarNameExoStr,
        CExoString* pPlayerExoStr,
        DataBlock* pDataBlock,
        uint16_t varType);

    /// <summary>Detour target for <c>CCampaignDB::GetBinaryData</c>; routes to the plugin whose ID matches the campaign name instead of calling through.</summary>
    static DataBlockPtr* __fastcall HookGetBinaryData(void* pThisCampaignDB,
        DataBlockPtr* pRetVal,
        CExoString* pCampNameExoStr,
        CExoString* pVarNameExoStr,
        CExoString* pPlayerExoStr);

    /// <summary>
    /// Detour target for <c>CNWSMessage::SendServerToPlayerChatMessage</c>; offers the message to
    /// <see cref="_ChatHook"/> (if one is registered) before deciding whether to call through.
    /// </summary>
    static int __fastcall HookSendServerToPlayerChatMessage(
        void* pThis,
        uint8_t mode,
        uint32_t senderId,
        CExoString* message,
        uint32_t targetId,
        void* clientList,
        CExoString* extraMessage,
        bool runScriptFlag);

    /// <summary>Resolves the absolute target address of a RIP-relative <c>LEA</c>/<c>MOV</c> instruction.</summary>
    /// <param name="functionAddress">The base address the other offsets are relative to.</param>
    /// <param name="instructionOffset">The offset of the instruction from <paramref name="functionAddress"/>.</param>
    /// <param name="displacementOffset">The offset of the instruction's 4-byte signed displacement operand.</param>
    /// <param name="instructionLength">The total length of the instruction, in bytes.</param>
    /// <returns>The absolute address the instruction refers to.</returns>
    static uintptr_t ExtractRipRelativeAddress(uintptr_t functionAddress,
        size_t instructionOffset,
        size_t displacementOffset,
        size_t instructionLength);

    /// <summary>Locates every hook target, validates the matched bytes, and attaches all Detours.</summary>
    std::expected<void, std::string> DoHooks();

    /// <summary>Locates <c>CNWVirtualMachineCommands</c>'s command-table initializer by byte pattern.</summary>
    std::expected<void*, std::string> FindInitializeCommands();

    /// <summary>Locates <c>CServerExoAppInternal::InitializeNetLayer</c> by byte pattern.</summary>
    std::expected<void*, std::string> FindInitializeNetLayer();

    /// <summary>Locates <c>CCampaignDB::SetBinaryData</c> by byte pattern.</summary>
    std::expected<void*, std::string> FindSetBinaryData();

    /// <summary>Locates <c>CCampaignDB::GetBinaryData</c> by byte pattern.</summary>
    std::expected<void*, std::string> FindGetBinaryData();

    /// <summary>Locates <c>CNWSMessage::SendServerToPlayerChatMessage</c> by byte pattern.</summary>
    std::expected<void*, std::string> FindSendServerToPlayerChatMessage();

    /// <summary>Locates the instruction that writes the <c>g_pVirtualMachine</c> global, by byte pattern.</summary>
    std::expected<void*, std::string> FindVirtualMachineWrite();

    /// <summary>Locates the <c>CVirtualMachine::RunScript</c> convenience overload by byte pattern.</summary>
    std::expected<void*, std::string> FindRunScript();

    /// <summary>Runs once the command table is initialized: maps NWNX functions and fixes up <see cref="_VirtualMachine"/>.</summary>
    void FinishInitialization();

    std::shared_ptr<Logger> _Logger;
    CNWVirtualMachineCommands *_NWVirtualMachineCommands;

    /// <summary>
    /// The real <c>g_pVirtualMachine</c> value, once <see cref="FinishInitialization"/> has fixed it up.
    /// Null beforehand, which <see cref="RunScript"/> uses as its "the VM isn't ready yet" check.
    /// </summary>
    void *_VirtualMachine;

    Config _Config;
    PluginManager _PluginManager;
};
