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

/// The real CServerExoAppInternal::InitializeNetLayer.
typedef void(__fastcall* InitializeNetLayerFunc)(void* pThis, bool);

/// The real CNWVirtualMachineCommands command-table initializer.
typedef void(__fastcall* InitializeCommandsFunc)(void* pThis);

/// The real CCampaignDB::SetBinaryData.
typedef bool(__fastcall* SetBinaryDataFunc)(
    void* pThisCampaignDB,
    CExoString* pCampNameExoStr,
    CExoString* pVarNameExoStr,
    CExoString* pPlayerExoStr,
    DataBlock* pDataBlock,
    uint16_t varType);

/// The real CCampaignDB::GetBinaryData.
typedef DataBlockPtr*(__fastcall* GetBinaryDataFunc)(
    void* pThisCampaignDB,
    DataBlockPtr* pRetVal,
    CExoString* pCampNameExoStr,
    CExoString* pVarNameExoStr,
    CExoString* pPlayerExoStr);

/// The real CNWSMessage::SendServerToPlayerChatMessage: the dispatcher player chat (Talk, Shout,
/// Whisper, Tell, Party and their DM variants) and server tells go through on their way to clients.
/// It is not the only way text reaches a player - NWScript's SendMessageToPC is delivered as a
/// feedback message instead, and a few engine paths call the per-channel senders directly.
///
/// Parameter names are the engine's own, from the server PDB. extraMessage is the exception: that
/// parameter is optimized out of the debug info, so the name is ours.
typedef int(__fastcall* SendServerToPlayerChatMessageFunc)(
    void* pThis,
    uint8_t nChatMessageType,
    uint32_t oidSpeaker,
    CExoString* sSpeakerMessage,
    uint32_t nTellPlayerId,
    void* pPlayerList,
    CExoString* extraMessage,
    bool bTriggerEvent);

/// The real CVirtualMachine::RunScript convenience overload, which always runs against the global
/// g_pVirtualMachine.
///
/// It is called as a plain function, not through a CVirtualMachine*: the first parameter is a dead
/// "this" slot the function overwrites with g_pVirtualMachine before ever reading it, so any value
/// is safe to pass for it.
typedef int(__fastcall* RunScriptFunc)(
    void* unusedThis,
    CExoString* pScriptExoStr,
    unsigned long objectId,
    int flag,
    int validation);

/// Owns the whole lifetime of the injected loader: finding and hooking the server's internal
/// functions, dispatching NWNX and campaign-object calls to plugins, and logging.
///
/// Plugins never see this class. They get HostAbi(), a plain C struct of function pointers, so a
/// plugin needs neither this class's real definition nor a C++ compiler that matches ours.
class NWN2Mod
{
public:
    /// Takes an already-parsed nwn2mod.config.
    NWN2Mod(const Config config)
        : _Config(config)
        , _NWVirtualMachineCommands(nullptr)
    {
        // The host API every plugin is handed. self is this loader; each entry is a static thunk
        // that recovers it and forwards to the matching member below.
        _HostAbi.structSize = sizeof(_HostAbi);
        _HostAbi.self = this;
        _HostAbi.GetPlugin = &HostGetPlugin;
        _HostAbi.RunScript = &HostRunScript;
        _HostAbi.RegisterChatHook = &HostRegisterChatHook;
        _HostAbi.QueryService = &HostQueryService;
        _HostAbi.GetCallingObject = &HostGetCallingObject;
    }

    // _HostAbi.self points at this object, so a copy would hand plugins a pointer to the original.
    NWN2Mod(const NWN2Mod&) = delete;
    NWN2Mod& operator=(const NWN2Mod&) = delete;

    /// Hooks the server's internal functions, then loads and initializes every configured plugin.
    /// Fails with a Win32-style error code.
    std::expected<void, uint32_t> Initialize();

    /// Loads the config at configPath, constructs Current, and initializes it. Fails with a
    /// Win32-style error code.
    static std::expected<void, uint32_t> Initialize(std::wstring_view configPath);

    /// The one loader instance for this process.
    static std::unique_ptr<NWN2Mod> Current;

    /// Logs through Current's logger at the given level.
    template <typename... Args>
    static void Log(Logger::Level level, std::format_string<Args...> fmt, Args&&... args)
    {
        Current->_Logger->log(level, fmt, std::forward<Args>(args)...);
    }

    /// Logs through Current's logger at Info.
    template <typename... Args>
    static void Log(std::format_string<Args...> fmt, Args&&... args)
    {
        Current->_Logger->log(Logger::Level::Info, fmt, std::forward<Args>(args)...);
    }

    /// The host API handed to every plugin, at load time and again at initialization.
    const NWN2PluginHost* HostAbi() const { return &_HostAbi; }

    /// A loaded plugin by ID, or null. Backs NWN2PluginHost::GetPlugin.
    NWN2Plugin* GetPlugin(const char* id) const
    {
        return _PluginManager.FindById(id ? id : "");
    }

    /// Runs a compiled script. Backs NWN2PluginHost::RunScript.
    bool RunScript(const char* script, uint32_t objectId) const;

    /// Registers the chat interceptor and returns the one registered before it. Backs
    /// NWN2PluginHost::RegisterChatHook.
    NWN2ChatHookFunc RegisterChatHook(NWN2ChatHookFunc hook);

    /// The object that the running script was called on. Backs NWN2PluginHost::GetCallingObject.
    uint32_t GetCallingObject() const;

private:
    static InitializeNetLayerFunc _InitializeNetLayer;
    static InitializeCommandsFunc _InitializeCommands;
    static SetBinaryDataFunc _SetBinaryData;
    static GetBinaryDataFunc _GetBinaryData;
    static RunScriptFunc _RunScript;
    static SendServerToPlayerChatMessageFunc _SendServerToPlayerChatMessage;

    /// The registered chat hook, or null if no plugin has registered one.
    static NWN2ChatHookFunc _ChatHook;

    /// The five NWN2PluginHost entry points. Each one recovers the loader from self and forwards to
    /// the matching member above. All of them are noexcept: an exception must never unwind out of
    /// this DLL into plugin code, which may have been built by an entirely different compiler.
    static NWN2Plugin* NWN2_CALL HostGetPlugin(void* self, const char* id) noexcept;
    static NWN2Result NWN2_CALL HostRunScript(void* self, const char* script, uint32_t objectId) noexcept;
    static NWN2ChatHookFunc NWN2_CALL HostRegisterChatHook(void* self, NWN2ChatHookFunc hook) noexcept;
    static void* NWN2_CALL HostQueryService(void* self, const char* versionedName) noexcept;
    static uint32_t NWN2_CALL HostGetCallingObject(void* self) noexcept;

    /// Writes the NWNX* handlers straight into the game's command table. NWN2Server calls them
    /// through the table by pointer, so they need no detour.
    void MapNWNXFunctions();

    /// Handler for a script's NWNXSetString call, wired into the command table.
    static void __cdecl NWNXSetString(const char *plugin, const char *function, const char *param1, int param2, const char *value);

    /// Handler for a script's NWNXSetInt call, wired into the command table.
    static void __cdecl NWNXSetInt(const char* plugin, const char* function, const char* param1, int param2, int value);

    /// Handler for a script's NWNXSetFloat call, wired into the command table.
    static void __cdecl NWNXSetFloat(const char* plugin, const char* function, const char* param1, int param2, float value);

    /// Handler for a script's NWNXGetString call. The returned pointer stays valid until the next
    /// call - the engine never modifies the string it is given, so one reused static buffer is safe
    /// - and is empty if no plugin answered.
    static const char * __cdecl NWNXGetString(const char* plugin, const char* function, const char* param1, int param2);

    /// Handler for a script's NWNXGetInt call. Returns 0 if no plugin answered, which the script
    /// cannot tell apart from a genuine 0.
    static int __cdecl NWNXGetInt(const char* plugin, const char* function, const char* param1, int param2);

    /// Handler for a script's NWNXGetFloat call. Returns 0.0 if no plugin answered, which the script
    /// cannot tell apart from a genuine 0.0.
    static float __cdecl NWNXGetFloat(const char* plugin, const char* function, const char* param1, int param2);

    /// Detour for CServerExoAppInternal::InitializeNetLayer: runs FinishInitialization, then chains
    /// to the real function.
    static void __fastcall HookInitializeNetLayer(void *pThis, bool param);

    /// Detour for the command-table initializer: saves pThis as _NWVirtualMachineCommands, then
    /// chains to the real function.
    static void __fastcall HookInitializeCommands(void* pThis);

    /// Detour for CCampaignDB::SetBinaryData: routes to the plugin whose ID matches the campaign
    /// name instead of calling through.
    static bool __fastcall HookSetBinaryData(void* pThisCampaignDB,
        CExoString* pCampNameExoStr,
        CExoString* pVarNameExoStr,
        CExoString* pPlayerExoStr,
        DataBlock* pDataBlock,
        uint16_t varType);

    /// Detour for CCampaignDB::GetBinaryData: routes to the plugin whose ID matches the campaign
    /// name instead of calling through.
    static DataBlockPtr* __fastcall HookGetBinaryData(void* pThisCampaignDB,
        DataBlockPtr* pRetVal,
        CExoString* pCampNameExoStr,
        CExoString* pVarNameExoStr,
        CExoString* pPlayerExoStr);

    /// Detour for CNWSMessage::SendServerToPlayerChatMessage: offers the message to the registered
    /// chat hook, if there is one, before deciding whether to call through.
    static int __fastcall HookSendServerToPlayerChatMessage(
        void* pThis,
        uint8_t nChatMessageType,
        uint32_t oidSpeaker,
        CExoString* sSpeakerMessage,
        uint32_t nTellPlayerId,
        void* pPlayerList,
        CExoString* extraMessage,
        bool bTriggerEvent);

    /// Resolves the absolute address a RIP-relative LEA or MOV refers to. instructionOffset and
    /// displacementOffset are measured from functionAddress; instructionLength is the whole
    /// instruction's size in bytes.
    static uintptr_t ExtractRipRelativeAddress(uintptr_t functionAddress,
        size_t instructionOffset,
        size_t displacementOffset,
        size_t instructionLength);

    /// Finds every hook target, checks the bytes it matched, and attaches all the Detours.
    std::expected<void, std::string> DoHooks();

    /// Finds CNWVirtualMachineCommands' command-table initializer by byte pattern.
    std::expected<void*, std::string> FindInitializeCommands();

    /// Finds CServerExoAppInternal::InitializeNetLayer by byte pattern.
    std::expected<void*, std::string> FindInitializeNetLayer();

    /// Finds CCampaignDB::SetBinaryData by byte pattern.
    std::expected<void*, std::string> FindSetBinaryData();

    /// Finds CCampaignDB::GetBinaryData by byte pattern.
    std::expected<void*, std::string> FindGetBinaryData();

    /// Finds CNWSMessage::SendServerToPlayerChatMessage by byte pattern.
    std::expected<void*, std::string> FindSendServerToPlayerChatMessage();

    /// Finds the instruction that writes the g_pVirtualMachine global, by byte pattern.
    std::expected<void*, std::string> FindVirtualMachineWrite();

    /// Finds the CVirtualMachine::RunScript convenience overload by byte pattern.
    std::expected<void*, std::string> FindRunScript();

    /// Runs once the command table is initialized: maps the NWNX functions and fixes up
    /// _VirtualMachine.
    void FinishInitialization();

    std::shared_ptr<Logger> _Logger;
    CNWVirtualMachineCommands *_NWVirtualMachineCommands;

    /// The real g_pVirtualMachine value, once FinishInitialization has fixed it up. Null before
    /// that, which RunScript treats as "the VM isn't ready yet".
    void *_VirtualMachine;

    Config _Config;
    PluginManager _PluginManager;

    /// The host API struct, filled in once by the constructor.
    NWN2PluginHost _HostAbi{};
};
