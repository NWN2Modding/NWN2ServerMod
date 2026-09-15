#include "NWN2Mod.h"
#include "PEPattern.h"
#include <detours.h>
#include <cstring>
#include <cstdlib>
#include <new>
#include "Commands.h"

std::unique_ptr<NWN2Mod> NWN2Mod::Current;

InitializeNetLayerFunc NWN2Mod::_InitializeNetLayer;
InitializeCommandsFunc NWN2Mod::_InitializeCommands;
SetBinaryDataFunc NWN2Mod::_SetBinaryData;
GetBinaryDataFunc NWN2Mod::_GetBinaryData;
RunScriptFunc NWN2Mod::_RunScript;
SendServerToPlayerChatMessageFunc NWN2Mod::_SendServerToPlayerChatMessage;
ChatHookFunc NWN2Mod::_ChatHook = nullptr;

std::expected<void, uint32_t> NWN2Mod::Initialize()
{
    _Logger = std::make_shared<Logger>(
        _Config.loader_log.transform([](auto v) { return::ToWString(v); }),
        _Config.std_log.value_or(true),
        _Config.debug_log.value_or(false));

    Log("*** Initializing NWN2 Hooks ***");

    auto hooksResult = DoHooks();
    if (!hooksResult)
    {
        Log("Failed to hook functions: {}", hooksResult.error());

        return std::unexpected(ERROR_HOOK_NOT_INSTALLED);
    }

    // All hooks are attached at this point, so it's safe for plugins to start receiving calls.
    _PluginManager.LoadPlugins(this, _Config.plugins.value_or(std::vector<std::string>{}));

    // Every plugin is loaded by this point, so it's safe for OnInitialize to look up any other
    // plugin regardless of load order.
    _PluginManager.InitializeAll(this);

    Log("Initializing complete.");

    return {};
}

std::expected<void, uint32_t> NWN2Mod::Initialize(std::wstring_view configPath)
{
    auto configResult = Config::FromFile(configPath);
    if (!configResult)
    {
        // Can't log this error since we have no config loaded.
        return std::unexpected(ERROR_INVALID_PARAMETER);
    }

    auto config = *configResult;

    Current = std::make_unique<NWN2Mod>(config);
    return Current->Initialize();
}
std::expected<void, std::string> NWN2Mod::DoHooks()
{
    auto initCommands = FindInitializeCommands();
    if (!initCommands)
    {
        return std::unexpected(std::format("Failed to find InitializeCommands: {}", initCommands.error()));
    }

    auto initNet = FindInitializeNetLayer();
    if (!initNet)
    {
        return std::unexpected(std::format("Failed to find InitializeNetLayer: {}", initNet.error()));
    }

    auto virtualMachine = FindVirtualMachineWrite();
    if (!virtualMachine)
    {
        return std::unexpected(std::format("Failed to find g_pVirtualMachine: {}", virtualMachine.error()));
    }

    auto setBinaryData = FindSetBinaryData();
    if (!setBinaryData)
    {
        return std::unexpected(std::format("Failed to find SetBinaryData: {}", setBinaryData.error()));
    }

    auto getBinaryData = FindGetBinaryData();
    if (!getBinaryData)
    {
        return std::unexpected(std::format("Failed to find GetBinaryData: {}", getBinaryData.error()));
    }

    auto runScript = FindRunScript();
    if (!runScript)
    {
        return std::unexpected(std::format("Failed to find RunScript: {}", runScript.error()));
    }

    auto sendChat = FindSendServerToPlayerChatMessage();
    if (!sendChat)
    {
        return std::unexpected(std::format("Failed to find SendServerToPlayerChatMessage: {}", sendChat.error()));
    }

    // Sanity check
    uint8_t *pBuffer = (uint8_t *)initNet.value();
    if (pBuffer[0] != 0x48)
    {
        return std::unexpected(std::format("Found invalid address for InitializeNetLayer: 0x{:016X} Data:0x{:02X}", (uint64_t)pBuffer, *pBuffer));
    }

    pBuffer = (uint8_t*)initCommands.value();
    if (pBuffer[0] != 0x48)
    {
        return std::unexpected(std::format("Found invalid address for InitializeCommands: 0x{:016X} Data:0x{:02X}", (uint64_t)pBuffer, *pBuffer));
    }

    pBuffer = (uint8_t*)setBinaryData.value();
    if (pBuffer[0] != 0x48)
    {
        return std::unexpected(std::format("Found invalid address for SetBinaryData: 0x{:016X} Data:0x{:02X}", (uint64_t)pBuffer, *pBuffer));
    }

    pBuffer = (uint8_t*)getBinaryData.value();
    if (pBuffer[0] != 0x48)
    {
        return std::unexpected(std::format("Found invalid address for GetBinaryData: 0x{:016X} Data:0x{:02X}", (uint64_t)pBuffer, *pBuffer));
    }

    pBuffer = (uint8_t*)runScript.value();
    if (pBuffer[0] != 0x48)
    {
        return std::unexpected(std::format("Found invalid address for RunScript: 0x{:016X} Data:0x{:02X}", (uint64_t)pBuffer, *pBuffer));
    }

    pBuffer = (uint8_t*)sendChat.value();
    if (pBuffer[0] != 0x48)
    {
        return std::unexpected(std::format("Found invalid address for SendServerToPlayerChatMessage: 0x{:016X} Data:0x{:02X}", (uint64_t)pBuffer, *pBuffer));
    }
    // Should be initialized to 0x0000000000000000
    if (*((uintptr_t*)virtualMachine.value()) != 0x0000000000000000)
    {
        return std::unexpected(std::format("Found invalid address for g_pVirtualMachine: 0x{:016X} Data:0x{:016X}", ((uint64_t)virtualMachine.value()), *((uint64_t*)virtualMachine.value())));
    }

    _InitializeCommands = (InitializeCommandsFunc)initCommands.value();
    _InitializeNetLayer = (InitializeNetLayerFunc)initNet.value();
    _SetBinaryData = (SetBinaryDataFunc)setBinaryData.value();
    _GetBinaryData = (GetBinaryDataFunc)getBinaryData.value();
    _RunScript = (RunScriptFunc)runScript.value();
    _SendServerToPlayerChatMessage = (SendServerToPlayerChatMessageFunc)sendChat.value();

    _VirtualMachine = virtualMachine.value(); // This is the address of the global. We'll fix it to the value of the global later. It lasts until server shutdown.

    // Populate the static variables with the hex addresses you found
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourAttach(&(PVOID&)_InitializeNetLayer, &HookInitializeNetLayer);
    DetourAttach(&(PVOID&)_InitializeCommands, &HookInitializeCommands);
    DetourAttach(&(PVOID&)_SetBinaryData, &HookSetBinaryData);
    DetourAttach(&(PVOID&)_GetBinaryData, &HookGetBinaryData);
    DetourAttach(&(PVOID&)_SendServerToPlayerChatMessage, &HookSendServerToPlayerChatMessage);

    LONG error = DetourTransactionCommit();
    if (error != NO_ERROR)
    {
        // Return the error state if Detours fails
        return std::unexpected(std::format("Detours failed to attach. Error code: {}", error));
    }

    return {};
}

void __cdecl NWN2Mod::NWNXSetString(const char* plugin, const char* function, const char* param1, int param2, const char* value)
{
    IPlugin* target = Current->_PluginManager.FindById(plugin);
    if (!target)
    {
        NWN2Mod::Log("NWNXSetString: no plugin registered for '{}'.", plugin);
        return;
    }

    target->OnNWNXSetString(function, param1, param2, value);
}

void __cdecl NWN2Mod::NWNXSetInt(const char* plugin, const char* function, const char* param1, int param2, int value)
{
    IPlugin* target = Current->_PluginManager.FindById(plugin);
    if (!target)
    {
        NWN2Mod::Log("NWNXSetInt: no plugin registered for '{}'.", plugin);
        return;
    }

    target->OnNWNXSetInt(function, param1, param2, value);
}

void __cdecl NWN2Mod::NWNXSetFloat(const char* plugin, const char* function, const char* param1, int param2, float value)
{
    IPlugin* target = Current->_PluginManager.FindById(plugin);
    if (!target)
    {
        NWN2Mod::Log("NWNXSetFloat: no plugin registered for '{}'.", plugin);
        return;
    }

    target->OnNWNXSetFloat(function, param1, param2, value);
}

namespace
{
    /// <summary>
    /// Host-side implementation of <see cref="IStringResult"/>, passed to a plugin's
    /// <c>OnNWNXGetString</c> by reference.
    /// </summary>
    /// <remarks>
    /// Backed by a <c>std::string</c> that lives entirely in this DLL - the plugin never sees or
    /// touches it directly, only the pure interface, so this doesn't reintroduce STL-across-ABI
    /// concerns.
    /// </remarks>
    class StringResult : public IStringResult
    {
    public:
        char* Allocate(size_t length) override
        {
            _value.assign(length, '\0');
            _handled = true;

            return _value.data();
        }

        void Set(const char* value) override
        {
            _value = value;
            _handled = true;
        }

        void Clear() override
        {
            _value.clear();
            _handled = false;
        }

        bool Handled() const { return _handled; }
        const char* CStr() const { return _value.c_str(); }

    private:
        std::string _value;
        bool _handled = false;
    };
}

const char *__cdecl NWN2Mod::NWNXGetString(const char* plugin, const char* function, const char* param1, int param2)
{
    // Note: The returned string is NOT manipulated in any way from NWN2Server64. Therefore we can reuse a single
    // static result across calls; its backing buffer just needs to outlive this function, not this specific call.
    static StringResult result;
    result.Clear();

    IPlugin* target = Current->_PluginManager.FindById(plugin);
    if (target)
    {
        target->OnNWNXGetString(function, param1, param2, result);
    }

    if (result.Handled())
    {
        return result.CStr();
    }

    NWN2Mod::Log("NWNXGetString: no plugin registered for '{}', or it had no value.", plugin);
    return "";
}

int __cdecl NWN2Mod::NWNXGetInt(const char* plugin, const char* function, const char* param1, int param2)
{
    int value = 0;
    IPlugin* target = Current->_PluginManager.FindById(plugin);
    if (target && target->OnNWNXGetInt(function, param1, param2, value))
    {
        return value;
    }

    NWN2Mod::Log("NWNXGetInt: no plugin registered for '{}'.", plugin);
    return 0;
}

float __cdecl NWN2Mod::NWNXGetFloat(const char* plugin, const char* function, const char* param1, int param2)
{
    float value = 0.0f;
    IPlugin* target = Current->_PluginManager.FindById(plugin);
    if (target && target->OnNWNXGetFloat(function, param1, param2, value))
    {
        return value;
    }

    NWN2Mod::Log("NWNXGetFloat: no plugin registered for '{}'.", plugin);
    return 0.0f;
}

void __fastcall NWN2Mod::HookInitializeNetLayer(void* pThis, bool param)
{
    NWN2Mod::Log("InitializeNetLayer called.");

    // Here it's safe to initialize all the modules where they can read the commands
    Current->FinishInitialization();

    _InitializeNetLayer(pThis, param);
}

void __fastcall NWN2Mod::HookInitializeCommands(void* pThis)
{
    NWN2Mod::Log("InitializeCommands called.");

    // Save off "this" as it's a global variable we can use to access the commands later.
    NWN2Mod::Current->_NWVirtualMachineCommands = (CNWVirtualMachineCommands *)pThis;

    _InitializeCommands(pThis);
}

bool __fastcall NWN2Mod::HookSetBinaryData(
    void* pThisCampaignDB,
    CExoString* pCampNameExoStr,
    CExoString* pVarNameExoStr,
    CExoString* pPlayerExoStr,
    DataBlock* pDataBlock,
    uint16_t varType)
{
    NWN2Mod::Log("SetBinaryData called.");

    // The campaign name is the plugin selector: StoreCampaignObject("MyPlugin", varName, obj,
    // player) routes straight to the plugin whose GetPluginId() is "MyPlugin". CExoString's
    // buffer is null-terminated, so it can be handed to the plugin as-is.
    //
    // Never call through to the real SetBinaryData: that would write into NWN2Server's own
    // campaign DB cache (and eventually its SQLite file), which we don't want under any
    // circumstances. The plugin's own return value becomes StoreCampaignObject's result to the
    // calling script, so it's fully in charge of that; an unclaimed campaign name reports false,
    // since nothing stored anything.
    //
    // varType is part of the real SetBinaryData's signature (kept here so this hook's own ABI
    // matches what the engine calls), but it's dead at every known call site, so it's not
    // forwarded to the plugin to avoid representing it as meaningful data.
    IPlugin* plugin = Current->_PluginManager.FindById(pCampNameExoStr->m_sString);
    if (!plugin)
    {
        return false;
    }

    return plugin->OnSetBinaryData(pVarNameExoStr->m_sString, pPlayerExoStr->m_sString,
        pDataBlock->m_data, (size_t)pDataBlock->m_used);
}

namespace
{
    /// <summary>
    /// Host-side implementation of <see cref="IBinaryDataResult"/>, passed to a plugin's
    /// <c>OnGetBinaryData</c> by reference. Owns the <see cref="DataBlock"/> the plugin allocates
    /// into, if it does.
    /// </summary>
    /// <remarks>
    /// <see cref="Allocate"/> fabricates a <c>std::shared_ptr&lt;DataBlock&gt;</c> control block
    /// via <c>std::make_shared</c>, compiled entirely in this DLL, and hands it to the engine
    /// through <c>pRetVal</c> (see <see cref="NWN2Mod::HookGetBinaryData"/>). This matches the
    /// real engine's own <c>CCampaignDB::GetBinaryData</c>, which builds its return value the same
    /// way; see <see cref="DataBlock"/> for how its allocation matches the real one too.
    /// </remarks>
    class BinaryDataResult : public IBinaryDataResult
    {
    public:
        uint8_t* Allocate(size_t size) override
        {
            _block = std::make_shared<DataBlock>();
            _block->m_used = size;
            _block->m_allocated = size;
            _block->m_owning = true;
            _block->m_data = size ? static_cast<uint8_t*>(std::malloc(size)) : nullptr;

            return _block->m_data;
        }

        void Clear() override
        {
            _block.reset();
        }

        bool Handled() const { return (bool)_block; }
        std::shared_ptr<DataBlock> Take() { return std::move(_block); }

    private:
        std::shared_ptr<DataBlock> _block;
    };
}

DataBlockPtr* __fastcall NWN2Mod::HookGetBinaryData(
    void* pThisCampaignDB,
    DataBlockPtr* pRetVal,
    CExoString* pCampNameExoStr,
    CExoString* pVarNameExoStr,
    CExoString* pPlayerExoStr)
{
    NWN2Mod::Log("GetBinaryData called.");

    // The campaign name is the plugin selector, same as HookSetBinaryData.
    IPlugin* plugin = Current->_PluginManager.FindById(pCampNameExoStr->m_sString);
    if (plugin)
    {
        BinaryDataResult result;
        plugin->OnGetBinaryData(pVarNameExoStr->m_sString, pPlayerExoStr->m_sString, result);

        if (result.Handled())
        {
            // DataBlockPtr mirrors std::shared_ptr<DataBlock>'s layout exactly, so constructing
            // the real shared_ptr directly into the caller-supplied storage at pRetVal is exactly
            // what the compiler-generated hidden-return-value thunk would do for a genuine
            // std::shared_ptr<DataBlock> return value.
            new (pRetVal) std::shared_ptr<DataBlock>(result.Take());

            return pRetVal;
        }
    }

    // Never call through to the real GetBinaryData: on a cache miss it would load from
    // NWN2Server's own SQLite-backed campaign DB, which we don't want under any circumstances -
    // claimed by a plugin or not. Hand back an empty (null) shared_ptr instead; this is always
    // safe regardless of shared_ptr ABI, since a null shared_ptr never touches its control block.
    pRetVal->m_pDataBlock = nullptr;
    pRetVal->m_pRefCount = nullptr;

    return pRetVal;
}

std::expected<void*, std::string> NWN2Mod::FindInitializeCommands()
{
    // std::string sig = "48 89 10 48 89 50 08 49 8D 50 ?? 48 8B 02 48 8D 0D ?? ?? ?? ?? 48 89 08 48 8B 02 48 8D 0D ?? ?? ?? ?? 48 89 48 08";
    // This pattern matches this opcode pattern here:
    // 1407530f9 48 89 10        MOV        qword ptr [RAX],RDX
    // 1407530fc 48 89 50 08     MOV        qword ptr[RAX + 0x8], RDX
    // 140753100 49 8d 50 18     LEA        RDX, [R8 + 0x18]
    // 140753104 48 8b 02        MOV        RAX, qword ptr[RDX]
    // 140753107 48 8d 0d        LEA        this, [CNWVirtualMachineCommands::ExecuteComman
    // 22 7a 00 00
    // 14075310e 48 89 08        MOV        qword ptr[RAX], this = > CNWVirtualMachineCommand
    // 140753111 48 8b 02        MOV        RAX, qword ptr[RDX]
    // 140753114 48 8d 0d        LEA        this, [CNWVirtualMachineCommands::ExecuteComman
    // a5 7a 00 00
    // 14075311b 48 89 48 08     MOV        qword ptr[RAX + 0x8], this = > CNWVirtualMachineC

    // The idea is, this is a unique area of code where the entire m_pVirtualMachineCommands array is initialized. Finding this location
    // gives us access to ALOT of the virtual machine and functions in the NWN2Server64.exe. For one, the table is a pointer to every
    // script function that can be called, but it only gets initialized programmatically after the server is running.
    // This entire section of code has the function pointers, so we COULD read them all here, but that would be a lot of iterating for
    // little gain. Better to save it's pointer.


    // This sig is for the START of the function.
    std::string sig = "48 89 5C 24 20 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC B0 00 00 00 48 8B C1 33 C9 88 48 10 88 0D ?? ?? ?? ?? 88 0D ?? ?? ?? ??";

    return PEPattern::FindPattern(L"NWN2Server64.exe", sig);
}

std::expected<void*, std::string> NWN2Mod::FindInitializeNetLayer()
{
    // This is the byte pattern for the beginning of the CServerExoAppInternal::InitializeNetLayer. This is called
    // right after the InitializeCommands. So this is a safe place to read the m_pVirtualMachineCommands member with all the
    // functions in it.
    
    std::string pattern = "48 89 5C 24 20 88 54 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC 00 01 00 00 33 C0 48 89 45 8F 89 45 97 41 B8 F9 01 00 00 48 8D 15 ?? ?? ?? ??";
    return PEPattern::FindPattern(L"NWN2Server64.exe", pattern);
}

std::expected<void*, std::string> NWN2Mod::FindSetBinaryData()
{
    // This is the byte pattern for the beginning of the CCampaignDB::SetBinaryData. This is hooked so we can store
    // binary objects how we see fit and restore them.

    std::string pattern = "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 4D 8B E1 49 8B F8 48 8B F2 4C 8B E9 48 8D 4D ?? E8 ?? ?? ?? ??";
    return PEPattern::FindPattern(L"NWN2Server64.exe", pattern);
}

std::expected<void*, std::string> NWN2Mod::FindGetBinaryData()
{
    // This is the byte pattern for the beginning of CCampaignDB::GetBinaryData. This is hooked so we can
    // observe (and eventually intercept) binary campaign object retrieval, the counterpart to SetBinaryData.
    //
    // This is the function's prologue (identical, aside from the wildcarded bytes noted below,
    // between the reference build and the current build):
    // 48 89 5C 24 10        MOV    [RSP+0x10], RBX
    // 4C 89 4C 24 20        MOV    [RSP+0x20], R9
    // 55                    PUSH   RBP
    // 56                    PUSH   RSI
    // 57                    PUSH   RDI
    // 41 54                 PUSH   R12
    // 41 55                 PUSH   R13
    // 41 56                 PUSH   R14
    // 41 57                 PUSH   R15
    // 48 8D 6C 24 90        LEA    RBP, [RSP-0x70]      (offset wildcarded)
    // 48 81 EC 70 01 00 00  SUB    RSP, 0x170            (immediate wildcarded)
    // 0F 29 B4 24 60 01 00 00  MOVAPS [RSP+0x160], XMM6  (offset wildcarded)
    // 4D 8B F9              MOV    R15, R9
    // 4D 8B E0              MOV    R12, R8
    // 4C 8B F2              MOV    R14, RDX
    // 48 8B F9              MOV    RDI, RCX
    // 49 8B D0              MOV    RDX, R8
    // 48 8D 4D A8           LEA    RCX, [RBP-0x58]       (offset wildcarded)
    // E8 ?? ?? ?? ??        CALL   CExoString copy ctor  (displacement wildcarded, target differs per build)
    //
    // The register shuffle (R15/R12/R14/RDI/RDX) is the parameter marshalling for the
    // (this, retval-shared_ptr, CExoString*, CExoString*, CExoString*) signature and is kept fixed since
    // it reflects the calling convention, not compiler-specific frame layout.
    std::string pattern = "48 89 5C 24 10 4C 89 4C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 4D 8B F9 4D 8B E0 4C 8B F2 48 8B F9 49 8B D0 48 8D 4D ?? E8 ?? ?? ?? ??";
    return PEPattern::FindPattern(L"NWN2Server64.exe", pattern);
}


std::expected<void*, std::string> NWN2Mod::FindVirtualMachineWrite()
{
    // This is the byte pattern for the section of StartServices that writes the g_pVirtualMachine global. The pattern
    // ends at the TEST preceding that write, as the instructions between there and the write differ between builds.
    // Extending the pattern past this point will break the match.

    std::string pattern = "41 B8 32 12 00 00 48 8D 15 ?? ?? ?? ?? B9 08 05 00 00 FF 15 ?? ?? ?? ?? 48 89 85 ?? ?? ?? ?? 48 85 C0";

    auto result = PEPattern::FindPattern(L"NWN2Server64.exe", pattern);
    if (!result)
    {
        return std::unexpected(result.error());
    }

    uint8_t* anchor = (uint8_t*)result.value();

    // The write itself is "48 89 05 <disp32>" (MOV [rip+disp32], RAX). We scan forward for that opcode rather than
    // stepping a fixed distance from the anchor, as the distance shifts between builds. It is the only occurrence
    // within the search window, so there is no risk of landing on the wrong instruction.
    constexpr uint8_t STORE_OPCODE[] = { 0x48, 0x89, 0x05 };
    constexpr size_t  STORE_LENGTH   = 7;     // opcode (3) + disp32 (4)
    constexpr size_t  SEARCH_LIMIT   = 0x50;

    size_t storeOffset = 0;
    for (; storeOffset < SEARCH_LIMIT; ++storeOffset)
    {
        if (std::memcmp(anchor + storeOffset, STORE_OPCODE, sizeof(STORE_OPCODE)) == 0)
        {
            break;
        }
    }

    if (storeOffset >= SEARCH_LIMIT)
    {
        return std::unexpected(std::format("Found g_pVirtualMachine anchor at 0x{:016X}, but no write instruction within 0x{:X} bytes", (uint64_t)anchor, SEARCH_LIMIT));
    }

    auto absoluteAddress = ExtractRipRelativeAddress((uintptr_t)(anchor + storeOffset), 0, 3, STORE_LENGTH);

    return (void *)absoluteAddress;
}

std::expected<void*, std::string> NWN2Mod::FindRunScript()
{
    // This is the byte pattern for CVirtualMachine::RunScript(CExoString*, unsigned long, int,
    // PARAMTER_VALIDATION) - the 4-argument convenience overload that always runs against the
    // global g_pVirtualMachine (rather than a caller-supplied CVirtualMachine*, and with no extra
    // script parameters). It is a thin wrapper that just forwards to the real worker function:
    //
    // 48 83 EC 48              SUB    RSP,0x48
    // 33 C0                    XOR    EAX,EAX
    // 48 89 44 24 38           MOV    [RSP+0x38],RAX      ; zero the temp (empty) parameter array
    // 48 89 44 24 30           MOV    [RSP+0x30],RAX
    // 8B 44 24 70              MOV    EAX,[RSP+0x70]      ; reload this function's own 5th (stack) arg
    // 89 44 24 28              MOV    [RSP+0x28],EAX      ; ...onto the worker's 5th arg slot
    // 44 89 4C 24 20           MOV    [RSP+0x20],R9D      ; this function's own 4th arg (register)...
    // 4C 8D 4C 24 30           LEA    R9,[RSP+0x30]       ; ...goes to the worker's 4th arg slot, so R9 is now free to hold &tempArray
    // 48 8B 0D ?? ?? ?? ??     MOV    RCX,[g_pVirtualMachine]  (RIP-relative; displacement wildcarded, shifts between builds)
    // E8 ?? ?? ?? ??           CALL   RunScript worker         (displacement wildcarded, target differs per build)
    //
    // The function's first parameter is a dead "this" slot: it's clobbered by the MOV RCX,
    // [g_pVirtualMachine] above before ever being read, which is how a plain, non-member call
    // into it is possible at all.
    std::string pattern = "48 83 EC 48 33 C0 48 89 44 24 38 48 89 44 24 30 8B 44 24 70 89 44 24 28 44 89 4C 24 20 4C 8D 4C 24 30 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 83 C4 48 C3";
    return PEPattern::FindPattern(L"NWN2Server64.exe", pattern);
}

uintptr_t  NWN2Mod::ExtractRipRelativeAddress(uintptr_t functionAddress,
    size_t instructionOffset,
    size_t displacementOffset,
    size_t instructionLength)
{
    // 1. Calculate the absolute address of the *next* instruction (the RIP base used by the CPU)
    uintptr_t nextInstructionRIP = functionAddress + instructionOffset + instructionLength;

    // 2. Read the 4-byte signed displacement offset from the function memory space
    int32_t relativeDisplacement = 0;
    std::memcpy(&relativeDisplacement, reinterpret_cast<void*>(functionAddress + displacementOffset), sizeof(int32_t));

    // 3. The absolute address is Next RIP + Signed Displacement
    return nextInstructionRIP + relativeDisplacement;
}

void NWN2Mod::MapNWNXFunctions()
{
    struct Func
    {
        size_t functionId;
        size_t instructionOffset;
        size_t displacementOffset;
        size_t opcodeSize;
        void *function;
    };

    Func funcs[] = {
        { Commands::COMMAND_NWNXSETSTRING,  0x3AC,  0x3AF,  7,  (void*)&NWN2Mod::NWNXSetString },
        { Commands::COMMAND_NWNXSETINT,     0x305,  0x308,  7,  (void*)&NWN2Mod::NWNXSetInt },
        { Commands::COMMAND_NWNXSETFLOAT,   0x305,  0x308,  7,  (void*)&NWN2Mod::NWNXSetFloat },
        { Commands::COMMAND_NWNXGETSTRING,  0x2C0,  0x2C3,  7,  (void*)&NWN2Mod::NWNXGetString },
        { Commands::COMMAND_NWNXGETINT,     0x2B8,  0x2BB,  7,  (void*)&NWN2Mod::NWNXGetInt },
        { Commands::COMMAND_NWNXGETFLOAT,   0x2B8,  0x2BB,  7,  (void*)&NWN2Mod::NWNXGetFloat },
    };

    for (size_t i = 0; i < 6; ++i)
    {
        auto functionAddress = (uintptr_t)_NWVirtualMachineCommands->m_pVirtualMachineCommands[funcs[i].functionId];

        // Distance calculations from function start (0x1407bbda0) to opcode locations
        uintptr_t hookAddress = ExtractRipRelativeAddress(
            functionAddress,
            funcs[i].instructionOffset, // 1407bc14c - 1407bbda0
            funcs[i].displacementOffset,// 1407bc14f - 1407bbda0
            funcs[i].opcodeSize
        );

        void** ppHookAddress = reinterpret_cast<void**>(hookAddress);


        if (((uintptr_t)*ppHookAddress) != 0)
        {
            _Logger->log("Invalid address found.");
        }
        else
        {
            *ppHookAddress = funcs[i].function;
        }
    }
}
void NWN2Mod::FinishInitialization()
{
    MapNWNXFunctions();

    // Fixup Virtual Machine
    _VirtualMachine = *((void **)_VirtualMachine);
}

bool NWN2Mod::RunScript(const char* script, uint32_t objectId) const
{
    if (!script || !*script || !_VirtualMachine)
    {
        return false;
    }

    // CVirtualMachine::RunScript's operator= call only ever reads m_sString (to strlen() and copy
    // it) - it never reads or writes m_nBufferLength, so leaving it at 0 here is safe.
    CExoString scriptName{ const_cast<char*>(script), 0 };

    return _RunScript(nullptr, &scriptName, objectId, 0, 0) != 0;
}

ChatHookFunc NWN2Mod::RegisterChatHook(ChatHookFunc hook)
{
    ChatHookFunc previous = _ChatHook;
    _ChatHook = hook;
    return previous;
}

int __fastcall NWN2Mod::HookSendServerToPlayerChatMessage(
    void* pThis,
    uint8_t mode,
    uint32_t senderId,
    CExoString* message,
    uint32_t targetId,
    void* clientList,
    CExoString* extraMessage,
    bool runScriptFlag)
{
    // A registered hook fully owns the decision to suppress; if it declines (or none is
    // registered), fall through to the real function untouched - all six parameters, including
    // the ones no plugin ever sees, are forwarded exactly as NWN2 passed them in.
    if (_ChatHook && _ChatHook(mode, senderId, message->m_sString, targetId))
    {
        return 1;
    }

    return _SendServerToPlayerChatMessage(pThis, mode, senderId, message, targetId, clientList, extraMessage, runScriptFlag);
}

std::expected<void*, std::string> NWN2Mod::FindSendServerToPlayerChatMessage()
{
    // This is the byte pattern for the beginning of CNWSMessage::SendServerToPlayerChatMessage -
    // the single function every chat message (Talk/Shout/Whisper/Tell/Party and their DM variants)
    // funnels through before NWN2 sends it to any client, which is what makes it the right place
    // to intercept chat for IPluginHost::RegisterChatHook.
    //
    // 48 8B C4                 MOV    RAX,RSP
    // 48 89 58 18              MOV    [RAX+0x18],RBX
    // 88 50 10                 MOV    byte ptr [RAX+0x10],DL    ; mode (2nd arg)
    // 48 89 48 08              MOV    [RAX+0x8],RCX             ; this (1st arg)
    // 55 56 57 41 54 41 55 41 56 41 57   PUSH RBP/RSI/RDI/R12/R13/R14/R15
    // 48 8D 68 ??              LEA    RBP,[RAX-0x3F]            (offset wildcarded)
    // 48 81 EC ?? ?? ?? ??     SUB    RSP,0xA0                  (immediate wildcarded)
    // 0F 29 70 ??              MOVAPS [RAX-0x48],XMM6           (offset wildcarded)
    // 0F 29 78 ??              MOVAPS [RAX-0x58],XMM7           (offset wildcarded)
    // 4D 8B E1                 MOV    R12,R9                    ; message (4th arg)
    // 41 8B F0                 MOV    ESI,R8D                   ; senderId (3rd arg)
    // 0F B6 DA                 MOVZX  EBX,DL
    // 4C 8B F9                 MOV    R15,RCX
    // BF 01 00 00 00           MOV    EDI,0x1
    // 48 8B 0D ?? ?? ?? ??     MOV    RCX,[g_pAppManager]       (RIP-relative; displacement wildcarded)
    std::string pattern = "48 8B C4 48 89 58 18 88 50 10 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 0F 29 70 ?? 0F 29 78 ?? 4D 8B E1 41 8B F0 0F B6 DA 4C 8B F9 BF 01 00 00 00 48 8B 0D ?? ?? ?? ??";
    return PEPattern::FindPattern(L"NWN2Server64.exe", pattern);
}